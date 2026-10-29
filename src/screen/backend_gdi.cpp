// 屏幕域 GDI 兜底后端（F20）—— 一个实现服务两类目标：
//   显示器：CreateDCA(设备名) + BitBlt(SRCCOPY | CAPTUREBLT) 直进 32bpp top-down
//           DIB（GetDC(NULL)+偏移 的等价物；按输出设备名取 DC，多屏各自原点）
//   窗口：  GetWindowRect 尺寸 + PrintWindow(PW_RENDERFULLCONTENT)（直取 DWM
//           内容，硬件加速窗口大部分兼容）；失败再试 BitBlt(GetWindowDC)
// 语义契约（F14，技术选型 §10.5）：轮询后端定时出帧（静止 = 重复帧）——
// 不做变化检测（对 4K 帧 ~33MB/帧的逐像素比对不现实）。
// 轮询节奏 = fps（会话注入；0 → 默认 30fps），分块 ≤100ms 可中断等待（stop 延迟
// 有界）。窗口 resize 每回合查 GetWindowRect，尺寸变化即重建 DIB（F19 帧跟随）。

#include "screen_session.h"

#include <new>

namespace sparkcap {
namespace {

// GDI 轮询默认节拍（fps=0 时）：30fps
const int64_t kDefaultPollInterval100ns = 10000000LL / 30;
// 分块等待上限（ms）：stop 置位 wakeEvent 时快速让出
const DWORD kPaceChunkMs = 100;

int64_t gdiNow100ns()
{
    LARGE_INTEGER freq = {};
    LARGE_INTEGER now = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0)
        return 0;
    return static_cast<int64_t>((now.QuadPart * 10000000LL) / freq.QuadPart);
}

// GDI 兜底后端（会话层经 IScreenBackend 驱动；采集线程独占访问）
class ScreenBackendGdi final : public IScreenBackend {
public:
    ScreenBackendGdi(const ScreenTarget& target, const Host& host, int fps)
        : m_isWindow(target.kind == ScreenTarget::Kind::Window)
        , m_hwnd(target.hwnd)
        , m_displayId(target.displayId)
        , m_host(host)
        , m_interval((fps > 0) ? 10000000LL / fps : kDefaultPollInterval100ns)
    {
    }

    int start() override;
    ScreenCapResult captureNext(DWORD waitMs) override;  // waitMs 忽略：自管节拍
    const void* frameData() const override { return m_bits; }
    int frameWidth() const override { return m_width; }
    int frameHeight() const override { return m_height; }
    void stop() override;

private:
    bool createDib(int width, int height);  // 32bpp top-down DIB（重建=resize 跟随）
    bool captureDisplay();
    bool captureWindow();
    void pace();  // 节拍等待（分块，wakeEvent 可中断）

    bool     m_isWindow;
    uint64_t m_hwnd;
    int      m_displayId;
    Host     m_host;
    int64_t  m_interval;      // 轮询间隔（100ns）
    int64_t  m_nextTick = 0;  // 下一采集时刻（100ns）

    HDC     m_screenDc = nullptr;  // 显示器：输出设备 DC（start 建，stop 释放）
    HDC     m_memDc = nullptr;     // DIB 选入的内存 DC
    HBITMAP m_dib = nullptr;
    void*   m_bits = nullptr;      // DIB 位直接作为帧数据（stride = width*4，top-down）
    int     m_width = 0;
    int     m_height = 0;

    wchar_t m_deviceName[32] = {};  // 显示器设备名（\\.\DISPLAY1）
};

bool ScreenBackendGdi::createDib(int width, int height)
{
    if (m_memDc != nullptr) {
        DeleteDC(m_memDc);
        m_memDc = nullptr;
    }
    if (m_dib != nullptr) {
        DeleteObject(m_dib);
        m_dib = nullptr;
    }
    m_bits = nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // 负高 = top-down（stride 正，与 DXGI 路径一致）
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr)
            DeleteObject(dib);
        return false;
    }
    HDC screen = GetDC(nullptr);
    m_memDc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (m_memDc == nullptr) {
        DeleteObject(dib);
        return false;
    }
    SelectObject(m_memDc, dib);
    m_dib = dib;
    m_bits = bits;
    m_width = width;
    m_height = height;
    return true;
}

