cmake_minimum_required(VERSION 3.15)
project(Bareline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(Bareline WIN32
    main.cpp config.cpp metrics.cpp renderer.cpp tray.cpp
    interaction.cpp Bareline.rc
)

# windowscodecs is no longer linked: the tray icon comes from resource data and
# WIC was initialized but never used.
target_link_libraries(Bareline d2d1 dwrite pdh Iphlpapi)

target_compile_options(Bareline PRIVATE /O2 /utf-8)
target_compile_definitions(Bareline PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000 _UNICODE UNICODE)

# /GS is left at its default (on): the buffer-security check costs a few bytes
# and this binary processes strings from the system.
target_link_options(Bareline PRIVATE
    /OPT:REF /OPT:ICF
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
