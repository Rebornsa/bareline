#pragma once
#include <Windows.h>
#include <string>
#include <vector>

struct CpuMetrics {
    double usagePercent;
    double freqGHz;
};

struct GpuMetrics {
    double usagePercent;
    bool available;
};

struct MemMetrics {
    double usagePercent;
};

struct TempMetrics {
    // "known" means the sensor exists on this machine. When it does not, the
    // field is omitted from the bar entirely so the layout never reserves space
    // for a reading that will never arrive.
    bool cpuKnown;
    bool cpuValid;
    double cpuCelsius;
    bool gpuKnown;
    bool gpuValid;
    double gpuCelsius;
};

struct DiskMetrics {
    wchar_t driveRoot[4];
    double readBytesPerSec;
    double writeBytesPerSec;
    bool present;
};

struct NetMetrics {
    std::wstring description;
    double downloadBytesPerSec;
    double uploadBytesPerSec;
};

struct SystemMetrics {
    CpuMetrics cpu;
    GpuMetrics gpu;
    MemMetrics mem;
    TempMetrics temp;
    std::vector<DiskMetrics> disks;
    std::vector<NetMetrics> nets;
};

bool InitMetrics();
void ShutdownMetrics();
SystemMetrics CollectMetrics();
void FormatMetrics(const SystemMetrics& m, std::vector<std::wstring>& lines);
