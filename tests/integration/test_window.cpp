// I 系（屏幕域窗口捕获，F16–F20）：testing-guide §5.2。
// 自包含可自动化：测试进程自建窗口画图案（不依赖外部应用），覆盖
// list_windows 两段式 / open_window 内容与尺寸 / set_options 时序 / fps 限速 /
// 最小化暂停契约 / 关闭流终结 / resize 帧跟随 / bad hwnd。
// 主后端路径确认：日志路由捕获 start 时打点的 backend=wgc/gdi 标记，本机
// （Win11，WGC 可用）断言走 WGC；回落 GDI 时打印说明。
// 注：等待循环内保持消息泵——测试窗口属本进程，GDI 兜底的 PrintWindow 需要
// 窗口所属线程泵消息才能完成（库的已知限制，见技术选型 §10.5）。

#include "../test_macros.h"

#include <sparkcap.h>

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// ---- 测试窗口：客户区纯色图案（WM_PAINT / WM_PRINTCLIENT 同源，GDI 兜底可用）----

static const COLORREF kPattern = RGB(254, 0, 255);  // BGRA 字节序：B=255 G=0 R=254
static const unsigned char kPatB = 255;
static const unsigned char kPatG = 0;
static const unsigned char kPatR = 254;

static void fillPattern(HWND hwnd, HDC dc)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    HBRUSH brush = CreateSolidBrush(kPattern);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

