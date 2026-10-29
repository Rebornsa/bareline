#pragma once
#include <Windows.h>
#include <string>
#include <vector>

bool InitRenderer();
void ShutdownRenderer();
SIZE Render(HWND hwnd, const std::vector<std::wstring>& lines, int opacityPercent, int fontSize = 9);
