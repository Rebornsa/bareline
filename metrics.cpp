// winsock2/ws2ipdef must precede windows.h and are required by netioapi.h for
// GetIfTable2 (MIB_IF_TABLE2).
#include <winsock2.h>
#include <ws2ipdef.h>
#include "metrics.h"
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#include <map>
#include <string>
#include <vector>
#include <cmath>
#include "ratetracker.h"

// ----- History for delta calculations -----
static ULONGLONG s_prevIdle = 0, s_prevKernel = 0, s_prevUser = 0;

static std::map<ULONG, RateTracker<ULONG64>> s_netInTrackers;
static std::map<ULONG, RateTracker<ULONG64>> s_netOutTrackers;

// ========== GPU PDH ==========
struct GpuPdh {
    PDH_HQUERY hQuery = nullptr;
    PDH_HCOUNTER hCounter = nullptr;
    std::vector<BYTE> buffer;   // reused across polls: no per-frame allocation

    void Init() {
        if (PdhOpenQueryW(nullptr, 0, &hQuery) != ERROR_SUCCESS) return;
        if (PdhAddEnglishCounterW(hQuery, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                                  0, &hCounter) != ERROR_SUCCESS) {
            hCounter = nullptr;
            return;
        }
        PdhCollectQueryData(hQuery);
    }
    bool Ok() const { return hQuery && hCounter; }
    PDH_STATUS Refresh() { return PdhCollectQueryData(hQuery); }
    void Close() {
        if (hQuery) { PdhCloseQuery(hQuery); hQuery = nullptr; hCounter = nullptr; }
        buffer.clear();
        buffer.shrink_to_fit();
    }
    ~GpuPdh() { Close(); }
};
static GpuPdh s_gpu;
static PDH_HCOUNTER s_cpuFreqCounter = nullptr;
static double s_cpuBaseFreqMhz = 0.0;

// ========== Temperatures ==========
// CPU: ACPI thermal zones via PDH. No driver, no admin rights, no extra thread;
// the counter is polled every TEMP_INTERVAL_MS which costs microseconds.
// GPU: NVML (shipped with the NVIDIA driver) loaded on demand. Never bundled.
static const ULONGLONG TEMP_INTERVAL_MS = 5000;

static PDH_HQUERY   s_tzQuery = nullptr;
static PDH_HCOUNTER s_tzCounter = nullptr;
static bool         s_tzKnown = false;
static bool         s_tzValid = false;
static double       s_cpuTemps = 0.0;
static std::vector<BYTE> s_tzBuffer;

typedef int  (*NvmlInitFn)();
typedef int  (*NvmlShutdownFn)();
typedef int  (*NvmlCountFn)(unsigned*);
typedef int  (*NvmlHandleFn)(unsigned, void**);
typedef int  (*NvmlTempFn)(void*, int, unsigned*);

static HMODULE         s_nvmlDll = nullptr;
static void*           s_nvmlDevice = nullptr;
static NvmlInitFn      s_nvmlInit = nullptr;
static NvmlShutdownFn  s_nvmlShutdown = nullptr;
static NvmlTempFn      s_nvmlGetTemp = nullptr;
static bool            s_gpuTempKnown = false;
static bool            s_gpuTempValid = false;
static double          s_gpuTemps = 0.0;
static ULONGLONG       s_lastTempTick = 0;

// ACPI reports tenths of Kelvin (2982 = 25.05 C) on most systems, but some
// firmware reports Kelvin or Celsius directly. Normalise by magnitude.
static double NormalizeCelsius(double raw) {
    if (!(raw > 0.0)) return -1.0;
    double c;
    if (raw > 1000.0)      c = raw / 10.0 - 273.15;  // tenths of kelvin
    else if (raw > 200.0)  c = raw - 273.15;         // kelvin
    else                   c = raw;                  // celsius
    if (c < -40.0 || c > 150.0) return -1.0;
    return c;
}

