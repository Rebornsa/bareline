#include "metrics.h"
#include <iphlpapi.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#include <map>
#include "ratetracker.h"
// ----- History for delta calculations -----
static ULONGLONG s_prevIdle = 0, s_prevKernel = 0, s_prevUser = 0;

static std::map<ULONG, RateTracker<ULONG64>> s_netInTrackers;
static std::map<ULONG, RateTracker<ULONG64>> s_netOutTrackers;

// ========== GPU PDH ==========
struct GpuPdh {
    PDH_HQUERY hQuery = nullptr;
    PDH_HCOUNTER hCounter = nullptr;
    void Init() {
        if (PdhOpenQueryA(nullptr, 0, &hQuery) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterA(hQuery,
                "\\GPU Engine(*engtype_3D)\\Utilization Percentage",
                0, &hCounter) != ERROR_SUCCESS)
                hCounter = nullptr;
            else
                PdhCollectQueryData(hQuery);
        }
    }
    bool Ok() const { return hQuery && hCounter; }
    PDH_STATUS Refresh() { return PdhCollectQueryData(hQuery); }
    ~GpuPdh() { if (hQuery) PdhCloseQuery(hQuery); }
};
static GpuPdh s_gpu;
static PDH_HCOUNTER s_cpuFreqCounter = nullptr;
static double s_cpuBaseFreqMhz = 0.0;

// ========== Init / Shutdown ==========
bool InitMetrics() {
    s_gpu.Init();
    // Read base CPU frequency from registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD mhz = 0, sz = sizeof(DWORD);
        RegQueryValueExA(hKey, "~MHz", nullptr, nullptr, (BYTE*)&mhz, &sz);
        s_cpuBaseFreqMhz = (double)mhz;
        RegCloseKey(hKey);
    }
    // CPU performance percentage PDH counter
    if (s_gpu.hQuery && s_cpuBaseFreqMhz > 0.0) {
        PdhAddEnglishCounterA(s_gpu.hQuery,
            "\\Processor Information(_Total)\\% Processor Performance",
            0, &s_cpuFreqCounter);
    }
    return true;
}

void ShutdownMetrics() {}


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
static void CollectGpu(SystemMetrics& m) {
    m.gpu.available = false;
    m.gpu.usagePercent = 0.0;
    if (!s_gpu.Ok()) return;

    DWORD dwBufferSize = 0;
    DWORD dwItemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayA(s_gpu.hCounter, PDH_FMT_DOUBLE,
        &dwBufferSize, &dwItemCount, nullptr);
    if (status != PDH_MORE_DATA || dwItemCount == 0) return;

    PDH_FMT_COUNTERVALUE_ITEM_A* pItems =
        (PDH_FMT_COUNTERVALUE_ITEM_A*)HeapAlloc(GetProcessHeap(), 0, dwBufferSize);
    if (!pItems) return;

    status = PdhGetFormattedCounterArrayA(s_gpu.hCounter, PDH_FMT_DOUBLE,
        &dwBufferSize, &dwItemCount, pItems);
    if (status == ERROR_SUCCESS) {
        double totalUtil = 0.0;
        for (DWORD i = 0; i < dwItemCount; i++) {            totalUtil += pItems[i].FmtValue.doubleValue;
        }
        m.gpu.available = true;
        m.gpu.usagePercent = totalUtil;
        if (m.gpu.usagePercent < 0.0) m.gpu.usagePercent = 0.0;
        if (m.gpu.usagePercent > 100.0) m.gpu.usagePercent = 100.0;
    }
    HeapFree(GetProcessHeap(), 0, pItems);
}

// ========== Memory ==========
static void CollectMemory(SystemMetrics& m) {
    MEMORYSTATUSEX msx = { sizeof(msx) };
    if (GlobalMemoryStatusEx(&msx)) {
        m.mem.usagePercent = (double)msx.dwMemoryLoad;
        m.mem.totalPhys = msx.ullTotalPhys;
        m.mem.availPhys = msx.ullAvailPhys;
    }
}

// ========== Disk Space ==========
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
        ULARGE_INTEGER freeAvail, total, freeTotal;
        if (GetDiskFreeSpaceExW(root, &freeAvail, &total, &freeTotal)) {
            dm.totalBytes = total.QuadPart;
            dm.freeBytes = freeTotal.QuadPart;
        }
        m.disks.push_back(dm);
    }
}