static LRESULT CALLBACK testWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        fillPattern(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        fillPattern(hwnd, reinterpret_cast<HDC>(wp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// 短消息泵（窗口内容渲染进 DWM 后 WGC/PrintWindow 才有图可采）
static void pumpMessages(int ms)
{
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < static_cast<ULONGLONG>(ms)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

// ---- 回调状态 ----

struct ErrorCounters {
    std::atomic<int> paused{0};
    std::atomic<int> lost{0};
};

static void errorCb(sparkcap_device_t*, sparkcap_err_t code, void* ud)
{
    auto* c = static_cast<ErrorCounters*>(ud);
    if (code == SPARKCAP_ERR_SOURCE_PAUSED)
        c->paused.fetch_add(1);
    else if (code == SPARKCAP_ERR_STREAM_LOST)
        c->lost.fetch_add(1);
}

struct LogSink {
    std::mutex mu;
    std::string all;
};

static void logCb(sparkcap_device_t*, sparkcap_log_level_t, const char* backend,
                  const char* message, void* ud)
{
    auto* s = static_cast<LogSink*>(ud);
    std::lock_guard<std::mutex> lk(s->mu);
    s->all += "[";
    s->all += (backend != nullptr) ? backend : "";
    s->all += "] ";
    s->all += (message != nullptr) ? message : "";
    s->all += "\n";
}

// 帧内图案像素计数（须先断言 stride == width*4 > 0）
static long countPatternPixels(const sparkcap_frame_t* f)
{
    long count = 0;
    const unsigned char* base = static_cast<const unsigned char*>(f->data);
    for (int y = 0; y < f->height; ++y) {
        const unsigned char* row = base + static_cast<size_t>(f->stride) * y;
        for (int x = 0; x < f->width; ++x) {
            const unsigned char* px = row + static_cast<size_t>(x) * 4;
            if (px[0] == kPatB && px[1] == kPatG && px[2] == kPatR)
                ++count;
        }
    }
    return count;
}

static void case_null_args()
{
    sparkcap_backend_t* b = nullptr;
    sparkcap_device_t* d = nullptr;
    sparkcap_window_info_t w[1];
    CHECK_EQ(sparkcap_screen_list_windows(nullptr, nullptr, nullptr),
             SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_open_window(nullptr, 1, &d), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_open_window(nullptr, 1, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_start(nullptr, 0), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_set_options(nullptr, 0), SPARKCAP_ERR_INVALID_ARG);

    CHECK_EQ(sparkcap_create_backend(&b), SPARKCAP_ERR_OK);
    if (b == nullptr)
        return;
    CHECK_EQ(sparkcap_screen_list_windows(b, nullptr, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_list_windows(b, w, nullptr), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_open_window(b, 0, &d), SPARKCAP_ERR_INVALID_ARG);  // hwnd=0
    CHECK_EQ(sparkcap_screen_open_window(b, 1, nullptr), SPARKCAP_ERR_INVALID_ARG);
    sparkcap_destroy_backend(b);
}

// 主流程：枚举 → 打开 → 选项 → 内容/尺寸 → fps 限速 → 最小化/还原 → resize → 关闭
static void case_window_flow()
{
    SetProcessDPIAware();  // 物理像素坐标（与 WGC/GDI 捕获尺寸同口径）

    WNDCLASSW wc = {};
    wc.lpfnWndProc = testWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SparkCapWindowTest";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ATOM atom = RegisterClassW(&wc);
    CHECK(atom != 0);

    HWND hwnd = CreateWindowExW(0, L"SparkCapWindowTest",
                                L"SparkCap window capture test", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 500, 400, nullptr, nullptr,
                                wc.hInstance, nullptr);
    CHECK(hwnd != nullptr);
    if (hwnd == nullptr)
        return;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    pumpMessages(300);

    sparkcap_backend_t* backend = nullptr;
    CHECK_EQ(sparkcap_create_backend(&backend), SPARKCAP_ERR_OK);
    if (backend == nullptr)
        return;

    ErrorCounters counters;
    LogSink sink;
    sparkcap_set_log_callback(logCb, &sink);

    // F16：两段式枚举 + 测试窗口在列
    int count = 0;
    CHECK_EQ(sparkcap_screen_list_windows(backend, nullptr, &count), SPARKCAP_ERR_OK);
    if (count < 1) {
        // SCREEN=OFF 构建（空计数）或无窗口环境：会话用例整体跳过
        std::printf("  [SKIP] no windows (count=%d)\n", count);
        sparkcap_set_log_callback(nullptr, nullptr);
        sparkcap_destroy_backend(backend);
        return;
    }
    std::vector<sparkcap_window_info_t> windows(static_cast<size_t>(count > 0 ? count : 1));
    int inout = count;
    CHECK_EQ(sparkcap_screen_list_windows(backend, windows.data(), &inout),
             SPARKCAP_ERR_OK);
    CHECK_EQ(inout, count);
    bool found = false;
    for (int i = 0; i < inout; ++i) {
        CHECK(windows[static_cast<size_t>(i)].hwnd != 0);
        CHECK(windows[static_cast<size_t>(i)].width > 0);
        CHECK(windows[static_cast<size_t>(i)].height > 0);
        if (windows[static_cast<size_t>(i)].hwnd ==
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd)))
            found = true;
    }
    CHECK(found);  // 测试窗口为可见顶层窗口，必须在快照中

    // F17：bad hwnd → NOT_FOUND；合法 hwnd → OK
    sparkcap_device_t* bad = nullptr;
    CHECK_EQ(sparkcap_screen_open_window(backend, 0x1234ABCD, &bad), SPARKCAP_ERR_NOT_FOUND);

    sparkcap_device_t* device = nullptr;
    CHECK_EQ(sparkcap_screen_open_window(
                 backend, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd)), &device),
             SPARKCAP_ERR_OK);
    if (device == nullptr) {
        sparkcap_destroy_backend(backend);
        return;
    }

    CHECK_EQ(sparkcap_screen_set_error_callback(device, errorCb, &counters),
             SPARKCAP_ERR_OK);

    // 选项（Spec §4.7）：含未知位返回 OK（位掩码前向兼容）
    CHECK_EQ(sparkcap_screen_set_options(
                 device, SPARKCAP_SCREEN_CURSOR | 0x80000000u),
             SPARKCAP_ERR_OK);

    // F18：fps 负值非法；fps=10 启动
    CHECK_EQ(sparkcap_screen_start(device, -1), SPARKCAP_ERR_INVALID_ARG);
    CHECK_EQ(sparkcap_screen_start(device, 10), SPARKCAP_ERR_OK);

    // start 后设置选项 → UNSUPPORTED（时序对齐回调注册）
    CHECK_EQ(sparkcap_screen_set_options(device, 0), SPARKCAP_ERR_UNSUPPORTED);

    // 主后端路径确认（start 同步打点）：本机 Win11 应走 WGC
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        const bool wgc = sink.all.find("backend=wgc") != std::string::npos;
        const bool gdi = sink.all.find("backend=gdi") != std::string::npos;
        std::printf("  [INFO] window capture backend: %s\n",
                    wgc ? "wgc" : (gdi ? "gdi (fallback)" : "unknown"));
        if (gdi && !wgc) {
            std::printf("  [WARN] WGC unavailable, fell back to GDI PrintWindow. Log:\n%s",
                        sink.all.substr(0, 1200).c_str());
        }
        CHECK(wgc);
    }

    // 帧内容与尺寸：强制重绘触发事件驱动后端出帧（静止窗口无新帧为正常语义）。
    // 尺寸口径：各后端"窗口尺寸"约定不同（WGC=DWM 可视帧界≈窗口减不可见调整边
    // 框，GDI=GetWindowRect 整窗），均含标题栏、以各自口径如实上报——断言落在
    // [客户区, 整窗] 包络内，精确口径由 resize 用例按同后端增量锁死。
    RECT winRect = {};
    RECT cliRect = {};
    GetWindowRect(hwnd, &winRect);
    GetClientRect(hwnd, &cliRect);
    const int winW = winRect.right - winRect.left;
    const int winH = winRect.bottom - winRect.top;
    const int cliW = cliRect.right - cliRect.left;
    const int cliH = cliRect.bottom - cliRect.top;

    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);
    pumpMessages(100);

    sparkcap_frame_t* frame = nullptr;
    int rc = sparkcap_screen_get_frame(device, &frame, 3000);
    CHECK_EQ(rc, SPARKCAP_ERR_OK);
    int firstW = 0;
    int firstH = 0;
    if (rc == SPARKCAP_ERR_OK && frame != nullptr) {
        CHECK(frame->pixel_format == SPARKCAP_PIXFMT_RGB32);
        CHECK(frame->stride == frame->width * 4);
        CHECK(frame->stride > 0);
        CHECK(frame->size == frame->stride * frame->height);
        CHECK(frame->data != nullptr);
        CHECK(frame->timestamp > 0);
        std::printf("  [INFO] frame %dx%d (window %dx%d, client %dx%d)\n", frame->width,
                    frame->height, winW, winH, cliW, cliH);
        CHECK(frame->width >= cliW - 20 && frame->width <= winW);   // 尺寸在窗口口径包络内
        CHECK(frame->height >= cliH && frame->height <= winH);
        CHECK(countPatternPixels(frame) >= 100);  // 帧含所画图案色值
        firstW = frame->width;
        firstH = frame->height;
    }

    // F18：fps=10 静止内容 2 秒 ≤ 15 帧（WGC 静止不出帧；限速上限 10fps=20，
    // 上界收紧留裕量——内容无变化帧数应为个位数）
    {
        int frames = 0;
        const ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 2000) {
            sparkcap_frame_t* f = nullptr;
            const int r = sparkcap_screen_get_frame(device, &f, 100);
            if (r == SPARKCAP_ERR_OK)
                ++frames;
            else
                CHECK_EQ(r, SPARKCAP_ERR_TIMEOUT);
            pumpMessages(10);
        }
        std::printf("  [INFO] fps throttle: %d frames in 2s (fps=10)\n", frames);
        CHECK(frames <= 15);
    }

    // F19：最小化 → SOURCE_PAUSED 一次 + get_frame TIMEOUT
    ShowWindow(hwnd, SW_MINIMIZE);
    pumpMessages(100);
    {
        const ULONGLONG t0 = GetTickCount64();
        while (counters.paused.load() == 0 && GetTickCount64() - t0 < 3000)
            pumpMessages(20);
    }
    CHECK_EQ(counters.paused.load(), 1);
    frame = nullptr;
    CHECK_EQ(sparkcap_screen_get_frame(device, &frame, 500), SPARKCAP_ERR_TIMEOUT);
    pumpMessages(500);  // 暂停期间不得重复通知
    CHECK_EQ(counters.paused.load(), 1);

    // F19：还原 → 静默恢复出帧（无恢复通知）。还原动画期间产生过渡帧
    // （客户区尚未重绘），消耗至重绘后的图案帧。
    ShowWindow(hwnd, SW_RESTORE);
    pumpMessages(200);
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);
    bool restored = false;
    {
        const ULONGLONG tEnd = GetTickCount64() + 3000;
        while (GetTickCount64() < tEnd) {
            frame = nullptr;
            rc = sparkcap_screen_get_frame(device, &frame, 300);
            if (rc == SPARKCAP_ERR_OK && frame != nullptr &&
                countPatternPixels(frame) >= 100) {
                restored = true;
                break;
            }
            if (rc != SPARKCAP_ERR_OK)
                CHECK_EQ(rc, SPARKCAP_ERR_TIMEOUT);
            pumpMessages(20);
        }
    }
    CHECK(restored);  // 还原后帧恢复交付（内容为所画图案）
    CHECK_EQ(counters.paused.load(), 1);  // 无额外 SOURCE_PAUSED

    // F19：resize → 帧尺寸跟随（不通知）。同后端口径下外框 +80×+60 →
    // 帧尺寸增量恰为 +80×+60（边界与标题栏为常量），消耗过渡帧后锁定新尺寸。
    if (firstW > 0 && SetWindowPos(hwnd, nullptr, 0, 0, winW + 80, winH + 60,
                                   SWP_NOMOVE | SWP_NOZORDER) != FALSE) {
        pumpMessages(200);
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        bool sized = false;
        const ULONGLONG tEnd = GetTickCount64() + 3000;
        while (GetTickCount64() < tEnd) {
            sparkcap_frame_t* f = nullptr;
            const int rr = sparkcap_screen_get_frame(device, &f, 300);
            if (rr == SPARKCAP_ERR_OK && f != nullptr) {
                if (f->width == firstW + 80 && f->height == firstH + 60) {
                    sized = true;
                    std::printf("  [INFO] resized frame %dx%d (was %dx%d)\n", f->width,
                                f->height, firstW, firstH);
                    break;
                }
            } else {
                CHECK_EQ(rr, SPARKCAP_ERR_TIMEOUT);
            }
            pumpMessages(20);
        }
        CHECK(sized);  // resize 后帧尺寸跟随（增量精确）
    } else if (firstW > 0) {
        std::printf("  [WARN] SetWindowPos failed, resize check skipped\n");
    }

    // F19：关闭 → STREAM_LOST（错误回调 + get_frame）
    DestroyWindow(hwnd);
    pumpMessages(100);
    {
        const ULONGLONG t0 = GetTickCount64();
        while (counters.lost.load() == 0 && GetTickCount64() - t0 < 3000)
            pumpMessages(20);
    }
    CHECK_EQ(counters.lost.load(), 1);
    frame = nullptr;
    CHECK_EQ(sparkcap_screen_get_frame(device, &frame, 500), SPARKCAP_ERR_STREAM_LOST);

    // 收尾：stop → close → 句柄失效
    CHECK_EQ(sparkcap_screen_stop(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_screen_close(device), SPARKCAP_ERR_OK);
    CHECK_EQ(sparkcap_screen_start(device, 0), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_screen_get_frame(device, &frame, 100), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_screen_stop(device), SPARKCAP_ERR_INVALID_HANDLE);
    CHECK_EQ(sparkcap_screen_close(device), SPARKCAP_ERR_INVALID_HANDLE);

    sparkcap_set_log_callback(nullptr, nullptr);
    sparkcap_destroy_backend(backend);
}

int main()
{
    g_suiteName = "integration_window";
    RUN_CASE(case_null_args);
    RUN_CASE(case_window_flow);
    TEST_MAIN_END();
}
