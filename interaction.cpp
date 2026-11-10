#include "interaction.h"
#include "config.h"

InteractionManager::InteractionManager(HWND hwnd, const AppConfig& config)
    : m_hwnd(hwnd), m_config(config) {}

void InteractionManager::EnterInteractive() {
    if (m_interactive) return;
    if (m_config.positionLocked) return;
    m_interactive = true;
    LONG_PTR ex = GetWindowLongPtrA(m_hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrA(m_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void InteractionManager::LeaveInteractive() {
    if (!m_interactive) return;
    m_interactive = false;
    m_dragging = false;
    ReleaseCapture();
    LONG_PTR ex = GetWindowLongPtrA(m_hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TRANSPARENT;
    SetWindowLongPtrA(m_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void InteractionManager::OnPollTimer() {
    POINT pt; GetCursorPos(&pt);
    RECT rc; GetWindowRect(m_hwnd, &rc);
    if (PtInRect(&rc, pt)) {
        m_hoverOutsideCount = 0;
        if (!m_interactive) {
            if (!m_hoverStartTick) m_hoverStartTick = GetTickCount64();
            else if (GetTickCount64() - m_hoverStartTick >= 500) {
                m_hoverStartTick = 0;
                m_hoverOutsideCount = 0;
                EnterInteractive();
            }
        }
    } else {
        m_hoverOutsideCount++;
        if (m_hoverOutsideCount >= 2) {
            m_hoverStartTick = 0;
            m_hoverOutsideCount = 0;
        }
        if (m_interactive) LeaveInteractive();
    }
}

void InteractionManager::OnMouseMove() {
    if (m_dragging && !m_config.positionLocked) {
        POINT pt; GetCursorPos(&pt);
        SetWindowPos(m_hwnd, nullptr, pt.x - m_dragOffset.x, pt.y - m_dragOffset.y,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void InteractionManager::OnMouseLeave() {
    LeaveInteractive();
}

bool InteractionManager::OnLButtonDown() {
    if (m_interactive && !m_config.positionLocked) {
        m_dragging = true;
        POINT pt; GetCursorPos(&pt);
        RECT rc; GetWindowRect(m_hwnd, &rc);
        m_dragOffset.x = pt.x - rc.left;
        m_dragOffset.y = pt.y - rc.top;
        SetCapture(m_hwnd);
        return true;
    }
    return false;
}

bool InteractionManager::OnLButtonUp() {
    if (m_dragging) {
        m_dragging = false;
        ReleaseCapture();
        return true;
    }
    return false;
}

bool InteractionManager::OnSetCursor() {
    if (m_interactive) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return true;
    }
    return false;
}
