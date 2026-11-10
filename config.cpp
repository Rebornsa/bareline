#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr const wchar_t* CFG_NAME = L"Bareline.cfg";
static constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* RUN_VALUE = L"Bareline";
// Written by builds from before the rename. That entry points at an executable
// that no longer exists, so it would fail silently at every logon.
static constexpr const wchar_t* LEGACY_RUN_VALUE = L"SysMon";

static void GetConfigPath(wchar_t* out, size_t size) {
    GetModuleFileNameW(nullptr, out, (DWORD)size);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(out, size, CFG_NAME);
}

static bool FileExists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static DWORD ReadCfgDword(const wchar_t* cfgPath, const char* key, DWORD def) {
    char buf[2048] = {};
    HANDLE h = CreateFileW(cfgPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return def;
    DWORD read = 0;
    ReadFile(h, buf, (DWORD)(sizeof(buf) - 1), &read, nullptr);
    CloseHandle(h);
    char* line = strstr(buf, key);
    if (!line) return def;
    char* eq = strchr(line, '=');
    if (!eq) return def;
    return (DWORD)atoi(eq + 1);
}

static void DropLegacyAutostart() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;
    RegDeleteValueW(hKey, LEGACY_RUN_VALUE);
    RegCloseKey(hKey);
}

AppConfig LoadConfig() {
    AppConfig cfg = {};
    cfg.windowX = -1; cfg.windowY = 0;
    cfg.positionLocked = false;
    cfg.refreshIntervalMs = 500;
    cfg.opacity = 70;
    cfg.fontSize = 9;
    cfg.autoStart = false;
    wchar_t cfgPath[MAX_PATH];
    GetConfigPath(cfgPath, MAX_PATH);
    // No settings file yet means this is the first run of this build, so it is
    // also the one chance to clear the stale autostart entry left by the old
    // name. Doing it here keeps the registry untouched on every later launch.
    if (!FileExists(cfgPath)) DropLegacyAutostart();
    if (!FileExists(cfgPath)) return cfg;
    cfg.windowX = (int)ReadCfgDword(cfgPath, "WindowX", (DWORD)-1);
    cfg.windowY = (int)ReadCfgDword(cfgPath, "WindowY", 0);
    cfg.positionLocked = ReadCfgDword(cfgPath, "PositionLocked", 0) != 0;
    cfg.refreshIntervalMs = (int)ReadCfgDword(cfgPath, "RefreshIntervalMs", 500);
    cfg.opacity = (int)ReadCfgDword(cfgPath, "Opacity", 70);
    cfg.fontSize = (int)ReadCfgDword(cfgPath, "FontSize", 9);
    cfg.autoStart = ReadCfgDword(cfgPath, "AutoStart", 0) != 0;
    if (cfg.refreshIntervalMs != 250 && cfg.refreshIntervalMs != 500 && cfg.refreshIntervalMs != 1000) cfg.refreshIntervalMs = 500;
    if (cfg.opacity < 10) cfg.opacity = 10;
    if (cfg.opacity > 100) cfg.opacity = 100;
    if (cfg.fontSize < 6 || cfg.fontSize > 72) cfg.fontSize = 9;
    return cfg;
}

void SaveConfig(const AppConfig& cfg) {
    wchar_t cfgPath[MAX_PATH];
    GetConfigPath(cfgPath, MAX_PATH);

    // One buffer, one write. The previous version rewrote the whole file once
    // per key (seven read/modify/write round trips for a single change).
    char text[512];
    int len = sprintf_s(text,
        "WindowX=%d\r\n"
        "WindowY=%d\r\n"
        "PositionLocked=%d\r\n"
        "RefreshIntervalMs=%d\r\n"
        "Opacity=%d\r\n"
        "FontSize=%d\r\n"
        "AutoStart=%d\r\n",
        cfg.windowX, cfg.windowY, cfg.positionLocked ? 1 : 0,
        cfg.refreshIntervalMs, cfg.opacity, cfg.fontSize, cfg.autoStart ? 1 : 0);
    if (len <= 0) return;

    HANDLE h = CreateFileW(cfgPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text, (DWORD)len, &written, nullptr);
    CloseHandle(h);
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            RegSetValueExW(hKey, RUN_VALUE, 0, REG_SZ, (BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, RUN_VALUE);
        }
        RegCloseKey(hKey);
    }
}
