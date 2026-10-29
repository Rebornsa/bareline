cmake_minimum_required(VERSION 3.15)
project(SysMon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(SysMon WIN32
    main.cpp config.cpp metrics.cpp renderer.cpp tray.cpp
    interaction.cpp SysMon.rc
)

target_link_libraries(SysMon d2d1 dwrite windowscodecs pdh Iphlpapi)
target_compile_options(SysMon PRIVATE /O2 /GS- /utf-8)
target_compile_definitions(SysMon PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000 _UNICODE UNICODE)
target_link_options(SysMon PRIVATE /OPT:REF /OPT:ICF)