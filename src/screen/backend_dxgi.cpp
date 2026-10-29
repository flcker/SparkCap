// 屏幕域 DXGI Desktop Duplication 后端 —— 显示器枚举（F12）+ 定位 + 采集（F13–F15）。
// 枚举：IDXGIFactory1::EnumAdapters1 → EnumOutputs（AttachedToDesktop），
//       displayId = adapterIdx*16 + outputIdx（稳定序号，open/start 时重枚举匹配）。
// 采集：AcquireNextFrame（静止桌面 WAIT_TIMEOUT 继续为正常态），成功帧
//       CopyResource 到 staging → Map 逐行拷出（RowPitch 可能大于 width*4）→
//       立即 ReleaseFrame（系统缓冲有限）；
//       ACCESS_LOST（模式切换/独占全屏/TDR）：经 host 错误回调通知（STREAM_LOST）
//       + 重建 duplication（1s 退避重试，stop 经 wakeEvent 可中断；
//       显示器拔除判定流终结 Lost）。
// 旋转屏（F13 保真）：duplication 交付的桌面纹理恒为面板原生方向（landscape），
//       DXGI_OUTDUPL_DESC.ModeDesc 则是旋转后的桌面尺寸（竖），Rotation 指示
//       旋转方向——库须按 Rotation 把纹理旋转成屏幕所见（实测竖屏 ModeDesc
//       2160x3840 / 纹理 3840x2160 / Rotation=ROTATE270）。输出帧尺寸因此
//       与枚举口径（DesktopCoordinates 物理像素）方向一致。
// start 失败（远程桌面/虚拟显卡/他进程占用）由会话层回落 GDI（技术选型 §10.5）。

#include "screen_session.h"
#include "common/str_util.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstring>
#include <new>
#include <utility>

namespace sparkcap {
namespace {

using Microsoft::WRL::ComPtr;

// AcquireNextFrame 轮询超时：静止桌面期间周期性让出以观察 stop
const UINT kAcquireTimeoutMs = 200;
// ACCESS_LOST 重建退避间隔（stop 经 wakeEvent 可中断）
const DWORD kRebuildBackoffMs = 1000;

// 枚举条目：output/adapter 接口 + 输出描述（displayId 由枚举序编码）
struct OutputItem {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1>  output;
    DXGI_OUTPUT_DESC      desc = {};
    int                   displayId = 0;
};

// 枚举挂接桌面的输出。返回 false 仅当 DXGI 工厂创建失败（无输出不算失败）。
bool enumOutputs(std::vector<OutputItem>& out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.GetAddressOf()))))
        return false;

    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;  // 适配器枚举完
        if (adapter == nullptr)
            continue;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
                break;  // 输出枚举完
            if (output == nullptr)
                continue;
            DXGI_OUTPUT_DESC desc = {};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop)
                continue;  // 未挂接桌面的输出不可 duplication
            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1)))
                continue;
            OutputItem item;
            item.adapter = adapter;              // 拷贝（AddRef）：跨内层循环复用
            item.output = std::move(output1);
            item.desc = desc;
            item.displayId = static_cast<int>(a) * 16 + static_cast<int>(o);
            out.push_back(std::move(item));
        }
    }
    return true;
}

// 显示器整屏捕获后端（会话层经 IScreenBackend 驱动；采集线程独占访问）
class ScreenBackendDxgi final : public IScreenBackend {
public:
    ScreenBackendDxgi(int displayId, const Host& host)
        : m_displayId(displayId)
        , m_host(host)
    {
    }

    int start() override;
    ScreenCapResult captureNext(DWORD waitMs) override;
    const void* frameData() const override { return m_buffer.data(); }
    int frameWidth() const override { return m_width; }
    int frameHeight() const override { return m_height; }
    void stop() override;

private:
    int  createChain();      // 定位 → 设备 → duplication → staging
    void releaseChain();
    int  rebuildChain();     // ACCESS_LOST 重建（重定位，拓扑/模式已可能变化）
    bool recoverFromLoss();  // 通知（host）+ 退避重建；false=stop 或流终结
    bool copyFrame(ComPtr<IDXGIResource> desktop);
    bool recreateStaging(UINT width, UINT height);  // 读回通道按实际纹理尺寸对齐

    int m_displayId;
    Host m_host;

