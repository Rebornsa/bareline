@echo off
cd /d "%~dp0"
if not exist build mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
if %ERRORLEVEL% NEQ 0 (
    echo CMake configure failed. Trying VS 2022 Preview...
    cmake .. -G "Visual Studio 17 2022" -A x64
)
cmake --build . --config Release
echo.
if exist Release\SysMon.exe (
    echo Build SUCCESS: Release\SysMon.exe
) else (
    echo Build FAILED. Check errors above.
)
