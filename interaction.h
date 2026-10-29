#pragma once
#include <Windows.h>

struct AppConfig;

class InteractionManager {
public:
    InteractionManager(HWND hwnd, const AppConfig& config);

    void OnPollTimer();
    void OnMouseMove();
    void OnMouseLeave();
    bool OnLButtonDown();
    bool OnLButtonUp();
    bool OnSetCursor();
    bool IsInteractive() const { return m_interactive; }

private:
    void EnterInteractive();
    void LeaveInteractive();

    HWND m_hwnd;
    const AppConfig& m_config;
    bool m_interactive = false;
    bool m_dragging = false;
    POINT m_dragOffset = {};
    ULONGLONG m_hoverStartTick = 0;
    int m_hoverOutsideCount = 0;
};
