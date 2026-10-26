// N1/A5 延迟测量辅助（拍屏法之一：闪烁器）——testing-guide §5.3 S4
// 全屏窗口按固定周期黑白交替，每次翻转把 QPC 时间戳与颜色写入 flash_log.csv。
// 配合 measure_latency.exe（摄像头拍屏 + 翻转检测）得到端到端延迟。
// 用法：flash_screen.exe [周期 ms，默认 500]

#include <stdio.h>
#include <windows.h>

static FILE* g_log = NULL;
static LARGE_INTEGER g_freq = {};
static BOOL g_white = FALSE;
static UINT_PTR g_timer = 1;

static void logFlip(void)
{
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    fprintf(g_log, "%lld,%d\n", (long long)now.QuadPart, g_white ? 1 : 0);
    fflush(g_log);
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
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(g_white ? RGB(255, 255, 255) : RGB(0, 0, 0));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_CLOSE:
        KillTimer(hwnd, g_timer);
        if (g_log != NULL)
            fclose(g_log);
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
    fopen_s(&g_log, "flash_log.csv", "w");
    if (g_log == NULL) {
        printf("cannot open flash_log.csv\n");
        return 1;
    }
    fprintf(g_log, "qpc,white\n");

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
