# Sysmon-Exlowcost

An extremely lightweight Windows system monitor. A single-line, always-on-top bar sits flush with the top of your screen showing CPU, GPU, memory, per-disk and network rates — rendered with Direct2D, polling via PDH, with no runtime dependencies and a single small executable.

The goal is simple: near-zero cost. No installer, no .NET, no Electron, no background service — just one native exe that stays out of your way.

```
CPU 23% 3.25G │ GPU 5% │ MEM 62% │ C: R  1.2M W  0.3M │ D: R 11.0M W  2.8M │ NET ↓  2.3M ↑  0.1M
```

## Metrics

- **CPU** — total utilization percentage + current frequency (GHz, latched once first read so the field never jitters)
- **GPU** — 3D engine utilization (PDH `GPU Engine(*engtype_3D)`), shown as `N/A` at the same fixed width when unavailable
- **Memory** — utilization percentage
- **Disks** — one segment per volume: read/write rates
- **Network** — aggregate download/upload rates across adapters (VPN/tunnel/virtual adapters filtered out)

All numeric fields are fixed-width on purpose: the bar's layout never jumps around while values change.

## Features

- Always-on-top, click-through until you hover over it (then it becomes draggable)
- Tray icon: right-click for the full settings menu, double-click to show/hide
- Refresh interval: 1s / 0.5s / 0.25s
- Opacity: 60% / 70% / 80%
- Font size: 9 / 12 / 16 / custom (6–72)
- Lock position, reset position (settings persist across restarts)
- Optional autostart with Windows (current-user Run key)
- Single instance, per-monitor DPI aware
- Settings are stored in a plain `SysMon.cfg` next to the executable (no registry; old registry settings are migrated automatically)

## Requirements

- Windows 10 or later
- For building: CMake 3.15+ and Visual Studio 2019/2022 (MSVC, C++17)

## Build

```bat
cmake -B build
cmake --build build --config Release
```

The executable is at `build\Release\SysMon.exe`. A convenience `build.bat` doing the same is included.

## Usage

Run `SysMon.exe`. The bar appears at the top-left of your work area, one line high, as wide as its content.

- Hover the bar to interact with it (drag to move, right-click for the menu)
- While not hovering, mouse clicks pass through to whatever is underneath
- All settings live in the tray icon's right-click menu
- Double-click the tray icon to hide/show the overlay

## Tech notes

- Pure Win32: layered topmost window, `UpdateLayeredWindow` compositing
- Direct2D + DirectWrite rendering (Consolas, with fallbacks), WIC for the icon
- PDH performance counters for CPU/GPU; `GetIfTable` + a rate tracker for network rates; `DeviceIoControl` for per-volume disk statistics
- ~500 ms default poll interval; the process stays in the low single-digit MB of RAM

## License

[MIT](LICENSE)
