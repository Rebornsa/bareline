# Bareline

**A one-line system monitor for Windows that costs almost nothing.** One small
executable, no installer, no driver, no service, no admin rights.

`barely` + `line`: one bare line of live metrics pinned to the top of the screen,
and barely any CPU spent keeping it there.

| Measured on an idle desktop, default 500 ms refresh | |
|---|---|
| CPU | **0.12% of the machine** (≈0.9% of one core) |
| Memory | **23 MB** private, of which most is the Direct2D/DirectWrite runtime |
| On disk | **~520 KB**, single `.exe` |
| Dependencies | none — static CRT, no .NET, no VC++ redistributable, no kernel driver |

```
CPU  23% 3.25G  58° │ GPU   5%  61° │ MEM  62% │ C: R  1.2M W  0.3M │ D: R 11.0M W  2.8M │ NET ↓  2.3M ↑  0.1M
```

## Metrics

- **CPU** — utilization + current frequency (GHz, latched after the first read so the field never jitters) + temperature
- **GPU** — 3D engine utilization (PDH `GPU Engine(*engtype_3D)`), summed per physical adapter; shown as `N/A` at a fixed width when unavailable + temperature
- **Memory** — utilization percentage
- **Disks** — one segment per volume: read/write rates
- **Network** — aggregate download/upload rates from hardware interfaces only (VPN, tunnel and virtual adapters are filtered out by interface flags, not by name)

Every numeric field is fixed-width on purpose: the bar's layout never jumps
around while values change.

### Temperature support

Temperatures are best-effort and never cost a background thread or a driver:

| Sensor | Source | Availability |
|---|---|---|
| CPU | PDH `\Thermal Zone Information(*)\Temperature` (ACPI thermal zones) | Most laptops and many desktops. No admin rights, no driver. |
| GPU | NVML (`nvml.dll`, shipped with the NVIDIA driver, loaded on demand) | NVIDIA only. AMD/Intel expose no zero-dependency path. |

If no sensor exists the field is omitted entirely rather than showing a
placeholder, so the layout stays stable either way. Sampling runs at 5 s:
microseconds per sample, invisible in use.

## Features

- Always-on-top, click-through until you hover over it (then it becomes draggable)
- Tray icon: right-click for settings, double-click to show/hide
- Refresh interval: 1s / 0.5s / 0.25s
- Opacity: 60% / 70% / 80%
- Font size: 9 / 12 / 16 / custom (6–72)
- Lock position, reset position (settings persist across restarts)
- Optional autostart with Windows (current-user `Run` key, no elevation)
- Single instance, per-monitor DPI aware (correct at 125% / 150% / 200%)
- Multi-monitor aware: default position and reset target the monitor the bar is on, and it hops back on screen if that monitor disappears
- Settings live in a plain `Bareline.cfg` next to the executable — no registry writes

## Why it's cheap

Most of the cost of a bar like this is not the counting, it is the redrawing and
the counter walks. Both are cut down:

- **A frame with nothing new is not drawn.** Formatted text, size, DPI, font size
  and opacity are compared against the last frame; identical means the whole
  `UpdateLayeredWindow` composite is skipped.
- **One text layout, two draws.** The line is shaped once by DirectWrite and
  replayed with `DrawTextLayout` (foreground + 1 px shadow), instead of measuring
  and shaping on every draw call.
- **Surfaces are permanent.** The memory DC, DIB section and brushes are created
  once and only rebuilt when the pixel size actually changes.
- **Display hysteresis.** Percentages are held until they move ~2 points, so an
  idle CPU's jitter stops forcing redraws every tick.
- **Polling staged by cost.** CPU/memory/disk/network at the refresh interval,
  GPU counters every 2 s, temperatures every 5 s.
- **Network is the hot path, and it's cheap now.** `GetIfEntry2` against a cached
  interface list (5 s TTL); a full `GetIfTable2` walk cost ~2.4 ms per poll and was
  80% of a poll's CPU time.
- **Disk statistics use `DeviceIoControl(IOCTL_DISK_PERFORMANCE)`** with
  exponential backoff when the OS has those counters disabled, so a disabled
  counter cannot turn into a handle open/close loop.

If disk read/write always reads `0.0B`, the machine's performance counters are
probably off; an elevated `diskperf -y` re-enables them.

## Requirements

- Windows 10 or later
- To build: CMake 3.15+ and Visual Studio 2019/2022 (MSVC, C++17)

## Build

```bat
cmake -B build
cmake --build build --config Release
```

The executable lands at `build\Release\Bareline.exe`. A convenience `build.bat`
doing the same is included.

## Usage

Run `Bareline.exe`. The bar appears at the top-left of your work area, one line
high, as wide as its content.

- Hover the bar to interact with it (drag to move, right-click for the menu)
- While not hovering, mouse clicks pass through to whatever is underneath
- All settings live in the tray icon's right-click menu
- Double-click the tray icon to hide/show the bar

## License

[MIT](LICENSE)

---

Renamed in November 2026 from `Sysmon-Exlowcost` — that name collided with
Microsoft's Sysinternals Sysmon and read like a typo. Same project, same code;
settings from the old build are not carried over (they were three menu toggles
and a window position).