    ComPtr<ID3D11Device>           m_device;
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D>        m_staging;
    DXGI_MODE_ROTATION m_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    int m_stageW = 0;        // staging/纹理尺寸（面板原生方向）
    int m_stageH = 0;
    int m_width = 0;         // 输出帧尺寸（Rotation 旋转后 = 屏幕所见方向）
    int m_height = 0;
    std::vector<char> m_buffer;  // 连续 BGRA（RGB32，库分配）
};

// 建立采集链：重枚举定位 output → D3D 设备（挂 output 所属适配器——
// DuplicateOutput 要求同适配器；失败回退 WARP）→ DuplicateOutput → staging。
int ScreenBackendDxgi::createChain()
{
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    std::vector<OutputItem> items;
    if (!enumOutputs(items))
        return SPARKCAP_ERR_INTERNAL;
    for (auto& item : items) {
        if (item.displayId != m_displayId)
            continue;
        adapter = item.adapter;
        output = item.output;
        break;
    }
    if (output == nullptr)
        return SPARKCAP_ERR_NOT_FOUND;  // 显示器已拔除/拓扑变化后无此 id

    DXGI_OUTPUT_DESC desc = {};
    if (FAILED(output->GetDesc(&desc)))
        return SPARKCAP_ERR_INTERNAL;

    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   m_device.ReleaseAndGetAddressOf(), nullptr,
                                   m_context.ReleaseAndGetAddressOf());
    if (FAILED(hr))  // 虚拟机/远程会话等无硬件路径：回退 WARP
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, m_device.ReleaseAndGetAddressOf(), nullptr,
                               m_context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        releaseChain();
        return SPARKCAP_ERR_START_FAILED;
    }

    hr = output->DuplicateOutput(m_device.Get(), m_duplication.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        releaseChain();
        return (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            ? SPARKCAP_ERR_DEVICE_BUSY  // 同屏已有 duplication（系统单实例，F13）
            : SPARKCAP_ERR_START_FAILED;  // 安全桌面/会话断开等
    }

    // 旋转语义：ModeDesc 是旋转后的桌面尺寸（竖），纹理恒为面板原生方向
    // （landscape，实测竖屏 ModeDesc 2160x3840 / 纹理 3840x2160）——staging
    // 按面板方向建（90/270 逆交换），输出帧尺寸由 recreateStaging 按 Rotation
    // 换算。DesktopCoordinates 不可用作尺寸：其 DPI 虚拟化行为不一致（非旋转
    // 屏返回逻辑坐标、旋转屏返回物理——Windows 旋转屏怪癖）。
    DXGI_OUTDUPL_DESC dupDesc = {};
    m_duplication->GetDesc(&dupDesc);
    m_rotation = dupDesc.Rotation;
    UINT stageW = dupDesc.ModeDesc.Width;
    UINT stageH = dupDesc.ModeDesc.Height;
    if (m_rotation == DXGI_MODE_ROTATION_ROTATE90 ||
        m_rotation == DXGI_MODE_ROTATION_ROTATE270)
        std::swap(stageW, stageH);  // ModeDesc 旋转后 → 面板原生方向
    if (stageW == 0 || stageH == 0) {
        releaseChain();
        return SPARKCAP_ERR_START_FAILED;
    }
    if (!recreateStaging(stageW, stageH)) {
        releaseChain();
        return SPARKCAP_ERR_START_FAILED;
    }

    // 丢弃首个 acquire：duplication 建立后首帧常为未初始化黑帧（实测 4K 桌面
    // 首帧 avg=0、后续帧正常）。丢弃后首个有效帧来自下一次桌面更新（鼠标
    // 移动亦触发 AcquireNextFrame 返回有效快照）；完全静止桌面回到 F14
    // 语义（TIMEOUT 无帧）。
    DXGI_OUTDUPL_FRAME_INFO initInfo = {};
    ComPtr<IDXGIResource> initRes;
    if (SUCCEEDED(m_duplication->AcquireNextFrame(100, &initInfo,
                                                  initRes.GetAddressOf())))
        m_duplication->ReleaseFrame();
    return SPARKCAP_ERR_OK;
}

// 读回通道按实际纹理尺寸（重新）创建：staging 与桌面纹理尺寸错配时
// CopyResource 静默失败（无返回值）→ 黑帧，必须在拷贝前对齐。
// 尺寸语义：入参为面板原生方向尺寸（纹理口径）；输出帧尺寸（m_width/height）
// 按 m_rotation 换算——90/270 交换（旋转后与 DesktopCoordinates 方向一致）。
bool ScreenBackendDxgi::recreateStaging(UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // 桌面原生 BGRA → RGB32（F9 保真）
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_staging.ReleaseAndGetAddressOf())))
        return false;
    m_stageW = static_cast<int>(width);
    m_stageH = static_cast<int>(height);
    if (m_rotation == DXGI_MODE_ROTATION_ROTATE90 ||
        m_rotation == DXGI_MODE_ROTATION_ROTATE270) {
        m_width = m_stageH;  // 旋转 90/270：输出交换宽高（屏幕所见方向）
        m_height = m_stageW;
    } else {
        m_width = m_stageW;
        m_height = m_stageH;
    }
    return true;
}

