#pragma once
#include <Windows.h>

struct AppConfig {
    int windowX;
    int windowY;
    bool positionLocked;
    int refreshIntervalMs;
    int fontSize;
    int opacity;
    bool autoStart;
};

AppConfig LoadConfig();
void SaveConfig(const AppConfig& cfg);
void SetAutoStart(bool enable);
