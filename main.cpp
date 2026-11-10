#include <Windows.h>
#include <string>
#include <vector>
#include "resource.h"
#include "config.h"
#include "metrics.h"
#include "renderer.h"
#include "tray.h"
#include "interaction.h"

static constexpr const char* WINDOW_CLASS = "BarelineOverlay";
static AppConfig s_config;
static bool s_visible = true;
static InteractionManager* s_pInteraction = nullptr;

// ----- Formatting helpers -----



static void RefreshAndRender(HWND hwnd) {
    SystemMetrics m = CollectMetrics();
    std::vector<std::wstring> lines;
    FormatMetrics(m, lines);
    SIZE sz = Render(hwnd, lines, s_config.opacity, s_config.fontSize);
    RECT rc; GetWindowRect(hwnd, &rc);
    if (rc.right - rc.left != sz.cx || rc.bottom - rc.top != sz.cy) {
        SetWindowPos(hwnd, nullptr, 0, 0, sz.cx, sz.cy,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static INT_PTR CALLBACK FontSizeDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        int curSize = (int)(INT_PTR)lp;
        if (curSize < 6 || curSize > 72) curSize = 12;
        char buf[16];
        sprintf_s(buf, "%d", curSize);
        SetDlgItemTextA(hDlg, IDC_FONT_EDIT, buf);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            char buf[16];
            GetDlgItemTextA(hDlg, IDC_FONT_EDIT, buf, 16);
            int size = atoi(buf);
            if (size >= 6 && size <= 72) {
                EndDialog(hDlg, size);
            } else {
                MessageBoxA(hDlg, "Please enter a value between 6 and 72.", "Invalid Size", MB_OK | MB_ICONWARNING);
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
    }
    return FALSE;
}

// Place the bar at the top-left of the work area of the given monitor.
static void GetDefaultPosition(HMONITOR hMon, int& x, int& y) {
    x = 4; y = 0;
    MONITORINFO mi = { sizeof(mi) };
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
        x = mi.rcWork.left + 4;
        y = mi.rcWork.top;
    } else {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) { x = wa.left + 4; y = wa.top; }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        if (!InitMetrics() || !InitRenderer()) return -1;
        CreateTrayIcon(hwnd, ((LPCREATESTRUCTA)lp)->hInstance);
        SetTimer(hwnd, TIMER_REFRESH, s_config.refreshIntervalMs, nullptr);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        SetTimer(hwnd, TIMER_HOVER_POLL, 200, nullptr);
        s_pInteraction = new InteractionManager(hwnd, s_config);
        return 0;
    }
    case WM_SETCURSOR:
        if (s_pInteraction && s_pInteraction->OnSetCursor()) return TRUE;
        return DefWindowProcA(hwnd, msg, wp, lp);
    case WM_TIMER: {
        if (wp == TIMER_REFRESH) RefreshAndRender(hwnd);
        else if (wp == TIMER_HOVER_POLL) {
            if (s_pInteraction) s_pInteraction->OnPollTimer();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (s_pInteraction) s_pInteraction->OnMouseMove();
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s_pInteraction) s_pInteraction->OnMouseLeave();
        return 0;
    case WM_LBUTTONDOWN:
        if (s_pInteraction) s_pInteraction->OnLButtonDown();
        return 0;
    case WM_LBUTTONUP:
        if (s_pInteraction && s_pInteraction->OnLButtonUp()) {
            RECT rc; GetWindowRect(hwnd, &rc);
            s_config.windowX = rc.left;
            s_config.windowY = rc.top;
            SaveConfig(s_config);
        }
        return 0;
    case WM_RBUTTONUP:
        if (s_pInteraction && s_pInteraction->IsInteractive()) { ShowTrayMenu(hwnd, s_config); }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP) { ShowTrayMenu(hwnd, s_config); }
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            s_visible = !s_visible;
            ShowWindow(hwnd, s_visible ? SW_SHOW : SW_HIDE);
            if (s_visible) RefreshAndRender(hwnd);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_EXIT:       DestroyWindow(hwnd); break;
        case IDM_LOCK:       s_config.positionLocked = !s_config.positionLocked; SaveConfig(s_config); break;
        case IDM_RESETPOS: {
            int x, y;
            GetDefaultPosition(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), x, y);
            SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            s_config.windowX = x;
            s_config.windowY = y;
            SaveConfig(s_config);
            break;
        }
        case IDM_REFRESH_1S: s_config.refreshIntervalMs = 1000; SaveConfig(s_config);
                             SetTimer(hwnd, TIMER_REFRESH, s_config.refreshIntervalMs, nullptr); break;
        case IDM_REFRESH_500:s_config.refreshIntervalMs = 500; SaveConfig(s_config);
                             SetTimer(hwnd, TIMER_REFRESH, s_config.refreshIntervalMs, nullptr); break;
        case IDM_REFRESH_250:s_config.refreshIntervalMs = 250; SaveConfig(s_config);
                             SetTimer(hwnd, TIMER_REFRESH, s_config.refreshIntervalMs, nullptr); break;
        case IDM_OPACITY_60: s_config.opacity = 60; SaveConfig(s_config); RefreshAndRender(hwnd); break;
        case IDM_OPACITY_70: s_config.opacity = 70; SaveConfig(s_config); RefreshAndRender(hwnd); break;
        case IDM_OPACITY_80: s_config.opacity = 80; SaveConfig(s_config); RefreshAndRender(hwnd); break;
        case IDM_FONT_9:  s_config.fontSize = 9;  SaveConfig(s_config); RefreshAndRender(hwnd); break;
        case IDM_FONT_12: s_config.fontSize = 12; SaveConfig(s_config); RefreshAndRender(hwnd); break;
        case IDM_FONT_16: s_config.fontSize = 16; SaveConfig(s_config); RefreshAndRender(hwnd); break;
        case IDM_FONT_CUSTOM: {
            int result = (int)DialogBoxParamA(GetModuleHandleA(nullptr),
                MAKEINTRESOURCEA(IDD_FONT_DIALOG), hwnd, FontSizeDlgProc, (LPARAM)s_config.fontSize);
            if (result >= 6 && result <= 72) {
                s_config.fontSize = result;
                SaveConfig(s_config);
                RefreshAndRender(hwnd);
            }
            break;
        }
        case IDM_AUTOSTART:  s_config.autoStart = !s_config.autoStart;
                             SetAutoStart(s_config.autoStart); SaveConfig(s_config); break;
        case IDM_SHOWHIDE:   s_visible = !s_visible; ShowWindow(hwnd, s_visible ? SW_SHOW : SW_HIDE);
                             if (s_visible) RefreshAndRender(hwnd); break;
        }
        return 0;
    case WM_DISPLAYCHANGE: {
        // A monitor was added/removed or a resolution changed: if the bar is no
        // longer on any monitor, bring it back to a visible work area.
        RECT rc; GetWindowRect(hwnd, &rc);
        if (!MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) {
            int x, y;
            GetDefaultPosition(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), x, y);
            SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            s_config.windowX = x;
            s_config.windowY = y;
            SaveConfig(s_config);
        }
        RefreshAndRender(hwnd);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* prc = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RefreshAndRender(hwnd);
        return 0;
    }
    case WM_DESTROY:
        delete s_pInteraction;
        s_pInteraction = nullptr;
        KillTimer(hwnd, TIMER_REFRESH);
        KillTimer(hwnd, TIMER_HOVER_POLL);
        DestroyTrayIcon();
        ShutdownRenderer();
        ShutdownMetrics();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // Single instance (per session, not machine-wide).
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\Bareline_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(nullptr, "Bareline is already running.", "Bareline", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    s_config = LoadConfig();

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_APP));
    wc.hCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(IDC_ARROW));
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExA(&wc);

    int x = s_config.windowX, y = s_config.windowY;
    RECT savedRect = { x, y, x + 260, y + 100 };
    if (x < 0 || !MonitorFromRect(&savedRect, MONITOR_DEFAULTTONULL)) {
        GetDefaultPosition(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), x, y);
    }

    HWND hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        WINDOW_CLASS, "Bareline",
        WS_POPUP, x, y, 260, 100,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    RefreshAndRender(hwnd);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