void ScreenBackendDxgi::releaseChain()
{
    m_duplication.Reset();
    m_staging.Reset();
    m_context.Reset();
    m_device.Reset();
}

int ScreenBackendDxgi::start()
{
    return createChain();
}

void ScreenBackendDxgi::stop()
{
    releaseChain();
    m_buffer.clear();
    m_buffer.shrink_to_fit();
    m_stageW = 0;
    m_stageH = 0;
    m_width = 0;
    m_height = 0;
}

ScreenCapResult ScreenBackendDxgi::captureNext(DWORD waitMs)
{
    if (m_duplication == nullptr)
        return ScreenCapResult::Lost;
    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> desktop;
    const HRESULT hr = m_duplication->AcquireNextFrame(waitMs, &info,
                                                       desktop.GetAddressOf());
    if (SUCCEEDED(hr)) {
        const bool ok = copyFrame(desktop);
        // 立即归还系统缓冲（数量有限：持有不 release 会让后续 acquire 假超时）
        m_duplication->ReleaseFrame();
        return ok ? ScreenCapResult::Frame : ScreenCapResult::Idle;
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return ScreenCapResult::Idle;  // 静止桌面：无新帧为正常状态（F14 语义）
    // ACCESS_LOST（模式切换/独占全屏/TDR）等运行期错误
    return recoverFromLoss() ? ScreenCapResult::Idle : ScreenCapResult::Lost;
}

// GPU 纹理 → staging → CPU：Map 后按 RowPitch 逐行拷出（行对齐时 RowPitch >
// width*4）；旋转屏按 Rotation 转置（交付屏幕所见方向，见文件头注释）。
bool ScreenBackendDxgi::copyFrame(ComPtr<IDXGIResource> desktop)
{
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(desktop.As(&tex)))
        return false;

    // 防御：显示模式动态变化时纹理尺寸与 staging 错配 → CopyResource 静默失败
    // （黑帧）。按纹理实际尺寸重建读回通道。
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    if (td.Width != static_cast<UINT>(m_stageW) || td.Height != static_cast<UINT>(m_stageH))
        if (!recreateStaging(td.Width, td.Height))
            return false;

    m_context->CopyResource(m_staging.Get(), tex.Get());  // GPU 纹理不能直出应用指针

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return false;

    m_buffer.assign(static_cast<size_t>(m_width) * 4 * static_cast<size_t>(m_height), 0);
    const char* src = static_cast<const char*>(map.pData);
    char* dst = m_buffer.data();
    if (m_rotation == DXGI_MODE_ROTATION_IDENTITY ||
        m_rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
        const int wbytes = m_width * 4;
        for (int row = 0; row < m_height; ++row)
            memcpy(dst + static_cast<size_t>(wbytes) * row,
                   src + static_cast<size_t>(map.RowPitch) * row,
                   static_cast<size_t>(wbytes));
    } else {
        // 旋转读出（uint32 像素，BGRA 通道序不变）。公式（src 尺寸 stageW×stageH）：
        //   ROTATE90  屏幕所见 = 纹理顺时针 90°：dst[r][c] = src[stageH-1-c][r]
        //   ROTATE270 屏幕所见 = 纹理逆时针 90°：dst[r][c] = src[c][stageW-1-r]
        //   ROTATE180 屏幕所见 = 纹理旋转 180°：dst[r][c] = src[stageH-1-r][stageW-1-c]
        const UINT pitch = map.RowPitch / 4;
        const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
        uint32_t* d = reinterpret_cast<uint32_t*>(dst);
        if (m_rotation == DXGI_MODE_ROTATION_ROTATE90) {
            for (int r = 0; r < m_height; ++r)
                for (int c = 0; c < m_width; ++c)
                    d[static_cast<size_t>(r) * m_width + c] =
                        s[static_cast<size_t>(m_stageH - 1 - c) * pitch + r];
        } else if (m_rotation == DXGI_MODE_ROTATION_ROTATE270) {
            for (int r = 0; r < m_height; ++r)
                for (int c = 0; c < m_width; ++c)
                    d[static_cast<size_t>(r) * m_width + c] =
                        s[static_cast<size_t>(c) * pitch + (m_stageW - 1 - r)];
        } else {  // ROTATE180（尺寸不交换）
            for (int r = 0; r < m_height; ++r)
                for (int c = 0; c < m_width; ++c)
                    d[static_cast<size_t>(r) * m_width + c] =
                        s[static_cast<size_t>(m_stageH - 1 - r) * pitch +
                        (m_stageW - 1 - c)];
        }
    }
    m_context->Unmap(m_staging.Get(), 0);
    return true;
}

