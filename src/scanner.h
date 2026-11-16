#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xcaler {

struct DriveInfo {
    std::wstring rootPath;      // e.g. L"C:\\"
    std::wstring volumeLabel;   // e.g. L"Windows"
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t usedBytes = 0;
    uint32_t driveType = 0;
};

struct ScanEntry {
    std::wstring name;
    std::wstring fullPath;
    uint64_t sizeBytes = 0;
    uint64_t fileCount = 0;
    uint64_t dirCount = 0;
    uint64_t errorCount = 0;
    bool isDirectory = false;
    bool accessDenied = false;
};

struct ScanProgress {
    std::wstring currentPath;
    uint64_t scannedBytes = 0;
    uint64_t scannedFiles = 0;
    uint64_t scannedDirs = 0;
};

// Enumerates available fixed/removable/network drives on Windows.
std::vector<DriveInfo> EnumerateDrives();

// Queries disk capacity and usage for the drive containing `path`.
bool GetDriveUsageForPath(const std::wstring& path, DriveInfo& outInfo);

// Formats a byte count into a human-readable string (e.g., "12.45 GB").
std::wstring FormatBytes(uint64_t bytes);

// Formats an integer with thousand separators (e.g., "1,234,567").
std::wstring FormatCount(uint64_t count);

// Scans the immediate children of `rootPath` (folders and files).
// Each top-level folder is recursively measured, skipping reparse points
// (junctions/symlinks) to avoid double-counting or loops.
// Calls `onEntryDone` after each top-level item finishes, and `onProgress`
// periodically during traversal.
void ScanDirectoryRoot(
    const std::wstring& rootPath,
    const std::atomic<bool>& cancelFlag,
    const std::function<void(const ScanProgress&)>& onProgress,
    const std::function<void(ScanEntry&&)>& onEntryDone);

} // namespace xcaler