static void InitCpuTemp() {
    if (PdhOpenQueryW(nullptr, 0, &s_tzQuery) != ERROR_SUCCESS) { s_tzQuery = nullptr; return; }
    if (PdhAddEnglishCounterW(s_tzQuery, L"\\Thermal Zone Information(*)\\Temperature",
                              0, &s_tzCounter) != ERROR_SUCCESS) {
        PdhCloseQuery(s_tzQuery);
        s_tzQuery = nullptr;
        s_tzCounter = nullptr;
        return;
    }
    PdhCollectQueryData(s_tzQuery);
    DWORD size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(s_tzCounter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    s_tzKnown = (st == PDH_MORE_DATA && count > 0);
    if (!s_tzKnown) {
        PdhCloseQuery(s_tzQuery);
        s_tzQuery = nullptr;
        s_tzCounter = nullptr;
    }
}

static bool SampleCpuTemp(double& outC) {
    if (!s_tzQuery || !s_tzCounter) return false;
    if (PdhCollectQueryData(s_tzQuery) != ERROR_SUCCESS) return false;
    DWORD size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(s_tzCounter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (st != PDH_MORE_DATA || count == 0) return false;
    if (s_tzBuffer.size() < size) s_tzBuffer.resize(size);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)s_tzBuffer.data();
    st = PdhGetFormattedCounterArrayW(s_tzCounter, PDH_FMT_DOUBLE, &size, &count, items);
    if (st != ERROR_SUCCESS) return false;

    // Several zones exist per machine; the hottest one tracks CPU package heat.
    double best = -1.0;
    for (DWORD i = 0; i < count; i++) {
        double c = NormalizeCelsius(items[i].FmtValue.doubleValue);
        if (c > best) best = c;
    }
    if (best < 0.0) return false;
    outC = best;
    return true;
}

static void InitGpuTemp() {
    s_nvmlDll = LoadLibraryW(L"nvml.dll");
    if (!s_nvmlDll) return;
    s_nvmlInit = (NvmlInitFn)GetProcAddress(s_nvmlDll, "nvmlInit_v2");
    if (!s_nvmlInit) s_nvmlInit = (NvmlInitFn)GetProcAddress(s_nvmlDll, "nvmlInit");
    s_nvmlShutdown = (NvmlShutdownFn)GetProcAddress(s_nvmlDll, "nvmlShutdown");
    auto count = (NvmlCountFn)GetProcAddress(s_nvmlDll, "nvmlDeviceGetCount_v2");
    if (!count) count = (NvmlCountFn)GetProcAddress(s_nvmlDll, "nvmlDeviceGetCount");
    auto handle = (NvmlHandleFn)GetProcAddress(s_nvmlDll, "nvmlDeviceGetHandleByIndex_v2");
    if (!handle) handle = (NvmlHandleFn)GetProcAddress(s_nvmlDll, "nvmlDeviceGetHandleByIndex");
    s_nvmlGetTemp = (NvmlTempFn)GetProcAddress(s_nvmlDll, "nvmlDeviceGetTemperature");
    if (!s_nvmlInit || !count || !handle || !s_nvmlGetTemp) return;
    if (s_nvmlInit() != 0) return;
    unsigned devices = 0;
    if (count(&devices) != 0 || devices == 0) return;
    void* dev = nullptr;
    if (handle(0, &dev) != 0 || !dev) return;
    s_nvmlDevice = dev;
    s_gpuTempKnown = true;
}

static bool SampleGpuTemp(double& outC) {
    if (!s_nvmlDevice || !s_nvmlGetTemp) return false;
    unsigned t = 0;
    if (s_nvmlGetTemp(s_nvmlDevice, 0 /* NVML_TEMPERATURE_GPU */, &t) != 0) return false;
    if (t == 0 || t > 150) return false;
    outC = (double)t;
    return true;
}

static void CollectTemps(SystemMetrics& m) {
    ULONGLONG now = GetTickCount64();
    if (s_lastTempTick && now - s_lastTempTick >= TEMP_INTERVAL_MS) {
        s_lastTempTick = now;
        double c = 0.0;
        s_tzValid = SampleCpuTemp(c);
        if (s_tzValid) s_cpuTemps = c;
        s_gpuTempValid = SampleGpuTemp(c);
        if (s_gpuTempValid) s_gpuTemps = c;
    } else if (!s_lastTempTick) {
        s_lastTempTick = now;
    }
    m.temp.cpuKnown = s_tzKnown;
    m.temp.cpuValid = s_tzValid;
    m.temp.cpuCelsius = s_cpuTemps;
    m.temp.gpuKnown = s_gpuTempKnown;
    m.temp.gpuValid = s_gpuTempValid;
    m.temp.gpuCelsius = s_gpuTemps;
}

// ========== Init / Shutdown ==========
bool InitMetrics() {
    s_gpu.Init();

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD mhz = 0, sz = sizeof(DWORD);
        RegQueryValueExW(hKey, L"~MHz", nullptr, nullptr, (BYTE*)&mhz, &sz);
        s_cpuBaseFreqMhz = (double)mhz;
        RegCloseKey(hKey);
    }
    if (s_gpu.hQuery && s_cpuBaseFreqMhz > 0.0) {
        PdhAddEnglishCounterW(s_gpu.hQuery,
            L"\\Processor Information(_Total)\\% Processor Performance",
            0, &s_cpuFreqCounter);
    }

    InitCpuTemp();
    InitGpuTemp();
    return true;
}