// ACCESS_LOST 等运行期错误：错误回调（经 host，锁外）+ 重建（1s 退避，stop 可中断）。
// true=恢复；false=stop 或输出永久消失（Lost，通知已在进入时发过一次）。
bool ScreenBackendDxgi::recoverFromLoss()
{
    if (m_host.onError != nullptr)  // Spec §4.5：流中断通知；不通知恢复
        m_host.onError(m_host.ctx, SPARKCAP_ERR_STREAM_LOST);

    for (;;) {
        if (m_host.shouldStop != nullptr && m_host.shouldStop(m_host.ctx))
            return false;
        const int rc = rebuildChain();
        if (rc == SPARKCAP_ERR_OK)
            return true;
        if (rc == SPARKCAP_ERR_NOT_FOUND)
            return false;  // 显示器已拔除/拓扑变化后无此 id：流终结
        // 其余失败（如被他进程占用 DEVICE_BUSY）继续退避重试
        WaitForSingleObject(m_host.wakeEvent, kRebuildBackoffMs);
    }
}

// 重建采集链：释放后重枚举定位（拓扑/模式可能已变化）。
// NOT_FOUND 表示输出已消失（不可恢复）；其余失败可重试。
int ScreenBackendDxgi::rebuildChain()
{
    releaseChain();
    return createChain();
}

} // namespace

// ---- 枚举（F12）----

int screenListDisplays(std::vector<sparkcap_display_info_t>& out)
{
    std::vector<OutputItem> items;
    if (!enumOutputs(items))
        return SPARKCAP_ERR_INTERNAL;

    // 主屏判定：包含 (0,0) 的显示器
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    for (const auto& item : items) {
        sparkcap_display_info_t info = {};
        info.id = item.displayId;
        // name：EnumDisplayDevices 监视器友好名，失败回退 GDI 设备名（\\.\DISPLAY1）
        DISPLAY_DEVICEW mon = {};
        mon.cb = sizeof(mon);
        const wchar_t* name = item.desc.DeviceName;
        if (EnumDisplayDevicesW(item.desc.DeviceName, 0, &mon, 0) && mon.DeviceString[0] != L'\0')
            name = mon.DeviceString;
        toUtf8(name, info.name, sizeof(info.name));
        info.width = item.desc.DesktopCoordinates.right - item.desc.DesktopCoordinates.left;
        info.height = item.desc.DesktopCoordinates.bottom - item.desc.DesktopCoordinates.top;
        info.is_primary = (item.desc.Monitor == primary) ? 1 : 0;
        out.push_back(info);
    }
    return SPARKCAP_ERR_OK;
}

// ---- 显示器定位（DXGI 链与 GDI 兜底共用）----

bool screenLocateDisplay(int displayId, wchar_t deviceName[32], RECT* desktopRect,
                         int* width, int* height)
{
    if (deviceName == nullptr || desktopRect == nullptr || width == nullptr ||
        height == nullptr)
        return false;
    std::vector<OutputItem> items;
    if (!enumOutputs(items))
        return false;
    for (const auto& item : items) {
        if (item.displayId != displayId)
            continue;
        wcsncpy_s(deviceName, 32, item.desc.DeviceName, _TRUNCATE);
        *desktopRect = item.desc.DesktopCoordinates;
        *width = item.desc.DesktopCoordinates.right - item.desc.DesktopCoordinates.left;
        *height = item.desc.DesktopCoordinates.bottom - item.desc.DesktopCoordinates.top;
        return (*width > 0 && *height > 0);
    }
    return false;
}

// ---- 后端工厂 ----

std::unique_ptr<IScreenBackend> makeDxgiBackend(const ScreenTarget& target,
                                                const IScreenBackend::Host& host)
{
    if (target.kind != ScreenTarget::Kind::Display)
        return nullptr;
    try {
        return std::make_unique<ScreenBackendDxgi>(target.displayId, host);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sparkcap
