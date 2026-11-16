#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN

#include "scanner.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <vector>

namespace xcaler {

namespace {

std::wstring NormalizeDirPath(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    if (path.size() == 2 && path[1] == L':') {
        path.push_back(L'\\');
    }
    return path;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& child) {
    if (base.empty()) return child;
    if (base.back() == L'\\' || base.back() == L'/') {
        return base + child;
    }
    return base + L"\\" + child;
}

std::wstring ToLongWinPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path;
    }
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    if (path.size() >= 2 && path[1] == L':') {
        return L"\\\\?\\" + path;
    }
    return path;
}

uint64_t CombineFileSize(DWORD high, DWORD low) {
    return (static_cast<uint64_t>(high) << 32) | static_cast<uint64_t>(low);
}

void MeasureSubtreeIterative(
    const std::wstring& startDir,
    const std::atomic<bool>& cancelFlag,
    ScanEntry& entry,
    ScanProgress& progress,
    ULONGLONG& lastTick,
    const std::function<void(const ScanProgress&)>& onProgress) {

    std::vector<std::wstring> stack;
    stack.push_back(startDir);

    while (!stack.empty()) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            return;
        }

        std::wstring currentDir = std::move(stack.back());
        stack.pop_back();

        std::wstring searchPattern = ToLongWinPath(JoinPath(currentDir, L"*"));
        WIN32_FIND_DATAW ffd{};
        HANDLE hFind = FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &ffd,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH);

        if (hFind == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                ++entry.errorCount;
                if (currentDir == startDir) {
                    entry.accessDenied = true;
                }
            }
            continue;
        }

        do {
            if (cancelFlag.load(std::memory_order_relaxed)) {
                FindClose(hFind);
                return;
            }

            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
                continue;
            }

            const DWORD attrs = ffd.dwFileAttributes;
            const bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool isReparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            if (isDir) {
                ++entry.dirCount;
                ++progress.scannedDirs;
                // Skip junctions and symlinks to prevent double-counting or loops.
                if (!isReparse) {
                    stack.push_back(JoinPath(currentDir, ffd.cFileName));
                }
            } else {
                const uint64_t fileSize = CombineFileSize(ffd.nFileSizeHigh, ffd.nFileSizeLow);
                entry.sizeBytes += fileSize;
                ++entry.fileCount;
                progress.scannedBytes += fileSize;
                ++progress.scannedFiles;
            }
        } while (FindNextFileW(hFind, &ffd) != 0);

        FindClose(hFind);

        ULONGLONG now = GetTickCount64();
        if (onProgress && (now - lastTick >= 100)) {
            lastTick = now;
            progress.currentPath = currentDir;
            onProgress(progress);
        }
    }
}

} // namespace

std::vector<DriveInfo> EnumerateDrives() {
    std::vector<DriveInfo> drives;
    WCHAR buffer[512]{};
    DWORD len = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buffer) - 1), buffer);
    if (len == 0 || len >= std::size(buffer)) {
        return drives;
    }

    const WCHAR* ptr = buffer;
    while (*ptr != L'\0') {
        DriveInfo info;
        info.rootPath = ptr;
        info.driveType = GetDriveTypeW(ptr);

        if (info.driveType == DRIVE_FIXED ||
            info.driveType == DRIVE_REMOVABLE ||
            info.driveType == DRIVE_REMOTE) {
            WCHAR volName[MAX_PATH + 1]{};
            if (GetVolumeInformationW(ptr, volName, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) {
                info.volumeLabel = volName;
            }

            ULARGE_INTEGER freeAvail{}, totalBytes{}, totalFree{};
            if (GetDiskFreeSpaceExW(ptr, &freeAvail, &totalBytes, &totalFree)) {
                info.totalBytes = totalBytes.QuadPart;
                info.freeBytes = totalFree.QuadPart;
                info.usedBytes = (info.totalBytes >= info.freeBytes) ? (info.totalBytes - info.freeBytes) : 0;
            }
            drives.push_back(std::move(info));
        }

        ptr += wcslen(ptr) + 1;
    }

    return drives;
}