void ShutdownMetrics() {
    s_gpu.Close();
    s_cpuFreqCounter = nullptr;
    if (s_tzQuery) { PdhCloseQuery(s_tzQuery); s_tzQuery = nullptr; s_tzCounter = nullptr; }
    s_tzBuffer.clear();
    s_tzBuffer.shrink_to_fit();
    if (s_nvmlDll) {
        if (s_nvmlShutdown && s_nvmlDevice) s_nvmlShutdown();
        FreeLibrary(s_nvmlDll);
        s_nvmlDll = nullptr;
        s_nvmlDevice = nullptr;
        s_nvmlGetTemp = nullptr;
    }
}

// ========== CPU ==========
static void CollectCpu(SystemMetrics& m) {
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    ULARGE_INTEGER i, k, u;
    i.LowPart = idle.dwLowDateTime;   i.HighPart = idle.dwHighDateTime;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;

    if (s_prevIdle != 0) {
        ULONGLONG di = i.QuadPart - s_prevIdle;
        ULONGLONG dt = (k.QuadPart - s_prevKernel) + (u.QuadPart - s_prevUser);
        m.cpu.usagePercent = (dt > 0) ? (100.0 * (1.0 - (double)di / dt)) : 0.0;
        if (m.cpu.usagePercent < 0.0) m.cpu.usagePercent = 0.0;
        if (m.cpu.usagePercent > 100.0) m.cpu.usagePercent = 100.0;
    }
    s_prevIdle = i.QuadPart; s_prevKernel = k.QuadPart; s_prevUser = u.QuadPart;

    m.cpu.freqGHz = 0.0;
    if (s_cpuFreqCounter) {
        DWORD dwType = 0;
        PDH_FMT_COUNTERVALUE cv;
        if (PdhGetFormattedCounterValue(s_cpuFreqCounter, PDH_FMT_DOUBLE, &dwType, &cv) == ERROR_SUCCESS)
            m.cpu.freqGHz = s_cpuBaseFreqMhz * cv.doubleValue / 100000.0;
    }
}

// ========== GPU ==========
static int PhysIndexOf(const wchar_t* name) {
    if (!name) return 0;
    const wchar_t* p = wcsstr(name, L"_phys_");
    return p ? _wtoi(p + 6) : 0;
}

// Reused: the buffer is already large enough in the steady state, so the
// counter array is fetched with a single PDH call instead of the usual
// "ask for size, then ask again for data" pair.
static bool SampleGpuUtil(double& outPercent) {
    if (!s_gpu.Ok()) return false;

    DWORD size = (DWORD)s_gpu.buffer.size();
    DWORD count = 0;
    PDH_STATUS status = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto* pItems = size ? (PDH_FMT_COUNTERVALUE_ITEM_W*)s_gpu.buffer.data() : nullptr;
        status = PdhGetFormattedCounterArrayW(s_gpu.hCounter, PDH_FMT_DOUBLE, &size, &count, pItems);
        if (status != PDH_MORE_DATA) break;
        s_gpu.buffer.resize(size);
    }
    if (status != ERROR_SUCCESS || count == 0) return false;

    auto* pItems = (PDH_FMT_COUNTERVALUE_ITEM_W*)s_gpu.buffer.data();
    // Sum per physical adapter, then report the busiest one. Summing across all
    // adapters (the old behaviour) over-reports on multi-GPU machines.
    std::map<int, double> perPhys;
    for (DWORD i = 0; i < count; i++) {
        double v = pItems[i].FmtValue.doubleValue;
        if (v < 0.0) v = 0.0;
        perPhys[PhysIndexOf(pItems[i].szName)] += v;
    }
    double busiest = 0.0;
    for (auto& kv : perPhys) if (kv.second > busiest) busiest = kv.second;
    if (busiest > 100.0) busiest = 100.0;
    outPercent = busiest;
    return true;
}

