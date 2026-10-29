#pragma once
#include <Windows.h>

template<typename T>
class RateTracker {
    T m_prev = {};
    LARGE_INTEGER m_prevTime = {};
    bool m_hasPrev = false;
public:
    double Push(T current) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double rate = -1.0;
        if (m_hasPrev) {
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            double dt = (double)(now.QuadPart - m_prevTime.QuadPart) / (double)f.QuadPart;
            if (dt > 0.0) {
                T diff = (current >= m_prev)
                    ? T(current - m_prev)
                    : T(current + (~T(0) - m_prev) + 1);
                rate = (double)(LONGLONG)diff / dt;
            }
        }
        m_prev = current;
        m_prevTime = now;
        m_hasPrev = true;
        return rate;
    }
};
