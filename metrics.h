#pragma once
#include <Windows.h>
#include <pdh.h>
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
    DWORDLONG totalPhys;
    DWORDLONG availPhys;
};

struct DiskMetrics {
    wchar_t driveRoot[4];
    DWORDLONG totalBytes;
    DWORDLONG freeBytes;
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
    std::vector<DiskMetrics> disks;
    std::vector<NetMetrics> nets;
};

bool InitMetrics();
void ShutdownMetrics();
SystemMetrics CollectMetrics();
void FormatMetrics(const SystemMetrics& m, std::vector<std::wstring>& lines);