static bool    s_gpuUtilValid = false;
static double  s_gpuUtil = 0.0;

static void CollectGpu(SystemMetrics& m, bool refresh) {
    if (refresh) {
        double v = 0.0;
        if (SampleGpuUtil(v)) { s_gpuUtilValid = true; s_gpuUtil = v; }
    }
    m.gpu.available = s_gpuUtilValid;
    m.gpu.usagePercent = s_gpuUtil;
}

// ========== Memory ==========
static void CollectMemory(SystemMetrics& m) {
    MEMORYSTATUSEX msx = { sizeof(msx) };
    if (GlobalMemoryStatusEx(&msx)) m.mem.usagePercent = (double)msx.dwMemoryLoad;
}

// ========== Disk volumes ==========
static void CollectDiskSpace(SystemMetrics& m) {
    DWORD drives = GetLogicalDrives();
    for (wchar_t c = L'A'; c <= L'Z'; c++) {
        if (!(drives & (1u << (c - L'A')))) continue;
        wchar_t root[4] = { c, L':', L'\\', 0 };
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        DiskMetrics dm = {};
        wcscpy_s(dm.driveRoot, root);
        dm.present = true;
        m.disks.push_back(dm);
    }
}

// ========== Disk IO (IOCTL_DISK_PERFORMANCE) ==========
// The counters behind this IOCTL can be disabled machine-wide (diskperf -N), in
// which case every call fails. Back off instead of reopening the volume handle
// twice a second forever.
struct DiskChannel {
    HANDLE    handle = nullptr;
    unsigned  failures = 0;
    ULONGLONG retryAt = 0;
    RateTracker<LONGLONG> read;
    RateTracker<LONGLONG> write;
};
static std::map<int, DiskChannel> s_diskChannels;

static void BackOff(DiskChannel& ch, ULONGLONG now) {
    if (ch.failures < 6) ch.failures++;
    ULONGLONG delay = 1000ULL << ch.failures;   // 2s, 4s, 8s ... 64s
    if (delay > 30000ULL) delay = 30000ULL;
    ch.retryAt = now + delay;
}

static void CollectDiskIO(SystemMetrics& m) {
    bool present[26] = {};
    for (auto& drv : m.disks) present[drv.driveRoot[0] - L'A'] = true;
    for (auto it = s_diskChannels.begin(); it != s_diskChannels.end(); ) {
        if (!present[it->first]) {
            if (it->second.handle) CloseHandle(it->second.handle);
            it = s_diskChannels.erase(it);
        } else {
            ++it;
        }
    }

    ULONGLONG now = GetTickCount64();
    for (auto& drv : m.disks) {
        int idx = drv.driveRoot[0] - L'A';
        DiskChannel& ch = s_diskChannels[idx];

        if (!ch.handle) {
            if (ch.retryAt && now < ch.retryAt) continue;
            wchar_t volPath[16];
            swprintf_s(volPath, L"\\\\.\\%c:", drv.driveRoot[0]);
            HANDLE h = CreateFileW(volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) { BackOff(ch, now); continue; }
            ch.handle = h;
            ch.read = RateTracker<LONGLONG>();
            ch.write = RateTracker<LONGLONG>();
        }

        DISK_PERFORMANCE dp = {};
        DWORD bytes = 0;
        if (!DeviceIoControl(ch.handle, IOCTL_DISK_PERFORMANCE, nullptr, 0,
                             &dp, sizeof(dp), &bytes, nullptr)) {
            // A genuine lost device: drop the handle and reopen later.
            if (GetLastError() == ERROR_INVALID_HANDLE) {
                CloseHandle(ch.handle);
                ch.handle = nullptr;
            }
            BackOff(ch, now);
            continue;
        }
        ch.failures = 0;
        ch.retryAt = 0;

        double rrate = ch.read.Push(dp.BytesRead.QuadPart);
        double wrate = ch.write.Push(dp.BytesWritten.QuadPart);
        if (rrate >= 0.0) drv.readBytesPerSec = rrate;
        if (wrate >= 0.0) drv.writeBytesPerSec = wrate;
    }
}

