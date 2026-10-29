#include "config.h"

static constexpr const wchar_t* REG_PATH = L"Software\\SysMon";

static void GetConfigPath(wchar_t* out, size_t size) {
    GetModuleFileNameW(nullptr, out, (DWORD)size);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(out, size, L"SysMon.cfg");
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

static void WriteCfgDword(const wchar_t* cfgPath, const char* key, DWORD val) {
    char buf[4096] = {};
    HANDLE h = CreateFileW(cfgPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD read = 0;
        ReadFile(h, buf, (DWORD)(sizeof(buf) - 1), &read, nullptr);
        CloseHandle(h);
    }
    char line[128];
    int lineLen = wsprintfA(line, "%s=%d\r\n", key, val);
    char* existing = strstr(buf, key);
    if (existing && (existing == buf || existing[-1] == '\n')) {
        char* end = strchr(existing, '\n');
        if (end) end++;
        char newBuf[4096] = {};
        size_t prefix = existing - buf;
        memcpy(newBuf, buf, prefix);
        strcat_s(newBuf, line);
        if (end) strcat_s(newBuf, end);
        h = CreateFileW(cfgPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, newBuf, (DWORD)strlen(newBuf), &written, nullptr);
            CloseHandle(h);
        }
    } else {
        h = CreateFileW(cfgPath, FILE_APPEND_DATA, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, line, (DWORD)lineLen, &written, nullptr);
            CloseHandle(h);
        }
    }
}

static void WriteCfgInt(const wchar_t* cfgPath, const char* key, int val) {
    WriteCfgDword(cfgPath, key, (DWORD)val);
}

static void DeleteRegistryKey() {
    RegDeleteKeyW(HKEY_CURRENT_USER, REG_PATH);
}

static void MigrateFromRegistry(const wchar_t* cfgPath) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;
    const wchar_t* wNames[] = { L"WindowX", L"WindowY", L"PositionLocked", L"RefreshIntervalMs", L"Opacity", L"FontSize", L"AutoStart" };
    const char* aNames[] = { "WindowX", "WindowY", "PositionLocked", "RefreshIntervalMs", "Opacity", "FontSize", "AutoStart" };
    DWORD defs[] = { (DWORD)-1, 0, 0, 500, 70, 9, 0 };
    DWORD vals[7] = {};
    for (int i = 0; i < 7; i++) {
        DWORD sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, wNames[i], nullptr, nullptr, (BYTE*)&vals[i], &sz) != ERROR_SUCCESS)
            vals[i] = defs[i];
    }
    RegCloseKey(hKey);
    HANDLE h = CreateFileW(cfgPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        for (int i = 0; i < 7; i++) {
            char line[128];
            int v = (vals[i] == 0xFFFFFFFF) ? -1 : (int)vals[i];
            int len = wsprintfA(line, "%s=%d\r\n", aNames[i], v);
            DWORD written = 0;
            WriteFile(h, line, (DWORD)len, &written, nullptr);
        }
        CloseHandle(h);
    }
    DeleteRegistryKey();
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
    if (!FileExists(cfgPath)) MigrateFromRegistry(cfgPath);
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
    if (!FileExists(cfgPath)) {
        HANDLE h = CreateFileW(cfgPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    WriteCfgInt(cfgPath, "WindowX", cfg.windowX);
    WriteCfgInt(cfgPath, "WindowY", cfg.windowY);
    WriteCfgInt(cfgPath, "PositionLocked", cfg.positionLocked ? 1 : 0);
    WriteCfgInt(cfgPath, "RefreshIntervalMs", cfg.refreshIntervalMs);
    WriteCfgInt(cfgPath, "Opacity", cfg.opacity);
    WriteCfgInt(cfgPath, "FontSize", cfg.fontSize);
    WriteCfgInt(cfgPath, "AutoStart", cfg.autoStart ? 1 : 0);
}

void SetAutoStart(bool enable) {
    constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            RegSetValueExW(hKey, L"SysMon", 0, REG_SZ, (BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"SysMon");
        }
        RegCloseKey(hKey);
    }
}