bool GetDriveUsageForPath(const std::wstring& path, DriveInfo& outInfo) {
    if (path.empty()) return false;

    WCHAR root[MAX_PATH]{};
    if (path.size() >= 2 && path[1] == L':') {
        root[0] = path[0];
        root[1] = L':';
        root[2] = L'\\';
        root[3] = L'\0';
    } else {
        wcsncpy(root, path.c_str(), MAX_PATH - 1);
    }

    ULARGE_INTEGER freeAvail{}, totalBytes{}, totalFree{};
    if (!GetDiskFreeSpaceExW(root, &freeAvail, &totalBytes, &totalFree)) {
        return false;
    }

    outInfo.rootPath = root;
    outInfo.totalBytes = totalBytes.QuadPart;
    outInfo.freeBytes = totalFree.QuadPart;
    outInfo.usedBytes = (outInfo.totalBytes >= outInfo.freeBytes) ? (outInfo.totalBytes - outInfo.freeBytes) : 0;

    WCHAR volName[MAX_PATH + 1]{};
    if (GetVolumeInformationW(root, volName, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) {
        outInfo.volumeLabel = volName;
    }
    return true;
}

std::wstring FormatBytes(uint64_t bytes) {
    static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }

    WCHAR buf[64]{};
    if (unit == 0) {
        swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (value >= 100.0) {
        swprintf(buf, 64, L"%.1f %ls", value, kUnits[unit]);
    } else {
        swprintf(buf, 64, L"%.2f %ls", value, kUnits[unit]);
    }
    return buf;
}

std::wstring FormatCount(uint64_t count) {
    std::wstring raw = std::to_wstring(count);
    std::wstring formatted;
    int group = 0;
    for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
        if (group > 0 && group % 3 == 0) {
            formatted.push_back(L',');
        }
        formatted.push_back(*it);
        ++group;
    }
    std::reverse(formatted.begin(), formatted.end());
    return formatted;
}

void ScanDirectoryRoot(
    const std::wstring& rootPath,
    const std::atomic<bool>& cancelFlag,
    const std::function<void(const ScanProgress&)>& onProgress,
    const std::function<void(ScanEntry&&)>& onEntryDone) {

    const std::wstring normRoot = NormalizeDirPath(rootPath);
    const std::wstring pattern = ToLongWinPath(JoinPath(normRoot, L"*"));

    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,
        &ffd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);

    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    struct TopItem {
        std::wstring name;
        std::wstring fullPath;
        uint64_t fileSize = 0;
        bool isDir = false;
        bool isReparse = false;
    };

    std::vector<TopItem> topItems;
    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) {
            continue;
        }
        TopItem item;
        item.name = ffd.cFileName;
        item.fullPath = JoinPath(normRoot, ffd.cFileName);
        item.isDir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.isReparse = (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (!item.isDir) {
            item.fileSize = CombineFileSize(ffd.nFileSizeHigh, ffd.nFileSizeLow);
        }
        topItems.push_back(std::move(item));
    } while (FindNextFileW(hFind, &ffd) != 0);

    FindClose(hFind);

    ScanProgress progress;
    ULONGLONG lastTick = GetTickCount64();

    // Group root-level files or report each individually? Reporting each folder and
    // each file (or a virtual "[Archivos en raíz]"? No, reporting each item directly
    // lets the user see big root files like pagefile.sys or hiberfil.sys too!).
    for (const auto& item : topItems) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            break;
        }

        ScanEntry entry;
        entry.name = item.name;
        entry.fullPath = item.fullPath;
        entry.isDirectory = item.isDir;

        if (item.isDir) {
            if (!item.isReparse) {
                progress.currentPath = item.fullPath;
                if (onProgress) {
                    onProgress(progress);
                }
                MeasureSubtreeIterative(item.fullPath, cancelFlag, entry, progress, lastTick, onProgress);
            }
        } else {
            entry.sizeBytes = item.fileSize;
            entry.fileCount = 1;
            progress.scannedBytes += item.fileSize;
            ++progress.scannedFiles;
        }

        if (cancelFlag.load(std::memory_order_relaxed)) {
            break;
        }

        if (onEntryDone) {
            onEntryDone(std::move(entry));
        }
    }

    if (onProgress) {
        onProgress(progress);
    }
}

} // namespace xcaler