int ScreenBackendGdi::start()
{
    if (m_isWindow) {
        const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
        RECT wr = {};
        if (hwnd == nullptr || !GetWindowRect(hwnd, &wr))
            return SPARKCAP_ERR_NOT_FOUND;
        const int width = wr.right - wr.left;
        const int height = wr.bottom - wr.top;
        if (width <= 0 || height <= 0 || !createDib(width, height))
            return SPARKCAP_ERR_START_FAILED;
    } else {
        RECT desktop = {};
        int width = 0;
        int height = 0;
        if (!screenLocateDisplay(m_displayId, m_deviceName, &desktop, &width, &height))
            return SPARKCAP_ERR_NOT_FOUND;
        if (!createDib(width, height))
            return SPARKCAP_ERR_START_FAILED;
        // 输出设备 DC（原点即该显示器左上角；失败 → START_FAILED，会话层
        // 已无双后端可落 → 返回主后端错误码）
        m_screenDc = CreateDCW(m_deviceName, nullptr, nullptr, nullptr);
        if (m_screenDc == nullptr)
            return SPARKCAP_ERR_START_FAILED;
    }
    m_nextTick = gdiNow100ns();  // 立即采首帧
    return SPARKCAP_ERR_OK;
}

void ScreenBackendGdi::stop()
{
    if (m_memDc != nullptr) {
        DeleteDC(m_memDc);
        m_memDc = nullptr;
    }
    if (m_dib != nullptr) {
        DeleteObject(m_dib);
        m_dib = nullptr;
    }
    if (m_screenDc != nullptr) {
        DeleteDC(m_screenDc);
        m_screenDc = nullptr;
    }
    m_bits = nullptr;
    m_width = 0;
    m_height = 0;
}

void ScreenBackendGdi::pace()
{
    for (;;) {
        const int64_t remain = m_nextTick - gdiNow100ns();
        if (remain <= 0)
            return;
        const DWORD ms = (remain > static_cast<int64_t>(kPaceChunkMs) * 10000)
            ? kPaceChunkMs
            : static_cast<DWORD>((remain + 9999) / 10000);
        if (WaitForSingleObject(m_host.wakeEvent, ms) != WAIT_TIMEOUT)
            return;  // stop 唤醒：让出（会话循环顶复查 running）
    }
}

// 显示器：BitBlt 直进 DIB（CAPTUREBLT 含分层窗口；不含鼠标光标——光标层
// 独立于桌面位图，与 DXGI 路径语义一致）
bool ScreenBackendGdi::captureDisplay()
{
    if (m_screenDc == nullptr || m_memDc == nullptr)
        return false;
    return BitBlt(m_memDc, 0, 0, m_width, m_height, m_screenDc, 0, 0,
                  SRCCOPY | CAPTUREBLT) != FALSE;
}

// 窗口：PrintWindow(RENDERFULLCONTENT) 优先（DWM 内容，硬件加速窗口兼容），
// 失败再试 BitBlt(GetWindowDC)（仅 GDI 窗口可靠，最后手段）。
// resize：每回合查 GetWindowRect，尺寸变化即重建 DIB（F19 帧跟随）。
bool ScreenBackendGdi::captureWindow()
{
    const HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    RECT wr = {};
    if (hwnd == nullptr || !GetWindowRect(hwnd, &wr))
        return false;
    const int width = wr.right - wr.left;
    const int height = wr.bottom - wr.top;
    if (width <= 0 || height <= 0)
        return false;
    if (width != m_width || height != m_height) {
        if (!createDib(width, height))
            return false;
    }
    if (m_memDc == nullptr)
        return false;
    if (PrintWindow(hwnd, m_memDc, PW_RENDERFULLCONTENT) != FALSE)
        return true;
    const HDC wndDc = GetWindowDC(hwnd);
    if (wndDc == nullptr)
        return false;
    const BOOL ok = BitBlt(m_memDc, 0, 0, width, height, wndDc, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, wndDc);
    return ok != FALSE;
}

ScreenCapResult ScreenBackendGdi::captureNext(DWORD)
{
    pace();
    const bool ok = m_isWindow ? captureWindow() : captureDisplay();
    m_nextTick = gdiNow100ns() + m_interval;
    return ok ? ScreenCapResult::Frame : ScreenCapResult::Idle;  // 失败下回合重试
}

} // namespace

std::unique_ptr<IScreenBackend> makeGdiBackend(const ScreenTarget& target,
                                               const IScreenBackend::Host& host, int fps)
{
    try {
        return std::make_unique<ScreenBackendGdi>(target, host, fps);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sparkcap