// ========== Network ==========
// GetIfTable2 walks every interface (including dozens of virtual/WFP ones) and
// cost ~2.4 ms per call -- 80% of a whole poll. So the qualifying interface
// list is cached and each poll only does a cheap per-interface GetIfEntry2.
// The list is rebuilt every few seconds, and immediately on any query failure.
struct NetIface {
    ULONG index;
    std::wstring desc;
};
static std::vector<NetIface> s_netIfaces;
static ULONGLONG s_netIfaceTick = 0;
static const ULONGLONG NET_IFACE_TTL_MS = 5000;

static void FriendlyName(std::wstring& desc) {
    if (desc.find(L"Bluetooth") != std::wstring::npos) { desc.clear(); return; }
    if (desc.find(L"VPN") != std::wstring::npos || desc.find(L"TAP") != std::wstring::npos
        || desc.find(L"Tunnel") != std::wstring::npos || desc.find(L"WireGuard") != std::wstring::npos
        || desc.find(L"WAN") != std::wstring::npos || desc.find(L"PPP") != std::wstring::npos
        || desc.find(L"L2TP") != std::wstring::npos || desc.find(L"SSTP") != std::wstring::npos
        || desc.find(L"IKEv2") != std::wstring::npos) { desc.clear(); return; }
    if (desc.length() > 28) desc = desc.substr(0, 28) + L"...";
}

static void RebuildNetIfaces() {
    s_netIfaces.clear();
    MIB_IF_TABLE2* pTable = nullptr;
    if (GetIfTable2(&pTable) != NO_ERROR || !pTable) return;
    for (ULONG i = 0; i < pTable->NumEntries; i++) {
        MIB_IF_ROW2& row = pTable->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface) continue;  // skip virtual/VPN
        if (row.InterfaceAndOperStatusFlags.FilterInterface) continue;
        std::wstring desc = row.Description;
        FriendlyName(desc);
        if (desc.empty()) continue;
        bool dup = false;
        for (auto& e : s_netIfaces) if (e.desc == desc) { dup = true; break; }
        if (!dup) s_netIfaces.push_back(NetIface{ row.InterfaceIndex, std::move(desc) });
    }
    FreeMibTable(pTable);
}

static void CollectNetwork(SystemMetrics& m) {
    ULONGLONG now = GetTickCount64();
    if (s_netIfaces.empty() || now - s_netIfaceTick >= NET_IFACE_TTL_MS) {
        RebuildNetIfaces();
        s_netIfaceTick = now;
    }

    for (auto& ifc : s_netIfaces) {
        MIB_IF_ROW2 row = {};
        row.InterfaceIndex = ifc.index;
        if (GetIfEntry2(&row) != NO_ERROR) {
            s_netIfaceTick = 0;   // interface vanished: rebuild on the next poll
            continue;
        }
        if (row.OperStatus != IfOperStatusUp) continue;

        double inRate = s_netInTrackers[ifc.index].Push(row.InOctets);
        double outRate = s_netOutTrackers[ifc.index].Push(row.OutOctets);

        NetMetrics nm;
        nm.description = ifc.desc;
        nm.downloadBytesPerSec = (inRate >= 0.0) ? inRate : 0.0;
        nm.uploadBytesPerSec = (outRate >= 0.0) ? outRate : 0.0;
        m.nets.push_back(std::move(nm));
    }
}

// ========== CollectMetrics ==========
// ========== CollectMetrics ==========
SystemMetrics CollectMetrics() {
    SystemMetrics m = {};
    // GPU engine enumeration is the priciest counter on the box; a 2 s cadence
    // is imperceptible and keeps the poll budget in the tens of microseconds.
    static unsigned s_pollSeq = 0;
    const bool gpuTick = ((s_pollSeq++ & 3u) == 0u);

    if (gpuTick && s_gpu.hQuery) s_gpu.Refresh();
    CollectCpu(m);
    CollectGpu(m, gpuTick);
    CollectMemory(m);
    CollectDiskSpace(m);
    CollectDiskIO(m);
    CollectNetwork(m);
    CollectTemps(m);
    return m;
}

