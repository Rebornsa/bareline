#include "tray.h"
#include "config.h"
#include "resource.h"
#include "_icon_data.h"

static NOTIFYICONDATAW s_nid = {};
static bool s_trayCreated = false;

bool CreateTrayIcon(HWND hwnd, HINSTANCE hInst) {
    s_nid.cbSize = sizeof(NOTIFYICONDATAW);
    s_nid.hWnd = hwnd;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = WM_TRAYICON;
    s_nid.hIcon = CreateIconFromResource((PBYTE)s_trayIconData, sizeof(s_trayIconData), TRUE, 0x00030000);
    if (!s_nid.hIcon) s_nid.hIcon = LoadIconA(nullptr, MAKEINTRESOURCEA(IDI_APPLICATION));
    wcscpy_s(s_nid.szTip, L"Bareline");
    s_trayCreated = Shell_NotifyIconW(NIM_ADD, &s_nid) != FALSE;
    return s_trayCreated;
}

void DestroyTrayIcon() {
    if (s_trayCreated) { Shell_NotifyIconW(NIM_DELETE, &s_nid); s_trayCreated = false; }
}

void ShowTrayMenu(HWND hwnd, const AppConfig& cfg) {
    HMENU hMenu = LoadMenuA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(IDR_TRAY_MENU));
    if (!hMenu) return;
    HMENU hSubMenu = GetSubMenu(hMenu, 0);
    SetMenuDefaultItem(hSubMenu, IDM_SHOWHIDE, FALSE);
    CheckMenuItem(hSubMenu, IDM_LOCK, MF_BYCOMMAND | (cfg.positionLocked ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_REFRESH_250, MF_BYCOMMAND | (cfg.refreshIntervalMs == 250 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_REFRESH_500, MF_BYCOMMAND | (cfg.refreshIntervalMs == 500 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_REFRESH_1S, MF_BYCOMMAND | (cfg.refreshIntervalMs == 1000 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_OPACITY_60, MF_BYCOMMAND | (cfg.opacity == 60 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_OPACITY_70, MF_BYCOMMAND | (cfg.opacity == 70 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_OPACITY_80, MF_BYCOMMAND | (cfg.opacity == 80 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_FONT_9,  MF_BYCOMMAND | (cfg.fontSize == 9  ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_FONT_12, MF_BYCOMMAND | (cfg.fontSize == 12 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_FONT_16, MF_BYCOMMAND | (cfg.fontSize == 16 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hSubMenu, IDM_FONT_CUSTOM, MF_BYCOMMAND | ((cfg.fontSize != 9 && cfg.fontSize != 12 && cfg.fontSize != 16) ? MF_CHECKED : MF_UNCHECKED));
    // Check registry write permission for AutoStart
    {
        HKEY hTest;
        bool canWrite = (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &hTest) == ERROR_SUCCESS);
        if (canWrite) {
            RegCloseKey(hTest);
            CheckMenuItem(hSubMenu, IDM_AUTOSTART, MF_BYCOMMAND | (cfg.autoStart ? MF_CHECKED : MF_UNCHECKED));
        } else {
            EnableMenuItem(hSubMenu, IDM_AUTOSTART, MF_BYCOMMAND | MF_GRAYED);
        }
    }
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hSubMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

