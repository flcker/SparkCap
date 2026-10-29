// N1/A5 延迟测量辅助（拍屏法之一：闪烁器）——testing-guide §5.3 S4
// 全屏窗口按固定周期黑白交替，每次翻转把 QPC 时间戳与颜色写入 flash_log.csv。
// 配合 measure_latency.exe（摄像头拍屏 + 翻转检测）得到端到端延迟。
// 用法：flash_screen.exe [周期 ms，默认 500]

#include <stdio.h>
#include <windows.h>

static LARGE_INTEGER g_freq = {};
static BOOL g_white = FALSE;
static UINT_PTR g_timer = 1;

// 每条独立打开-追加-关闭：长持有写句柄会令测量进程无法同时读取 CSV
static void logFlip(void)
{
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    FILE* f = NULL;
    if (fopen_s(&f, "flash_log.csv", "a") == 0 && f != NULL) {
        fprintf(f, "%lld,%d\n", (long long)now.QuadPart, g_white ? 1 : 0);
        fclose(f);
    }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, g_timer, 500, NULL);  // 周期在 main 里按参数调整
        return 0;
    case WM_TIMER:
        g_white = !g_white;
        logFlip();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        // 互补翻转：上下两半黑白互换——全屏平均亮度恒定，摄像头自动曝光
        // 不震荡（纯全屏黑白交替会令 UVC 曝光剧烈摆动甚至停流）。
        // 测量端（measure_latency）检测上半屏亮度与本翻转对齐。
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, top, bottom;
        GetClientRect(hwnd, &rc);
        top = rc;
        top.bottom = (rc.top + rc.bottom) / 2;
        bottom = rc;
        bottom.top = top.bottom;
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, g_white ? &top : &bottom, white);
        FillRect(dc, g_white ? &bottom : &top, black);
        DeleteObject(white);
        DeleteObject(black);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_CLOSE:
        KillTimer(hwnd, g_timer);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int main(int argc, char** argv)
{
    const UINT periodMs = (argc > 1) ? (UINT)atoi(argv[1]) : 500;

    QueryPerformanceFrequency(&g_freq);
    FILE* hdr = NULL;
    if (fopen_s(&hdr, "flash_log.csv", "w") != 0 || hdr == NULL) {
        printf("cannot open flash_log.csv\n");
        return 1;
    }
    fprintf(hdr, "qpc,white\n");
    fclose(hdr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"sparkcap_flash";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);

    // 全屏置顶无边框
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"SparkCap Latency Flash",
                                WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                GetSystemMetrics(SM_CYSCREEN), NULL, NULL,
                                wc.hInstance, NULL);
    SetTimer(hwnd, g_timer, periodMs, NULL);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    printf("flashing every %u ms (log: flash_log.csv); press any key to stop\n", periodMs);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