// ========== Disk IO (IOCTL_DISK_PERFORMANCE) ==========
static std::map<int, HANDLE> s_diskHandles;
static std::map<int, RateTracker<LONGLONG>> s_diskReadTrackers;
static std::map<int, RateTracker<LONGLONG>> s_diskWriteTrackers;

static void CollectDiskIO(SystemMetrics& m) {
    // Close cached handles for drive letters that disappeared (device dropped),
    // so a stale handle is never reused after the letter comes back.
    bool present[26] = {};
    for (auto& drv : m.disks) present[drv.driveRoot[0] - L'A'] = true;
    for (auto it = s_diskHandles.begin(); it != s_diskHandles.end(); ) {
        if (!present[it->first]) {
            if (it->second) CloseHandle(it->second);
            it = s_diskHandles.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& drv : m.disks) {
        int dkIdx = drv.driveRoot[0] - L'A';
        // Retry the open every pass while we hold no handle: a volume that
        // re-appears after a device drop/reconnect must recover on its own.
        if (!s_diskHandles[dkIdx]) {
            wchar_t volPath[16];
            swprintf_s(volPath, L"\\\\.\\%c:", drv.driveRoot[0]);
            HANDLE h = CreateFileW(volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            s_diskHandles[dkIdx] = (h != INVALID_HANDLE_VALUE) ? h : nullptr;
            if (!s_diskHandles[dkIdx]) continue;
            // Counters restart from zero on the fresh device: drop old trackers
            // so the first delta doesn't produce a bogus spike.
            s_diskReadTrackers.erase(dkIdx);
            s_diskWriteTrackers.erase(dkIdx);
        }
        HANDLE h = s_diskHandles[dkIdx];
        DISK_PERFORMANCE dp = {};
        DWORD bytes = 0;
        if (!DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, nullptr, 0,
                             &dp, sizeof(dp), &bytes, nullptr)) {
            // Dead handle (device dropped mid-session): discard it, reopen next pass.
            CloseHandle(h);
            s_diskHandles[dkIdx] = nullptr;
            continue;
        }
        double rrate = s_diskReadTrackers[dkIdx].Push(dp.BytesRead.QuadPart);
        double wrate = s_diskWriteTrackers[dkIdx].Push(dp.BytesWritten.QuadPart);
        if (rrate >= 0.0) drv.readBytesPerSec = rrate;
        if (wrate >= 0.0) drv.writeBytesPerSec = wrate;
    }
}




// Map common interface names to friendly labels
static void FriendlyName(std::wstring& desc) {
    if (desc.find(L"Wireless") != std::wstring::npos || desc.find(L"Wi-Fi") != std::wstring::npos) { desc = L"WiFi"; return; }
    if (desc.find(L"Ethernet") != std::wstring::npos || desc.find(L"Realtek") != std::wstring::npos) { desc = L"Ethernet"; return; }
    if (desc.find(L"Bluetooth") != std::wstring::npos) { desc.clear(); return; }
    // Filter VPN / tunnel / virtual adapters
    if (desc.find(L"VPN") != std::wstring::npos || desc.find(L"TAP") != std::wstring::npos
        || desc.find(L"Tunnel") != std::wstring::npos || desc.find(L"WireGuard") != std::wstring::npos
        || desc.find(L"WAN") != std::wstring::npos || desc.find(L"PPP") != std::wstring::npos
        || desc.find(L"L2TP") != std::wstring::npos || desc.find(L"SSTP") != std::wstring::npos
        || desc.find(L"IKEv2") != std::wstring::npos) { desc.clear(); return; }
    if (desc.length() > 28) desc = desc.substr(0, 28) + L"...";
}

// ========== Network (GetIfTable) ==========
static void CollectNetwork(SystemMetrics& m) {
    ULONG bufSize = 0;
    if (GetIfTable(nullptr, &bufSize, FALSE) != ERROR_INSUFFICIENT_BUFFER) return;
    auto* pIf = (MIB_IFTABLE*)HeapAlloc(GetProcessHeap(), 0, bufSize);
    if (!pIf) return;
    if (GetIfTable(pIf, &bufSize, FALSE) != NO_ERROR) { HeapFree(GetProcessHeap(), 0, pIf); return; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    for (DWORD i = 0; i < pIf->dwNumEntries; i++) {
        MIB_IFROW& row = pIf->table[i];
        if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (row.dwOperStatus == 0) continue;
        NetMetrics nm;
        int len = row.dwDescrLen > 255 ? 255 : (int)row.dwDescrLen;
        wchar_t wdesc[256] = {};
        MultiByteToWideChar(CP_ACP, 0, (LPCCH)row.bDescr, len, wdesc, 256);
        nm.description = wdesc;
        nm.downloadBytesPerSec = 0.0;
        nm.uploadBytesPerSec = 0.0;
        double inRate = s_netInTrackers[row.dwIndex].Push(row.dwInOctets);
        double outRate = s_netOutTrackers[row.dwIndex].Push(row.dwOutOctets);
        if (inRate >= 0.0) nm.downloadBytesPerSec = inRate;
        if (outRate >= 0.0) nm.uploadBytesPerSec = outRate;
        FriendlyName(nm.description);
        if (nm.description.empty()) continue;
        bool dup = false;
        for (auto& existing : m.nets) {
            if (existing.description == nm.description) { dup = true; break; }
        }
        if (!dup) m.nets.push_back(nm);
    }

    HeapFree(GetProcessHeap(), 0, pIf);
}

// ========== CollectMetrics ==========
SystemMetrics CollectMetrics() {
    SystemMetrics m = {};
    if (s_gpu.hQuery) s_gpu.Refresh();
    CollectCpu(m);
    CollectGpu(m);
    CollectMemory(m);
    CollectDiskSpace(m);
    CollectDiskIO(m);
    CollectNetwork(m);
    return m;
}

// ========== Formatting ==========
// Fixed-width rate field: %5.1f + single-char unit, always 6 wchars.
// Unit is chosen by the value that %5.1f will actually DISPLAY: at
// 999.95~999.99 the rounded text would be "1000.0" (7 wchars), so promote
// to the next unit there; the G band saturates at 999.9G.
static void FormatRate(double bps, wchar_t* buf) {
    double v = bps; wchar_t unit = L'B';
    while (unit != L'G' && v >= 999.95) {
        v /= 1e3;
        unit = (unit == L'B') ? L'K' : (unit == L'K') ? L'M' : L'G';
    }
    if (v >= 999.95) v = 999.9; // top of G band: no larger unit exists
    swprintf_s(buf, 8, L"%5.1f%c", v, unit);
}

void FormatMetrics(const SystemMetrics& m, std::vector<std::wstring>& lines) {
    // CPU frequency latch: once a valid reading is seen, keep last value
    // so the field width (and presence) never fluctuates.
    static bool s_freqValid = false;
    static double s_freqLast = 0.0;
    if (m.cpu.freqGHz > 0.0) { s_freqValid = true; s_freqLast = m.cpu.freqGHz; }

    wchar_t line[512], buf[64];
    int pos = swprintf_s(line, L"CPU %3.0f%%", m.cpu.usagePercent);
    if (s_freqValid)
        pos += swprintf_s(line + pos, 512 - pos, L" %.2fG", s_freqLast);
    // GPU segment: field is 4 wchars whether percent or N/A
    pos += swprintf_s(line + pos, 512 - pos, L" \x2502 GPU %s",
        m.gpu.available ? (swprintf_s(buf, L"%3.0f%%", m.gpu.usagePercent), buf)
                        : L" N/A");
    pos += swprintf_s(line + pos, 512 - pos, L" \x2502 MEM %3.0f%%", m.mem.usagePercent);
    // Disk segments (one per present drive)
    for (auto& d : m.disks) {
        if (!d.present) continue;
        wchar_t rBuf[8], wBuf[8];
        FormatRate(d.readBytesPerSec, rBuf);
        FormatRate(d.writeBytesPerSec, wBuf);
        pos += swprintf_s(line + pos, 512 - pos, L" \x2502 %c: R%s W%s",
            d.driveRoot[0], rBuf, wBuf);
    }
    // NET segment: aggregate across all adapters
    double dl = 0.0, ul = 0.0;
    for (auto& n : m.nets) { dl += n.downloadBytesPerSec; ul += n.uploadBytesPerSec; }
    wchar_t dBuf[8], uBuf[8];
    FormatRate(dl, dBuf);
    FormatRate(ul, uBuf);
    swprintf_s(line + pos, 512 - pos, L" \x2502 NET \x2193%s \x2191%s", dBuf, uBuf);

    lines.clear();
    lines.push_back(line);
}
