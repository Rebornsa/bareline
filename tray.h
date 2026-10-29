#pragma once
#include <Windows.h>

struct AppConfig;

bool CreateTrayIcon(HWND hwnd, HINSTANCE hInst);
void DestroyTrayIcon();
void ShowTrayMenu(HWND hwnd, const AppConfig& cfg);