// ========== Formatting ==========
// Fixed-width rate field: %5.1f + single-char unit, always 6 wchars.
static void FormatRate(double bps, wchar_t* buf) {
    double v = bps; wchar_t unit = L'B';
    while (unit != L'G' && v >= 999.95) {
        v /= 1e3;
        unit = (unit == L'B') ? L'K' : (unit == L'K') ? L'M' : L'G';
    }
    if (v >= 999.95) v = 999.9;
    swprintf_s(buf, 8, L"%5.1f%c", v, unit);
}

// Display hysteresis. An idle CPU jitters by a point or two every poll, which
// would otherwise force a full redraw on nearly every tick. Holding the shown
// value until it moves by a visible amount is imperceptible but cuts redraws
// by roughly 3x.
static double Sticky(double value, double& shown, double step) {
    if (shown < 0.0 || fabs(value - shown) >= step) shown = value;
    return shown;
}

void FormatMetrics(const SystemMetrics& m, std::vector<std::wstring>& lines) {
    // CPU frequency latch: once a valid reading is seen, keep last value
    // so the field width (and presence) never fluctuates.
    static bool s_freqValid = false;
    static double s_freqLast = 0.0;
    if (m.cpu.freqGHz > 0.0) { s_freqValid = true; s_freqLast = m.cpu.freqGHz; }

    static double s_cpuShown = -1.0, s_memShown = -1.0, s_gpuShown = -1.0;
    static double s_freqShown = -1.0, s_cpuTempShown = -1.0, s_gpuTempShown = -1.0;

    std::wstring line;
    wchar_t buf[64];

    swprintf_s(buf, L"CPU %3.0f%%", Sticky(m.cpu.usagePercent, s_cpuShown, 2.0));
    line += buf;
    if (s_freqValid) {
        swprintf_s(buf, L" %.2fG", Sticky(s_freqLast, s_freqShown, 0.06));
        line += buf;
    }
    if (m.temp.cpuKnown) {
        if (m.temp.cpuValid) swprintf_s(buf, L" %3.0f\u00B0", Sticky(m.temp.cpuCelsius, s_cpuTempShown, 1.0));
        else                 wcscpy_s(buf, L" --\u00B0");
        line += buf;
    }

    line += L" \u2502 GPU ";
    if (m.gpu.available) {
        swprintf_s(buf, L"%3.0f%%", Sticky(m.gpu.usagePercent, s_gpuShown, 2.0));
        line += buf;
    } else {
        line += L" N/A";
    }
    if (m.temp.gpuKnown) {
        if (m.temp.gpuValid) swprintf_s(buf, L" %3.0f\u00B0", Sticky(m.temp.gpuCelsius, s_gpuTempShown, 1.0));
        else                 wcscpy_s(buf, L" --\u00B0");
        line += buf;
    }

    swprintf_s(buf, L" \u2502 MEM %3.0f%%", Sticky(m.mem.usagePercent, s_memShown, 2.0));
    line += buf;

    for (auto& d : m.disks) {
        if (!d.present) continue;
        wchar_t rBuf[8], wBuf[8];
        FormatRate(d.readBytesPerSec, rBuf);
        FormatRate(d.writeBytesPerSec, wBuf);
        swprintf_s(buf, L" \u2502 %c: R%s W%s", d.driveRoot[0], rBuf, wBuf);
        line += buf;
    }

    double dl = 0.0, ul = 0.0;
    for (auto& n : m.nets) { dl += n.downloadBytesPerSec; ul += n.uploadBytesPerSec; }
    wchar_t dBuf[8], uBuf[8];
    FormatRate(dl, dBuf);
    FormatRate(ul, uBuf);
    swprintf_s(buf, L" \u2502 NET \u2193%s \u2191%s", dBuf, uBuf);
    line += buf;

    lines.clear();
    lines.push_back(std::move(line));
}
