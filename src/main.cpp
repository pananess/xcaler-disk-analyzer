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
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_ENTRY_DONE = WM_APP + 2;
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 3;

enum ControlId {
    IDC_DRIVE_COMBO = 1001,
    IDC_PATH_EDIT = 1002,
    IDC_BROWSE_BTN = 1003,
    IDC_UP_BTN = 1004,
    IDC_SCAN_BTN = 1005,
    IDC_EXPORT_BTN = 1006,
    IDC_SUMMARY_LABEL = 1007,
    IDC_PROGRESS_BAR = 1008,
    IDC_LISTVIEW = 1009,
    IDC_STATUS_LABEL = 1010,

    IDM_CTX_SCAN_FOLDER = 2001,
    IDM_CTX_OPEN_EXPLORER = 2002,
    IDM_CTX_COPY_PATH = 2003,
};

struct AppState {
    HWND hMain = nullptr;
    HWND hDriveCombo = nullptr;
    HWND hPathEdit = nullptr;
    HWND hBrowseBtn = nullptr;
    HWND hUpBtn = nullptr;
    HWND hScanBtn = nullptr;
    HWND hExportBtn = nullptr;
    HWND hSummaryLabel = nullptr;
    HWND hProgressBar = nullptr;
    HWND hListView = nullptr;
    HWND hStatusLabel = nullptr;
    HFONT hFont = nullptr;
    HFONT hBoldFont = nullptr;

    std::vector<xcaler::DriveInfo> drives;
    std::wstring currentTargetPath;
    xcaler::DriveInfo currentDriveInfo{};
    bool hasDriveInfo = false;

    std::vector<xcaler::ScanEntry> entries;
    uint64_t totalScannedBytes = 0;

    int sortColumn = 1; // Default: Size
    bool sortAscending = false;

    std::atomic<bool> isScanning{false};
    std::atomic<bool> cancelRequested{false};
    HANDLE hWorkerThread = nullptr;
    ULONGLONG scanStartTick = 0;
};

AppState g_app;

struct WorkerParams {
    HWND hWnd;
    std::wstring targetPath;
};

void ApplyFont(HWND hCtrl, HFONT hFont) {
    SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
}

std::wstring GetWindowTextStr(HWND hWnd) {
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) return L"";
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hWnd, &s[0], len + 1);
    s.resize(static_cast<size_t>(len));
    return s;
}

std::wstring GetParentDirectory(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    if (path.size() <= 3) {
        return path;
    }
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return path;
    }
    if (pos == 2 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, pos);
}

void UpdateSummaryText() {
    std::wstring summary;
    if (g_app.hasDriveInfo && g_app.currentDriveInfo.totalBytes > 0) {
        double usedPct = (static_cast<double>(g_app.currentDriveInfo.usedBytes) * 100.0) /
                         static_cast<double>(g_app.currentDriveInfo.totalBytes);
        WCHAR buf[512]{};
        swprintf(
            buf,
            512,
            L"Unidad %ls — Capacidad: %ls | Usado: %ls (%.1f%%) | Libre: %ls | Analizado aquí: %ls",
            g_app.currentDriveInfo.rootPath.c_str(),
            xcaler::FormatBytes(g_app.currentDriveInfo.totalBytes).c_str(),
            xcaler::FormatBytes(g_app.currentDriveInfo.usedBytes).c_str(),
            usedPct,
            xcaler::FormatBytes(g_app.currentDriveInfo.freeBytes).c_str(),
            xcaler::FormatBytes(g_app.totalScannedBytes).c_str());
        summary = buf;
    } else {
        summary = L"Ruta: " + g_app.currentTargetPath + L" | Analizado aquí: " +
                  xcaler::FormatBytes(g_app.totalScannedBytes);
    }
    SetWindowTextW(g_app.hSummaryLabel, summary.c_str());
}

void SortEntries() {
    const int col = g_app.sortColumn;
    const bool asc = g_app.sortAscending;

    std::sort(g_app.entries.begin(), g_app.entries.end(), [col, asc](const xcaler::ScanEntry& a, const xcaler::ScanEntry& b) {
        int cmp = 0;
        switch (col) {
            case 0: // Name
                cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                break;
            case 1: // Size
            case 2: // %
            case 3: // Bar
                if (a.sizeBytes < b.sizeBytes) cmp = -1;
                else if (a.sizeBytes > b.sizeBytes) cmp = 1;
                break;
            case 4: // Files
                if (a.fileCount < b.fileCount) cmp = -1;
                else if (a.fileCount > b.fileCount) cmp = 1;
                break;
            case 5: // Dirs
                if (a.dirCount < b.dirCount) cmp = -1;
                else if (a.dirCount > b.dirCount) cmp = 1;
                break;
            case 6: // Type
                if (a.isDirectory != b.isDirectory) cmp = a.isDirectory ? -1 : 1;
                else cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                break;
            default:
                break;
        }
        if (cmp == 0) {
            cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

void RefreshListView() {
    SendMessageW(g_app.hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_app.hListView);

    const uint64_t denom = (g_app.totalScannedBytes > 0) ? g_app.totalScannedBytes : 1;

    for (size_t i = 0; i < g_app.entries.size(); ++i) {
        const auto& e = g_app.entries[i];
        double pct = (static_cast<double>(e.sizeBytes) * 100.0) / static_cast<double>(denom);

        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = static_cast<int>(i);
        lvi.lParam = static_cast<LPARAM>(i);
        lvi.pszText = const_cast<LPWSTR>(e.name.c_str());
        int row = ListView_InsertItem(g_app.hListView, &lvi);

        std::wstring sizeStr = xcaler::FormatBytes(e.sizeBytes);
        ListView_SetItemText(g_app.hListView, row, 1, const_cast<LPWSTR>(sizeStr.c_str()));

        WCHAR pctBuf[32]{};
        swprintf(pctBuf, 32, L"%.1f%%", pct);
        ListView_SetItemText(g_app.hListView, row, 2, pctBuf);

        // Column 3 is custom-drawn usage bar
        ListView_SetItemText(g_app.hListView, row, 3, pctBuf);

        std::wstring filesStr = xcaler::FormatCount(e.fileCount);
        ListView_SetItemText(g_app.hListView, row, 4, const_cast<LPWSTR>(filesStr.c_str()));

        std::wstring dirsStr = e.isDirectory ? xcaler::FormatCount(e.dirCount) : L"-";
        ListView_SetItemText(g_app.hListView, row, 5, const_cast<LPWSTR>(dirsStr.c_str()));

        std::wstring typeStr;
        if (!e.isDirectory) {
            typeStr = L"Archivo";
        } else if (e.accessDenied) {
            typeStr = L"Acceso denegado";
        } else if (e.errorCount > 0) {
            typeStr = L"Carpeta (parcial)";
        } else {
            typeStr = L"Carpeta";
        }
        ListView_SetItemText(g_app.hListView, row, 6, const_cast<LPWSTR>(typeStr.c_str()));
    }

    SendMessageW(g_app.hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hListView, nullptr, FALSE);
}

DWORD WINAPI ScanThreadProc(LPVOID param) {
    std::unique_ptr<WorkerParams> wp(static_cast<WorkerParams*>(param));

    xcaler::ScanDirectoryRoot(
        wp->targetPath,
        g_app.cancelRequested,
        [hWnd = wp->hWnd](const xcaler::ScanProgress& prog) {
            auto* copy = new xcaler::ScanProgress(prog);
            if (!PostMessageW(hWnd, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(copy))) {
                delete copy;
            }
        },
        [hWnd = wp->hWnd](xcaler::ScanEntry&& entry) {
            auto* copy = new xcaler::ScanEntry(std::move(entry));
            if (!PostMessageW(hWnd, WM_APP_ENTRY_DONE, 0, reinterpret_cast<LPARAM>(copy))) {
                delete copy;
            }
        });

    PostMessageW(wp->hWnd, WM_APP_SCAN_DONE, 0, 0);
    return 0;
}

void StartScan(const std::wstring& path) {
    if (g_app.isScanning.load()) {
        return;
    }
    if (path.empty()) {
        return;
    }

    g_app.currentTargetPath = path;
    SetWindowTextW(g_app.hPathEdit, path.c_str());

    g_app.hasDriveInfo = xcaler::GetDriveUsageForPath(path, g_app.currentDriveInfo);
    g_app.entries.clear();
    g_app.totalScannedBytes = 0;
    RefreshListView();
    UpdateSummaryText();

    g_app.isScanning.store(true);
    g_app.cancelRequested.store(false);
    g_app.scanStartTick = GetTickCount64();

    SetWindowTextW(g_app.hScanBtn, L"Detener");
    EnableWindow(g_app.hDriveCombo, FALSE);
    EnableWindow(g_app.hBrowseBtn, FALSE);
    EnableWindow(g_app.hUpBtn, FALSE);
    EnableWindow(g_app.hExportBtn, FALSE);

    ShowWindow(g_app.hProgressBar, SW_SHOW);
    SendMessageW(g_app.hProgressBar, PBM_SETMARQUEE, TRUE, 30);
    SetWindowTextW(g_app.hStatusLabel, (L"Escaneando: " + path).c_str());

    if (g_app.hWorkerThread != nullptr) {
        CloseHandle(g_app.hWorkerThread);
        g_app.hWorkerThread = nullptr;
    }

    auto* wp = new WorkerParams{g_app.hMain, path};
    g_app.hWorkerThread = CreateThread(nullptr, 0, ScanThreadProc, wp, 0, nullptr);
    if (g_app.hWorkerThread == nullptr) {
        delete wp;
        g_app.isScanning.store(false);
        SetWindowTextW(g_app.hScanBtn, L"Analizar");
        ShowWindow(g_app.hProgressBar, SW_HIDE);
        MessageBoxW(g_app.hMain, L"No se pudo iniciar el hilo de análisis.", L"Error", MB_ICONERROR);
    }
}

void StopScan() {
    if (g_app.isScanning.load()) {
        g_app.cancelRequested.store(true);
        SetWindowTextW(g_app.hStatusLabel, L"Deteniendo análisis...");
    }
}

void PopulateDrives() {
    SendMessageW(g_app.hDriveCombo, CB_RESETCONTENT, 0, 0);
    g_app.drives = xcaler::EnumerateDrives();

    for (const auto& d : g_app.drives) {
        std::wstring label = d.rootPath;
        if (!d.volumeLabel.empty()) {
            label += L" [" + d.volumeLabel + L"]";
        }
        if (d.totalBytes > 0) {
            label += L" — " + xcaler::FormatBytes(d.usedBytes) + L" / " + xcaler::FormatBytes(d.totalBytes);
        }
        SendMessageW(g_app.hDriveCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    if (!g_app.drives.empty()) {
        SendMessageW(g_app.hDriveCombo, CB_SETCURSEL, 0, 0);
        g_app.currentTargetPath = g_app.drives[0].rootPath;
        SetWindowTextW(g_app.hPathEdit, g_app.currentTargetPath.c_str());
    }
}

void BrowseForFolder() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_app.hMain;
    bi.lpszTitle = L"Selecciona la carpeta o unidad raíz que deseas analizar:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl != nullptr) {
        WCHAR pathBuf[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidl, pathBuf)) {
            StartScan(pathBuf);
        }
        CoTaskMemFree(pidl);
    }
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], needed, nullptr, nullptr);
    return out;
}

std::string EscapeCsv(const std::string& field) {
    if (field.find_first_of(",\"\r\n") == std::string::npos) {
        return field;
    }
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void ExportToCsv() {
    if (g_app.entries.empty()) {
        MessageBoxW(g_app.hMain, L"No hay datos para exportar todavía.", L"Exportar CSV", MB_ICONINFORMATION);
        return;
    }

    WCHAR fileBuf[MAX_PATH] = L"informe_disco.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.hMain;
    ofn.lpstrFilter = L"Archivos CSV (*.csv)\0*.csv\0Todos los archivos (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) {
        return;
    }

    HANDLE hFile = CreateFileW(fileBuf, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_app.hMain, L"No se pudo crear el archivo CSV.", L"Error", MB_ICONERROR);
        return;
    }

    // UTF-8 BOM for Excel compatibility
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(hFile, bom, sizeof(bom), &written, nullptr);

    std::string header = "Nombre,Ruta,Tamaño (Bytes),Tamaño Legible,Porcentaje,Archivos,Subcarpetas,Tipo\r\n";
    WriteFile(hFile, header.data(), static_cast<DWORD>(header.size()), &written, nullptr);

    const uint64_t denom = (g_app.totalScannedBytes > 0) ? g_app.totalScannedBytes : 1;
    for (const auto& e : g_app.entries) {
        double pct = (static_cast<double>(e.sizeBytes) * 100.0) / static_cast<double>(denom);
        char pctBuf[32]{};
        snprintf(pctBuf, sizeof(pctBuf), "%.2f%%", pct);

        std::wstring typeW = e.isDirectory ? (e.accessDenied ? L"Acceso denegado" : L"Carpeta") : L"Archivo";
        std::string line =
            EscapeCsv(WideToUtf8(e.name)) + "," +
            EscapeCsv(WideToUtf8(e.fullPath)) + "," +
            std::to_string(e.sizeBytes) + "," +
            EscapeCsv(WideToUtf8(xcaler::FormatBytes(e.sizeBytes))) + "," +
            pctBuf + "," +
            std::to_string(e.fileCount) + "," +
            std::to_string(e.dirCount) + "," +
            EscapeCsv(WideToUtf8(typeW)) + "\r\n";

        WriteFile(hFile, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    CloseHandle(hFile);
    MessageBoxW(g_app.hMain, L"Informe exportado correctamente.", L"Exportar CSV", MB_ICONINFORMATION);
}

void CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_app.hMain)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(WCHAR);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem != nullptr) {
        void* ptr = GlobalLock(hMem);
        if (ptr != nullptr) {
            memcpy(ptr, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

int GetSelectedEntryIndex() {
    int sel = ListView_GetNextItem(g_app.hListView, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    LVITEMW lvi{};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = sel;
    if (!ListView_GetItem(g_app.hListView, &lvi)) return -1;
    int idx = static_cast<int>(lvi.lParam);
    if (idx < 0 || static_cast<size_t>(idx) >= g_app.entries.size()) return -1;
    return idx;
}

void LayoutControls(int width, int height) {
    const int margin = 10;
    const int row1Y = 10;
    const int ctrlH = 26;

    const int comboW = 240;
    const int browseW = 90;
    const int upW = 65;
    const int scanW = 85;
    const int exportW = 105;
    const int gap = 6;

    int rightControlsW = browseW + upW + scanW + exportW + gap * 4;
    int pathX = margin + comboW + gap;
    int pathW = std::max(120, width - margin - rightControlsW - pathX);

    MoveWindow(g_app.hDriveCombo, margin, row1Y, comboW, 200, TRUE);
    MoveWindow(g_app.hPathEdit, pathX, row1Y, pathW, ctrlH, TRUE);

    int bx = pathX + pathW + gap;
    MoveWindow(g_app.hBrowseBtn, bx, row1Y, browseW, ctrlH, TRUE);
    bx += browseW + gap;
    MoveWindow(g_app.hUpBtn, bx, row1Y, upW, ctrlH, TRUE);
    bx += upW + gap;
    MoveWindow(g_app.hScanBtn, bx, row1Y, scanW, ctrlH, TRUE);
    bx += scanW + gap;
    MoveWindow(g_app.hExportBtn, bx, row1Y, exportW, ctrlH, TRUE);

    const int row2Y = row1Y + ctrlH + 8;
    const int progW = 180;
    const int summaryW = std::max(200, width - margin * 2 - progW - gap);
    MoveWindow(g_app.hSummaryLabel, margin, row2Y + 2, summaryW, 20, TRUE);
    MoveWindow(g_app.hProgressBar, margin + summaryW + gap, row2Y, progW, 20, TRUE);

    const int statusH = 22;
    const int listY = row2Y + 26;
    const int listH = std::max(100, height - listY - statusH - margin);
    MoveWindow(g_app.hListView, margin, listY, std::max(200, width - margin * 2), listH, TRUE);

    MoveWindow(g_app.hStatusLabel, margin, listY + listH + 4, std::max(200, width - margin * 2), statusH, TRUE);
}

void InitListViewColumns(HWND hList) {
    struct ColDef {
        const wchar_t* title;
        int width;
        int fmt;
    };
    const ColDef cols[] = {
        {L"Nombre", 250, LVCFMT_LEFT},
        {L"Tamaño", 105, LVCFMT_RIGHT},
        {L"% Uso", 70, LVCFMT_RIGHT},
        {L"Distribución", 170, LVCFMT_LEFT},
        {L"Archivos", 90, LVCFMT_RIGHT},
        {L"Subcarpetas", 90, LVCFMT_RIGHT},
        {L"Tipo", 130, LVCFMT_LEFT},
    };

    for (int i = 0; i < static_cast<int>(std::size(cols)); ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = cols[i].fmt;
        col.cx = cols[i].width;
        col.pszText = const_cast<LPWSTR>(cols[i].title);
        ListView_InsertColumn(hList, i, &col);
    }
}

LRESULT HandleListCustomDraw(LPNMLVCUSTOMDRAW lplvcd) {
    switch (lplvcd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            if (lplvcd->iSubItem != 3) {
                return CDRF_DODEFAULT;
            }

            const int itemIndex = static_cast<int>(lplvcd->nmcd.dwItemSpec);
            const size_t dataIndex = static_cast<size_t>(lplvcd->nmcd.lItemlParam);
            if (dataIndex >= g_app.entries.size()) {
                return CDRF_DODEFAULT;
            }

            RECT rc{};
            ListView_GetSubItemRect(g_app.hListView, itemIndex, 3, LVIR_BOUNDS, &rc);

            HDC hdc = lplvcd->nmcd.hdc;
            bool selected = (ListView_GetItemState(g_app.hListView, itemIndex, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            HBRUSH bgBrush = GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
            FillRect(hdc, &rc, bgBrush);

            const uint64_t denom = (g_app.totalScannedBytes > 0) ? g_app.totalScannedBytes : 1;
            double ratio = static_cast<double>(g_app.entries[dataIndex].sizeBytes) / static_cast<double>(denom);
            if (ratio < 0.0) ratio = 0.0;
            if (ratio > 1.0) ratio = 1.0;

            RECT barBg = rc;
            InflateRect(&barBg, -6, -3);
            if (barBg.right > barBg.left && barBg.bottom > barBg.top) {
                HBRUSH trackBrush = CreateSolidBrush(RGB(232, 236, 240));
                FillRect(hdc, &barBg, trackBrush);
                DeleteObject(trackBrush);

                int fillW = static_cast<int>(static_cast<double>(barBg.right - barBg.left) * ratio);
                if (fillW == 0 && g_app.entries[dataIndex].sizeBytes > 0) {
                    fillW = 2;
                }
                if (fillW > 0) {
                    RECT barFill = barBg;
                    barFill.right = barBg.left + fillW;

                    COLORREF barColor = RGB(46, 160, 67); // Green < 20%
                    if (ratio >= 0.50) {
                        barColor = RGB(218, 59, 59);    // Red >= 50%
                    } else if (ratio >= 0.20) {
                        barColor = RGB(227, 152, 34);   // Amber 20-50%
                    }

                    HBRUSH fillBrush = CreateSolidBrush(barColor);
                    FillRect(hdc, &barFill, fillBrush);
                    DeleteObject(fillBrush);
                }

                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(200, 206, 212));
                HGDIOBJ oldPen = SelectObject(hdc, borderPen);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, barBg.left, barBg.top, barBg.right, barBg.bottom);
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(borderPen);
            }

            return CDRF_SKIPDEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_app.hMain = hWnd;
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            NONCLIENTMETRICSW ncm{};
            ncm.cbSize = sizeof(ncm);
            if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
                g_app.hFont = CreateFontIndirectW(&ncm.lfMessageFont);
                ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
                g_app.hBoldFont = CreateFontIndirectW(&ncm.lfMessageFont);
            } else {
                g_app.hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                g_app.hBoldFont = g_app.hFont;
            }

            g_app.hDriveCombo = CreateWindowExW(
                0, WC_COMBOBOXW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_DRIVE_COMBO), hInst, nullptr);

            g_app.hPathEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_EDITW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_PATH_EDIT), hInst, nullptr);

            g_app.hBrowseBtn = CreateWindowExW(
                0, WC_BUTTONW, L"Examinar...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_BROWSE_BTN), hInst, nullptr);

            g_app.hUpBtn = CreateWindowExW(
                0, WC_BUTTONW, L"Subir",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_UP_BTN), hInst, nullptr);

            g_app.hScanBtn = CreateWindowExW(
                0, WC_BUTTONW, L"Analizar",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_SCAN_BTN), hInst, nullptr);

            g_app.hExportBtn = CreateWindowExW(
                0, WC_BUTTONW, L"Exportar CSV",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_EXPORT_BTN), hInst, nullptr);

            g_app.hSummaryLabel = CreateWindowExW(
                0, WC_STATICW, L"Selecciona una unidad o carpeta y pulsa Analizar.",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_SUMMARY_LABEL), hInst, nullptr);

            g_app.hProgressBar = CreateWindowExW(
                0, PROGRESS_CLASSW, nullptr,
                WS_CHILD | PBS_MARQUEE,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_PROGRESS_BAR), hInst, nullptr);

            g_app.hListView = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_LISTVIEW), hInst, nullptr);
            ListView_SetExtendedListViewStyle(
                g_app.hListView,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            InitListViewColumns(g_app.hListView);

            g_app.hStatusLabel = CreateWindowExW(
                0, WC_STATICW, L"Listo.",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                0, 0, 0, 0, hWnd, reinterpret_cast<HMENU>(IDC_STATUS_LABEL), hInst, nullptr);

            ApplyFont(g_app.hDriveCombo, g_app.hFont);
            ApplyFont(g_app.hPathEdit, g_app.hFont);
            ApplyFont(g_app.hBrowseBtn, g_app.hFont);
            ApplyFont(g_app.hUpBtn, g_app.hFont);
            ApplyFont(g_app.hScanBtn, g_app.hBoldFont);
            ApplyFont(g_app.hExportBtn, g_app.hFont);
            ApplyFont(g_app.hSummaryLabel, g_app.hBoldFont);
            ApplyFont(g_app.hListView, g_app.hFont);
            ApplyFont(g_app.hStatusLabel, g_app.hFont);

            PopulateDrives();

            if (!g_app.currentTargetPath.empty()) {
                PostMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_SCAN_BTN, BN_CLICKED), 0);
            }
            return 0;
        }

        case WM_SIZE: {
            LayoutControls(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 780;
            mmi->ptMinTrackSize.y = 480;
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);

            if (id == IDC_DRIVE_COMBO && code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(g_app.hDriveCombo, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && static_cast<size_t>(sel) < g_app.drives.size()) {
                    StartScan(g_app.drives[static_cast<size_t>(sel)].rootPath);
                }
            } else if (id == IDC_BROWSE_BTN) {
                BrowseForFolder();
            } else if (id == IDC_UP_BTN) {
                std::wstring parent = GetParentDirectory(g_app.currentTargetPath);
                if (!parent.empty() && parent != g_app.currentTargetPath) {
                    StartScan(parent);
                }
            } else if (id == IDC_SCAN_BTN) {
                if (g_app.isScanning.load()) {
                    StopScan();
                } else {
                    std::wstring path = GetWindowTextStr(g_app.hPathEdit);
                    StartScan(path);
                }
            } else if (id == IDC_EXPORT_BTN) {
                ExportToCsv();
            } else if (id == IDM_CTX_SCAN_FOLDER) {
                int idx = GetSelectedEntryIndex();
                if (idx >= 0 && g_app.entries[static_cast<size_t>(idx)].isDirectory) {
                    StartScan(g_app.entries[static_cast<size_t>(idx)].fullPath);
                }
            } else if (id == IDM_CTX_OPEN_EXPLORER) {
                int idx = GetSelectedEntryIndex();
                if (idx >= 0) {
                    const auto& e = g_app.entries[static_cast<size_t>(idx)];
                    if (e.isDirectory) {
                        ShellExecuteW(hWnd, L"open", e.fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    } else {
                        std::wstring args = L"/select,\"" + e.fullPath + L"\"";
                        ShellExecuteW(hWnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                }
            } else if (id == IDM_CTX_COPY_PATH) {
                int idx = GetSelectedEntryIndex();
                if (idx >= 0) {
                    CopyTextToClipboard(g_app.entries[static_cast<size_t>(idx)].fullPath);
                }
            }
            return 0;
        }

        case WM_NOTIFY: {
            auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->idFrom == IDC_LISTVIEW) {
                if (nmhdr->code == NM_CUSTOMDRAW) {
                    return HandleListCustomDraw(reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam));
                } else if (nmhdr->code == LVN_COLUMNCLICK) {
                    auto* pnmv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                    if (g_app.sortColumn == pnmv->iSubItem) {
                        g_app.sortAscending = !g_app.sortAscending;
                    } else {
                        g_app.sortColumn = pnmv->iSubItem;
                        g_app.sortAscending = (pnmv->iSubItem == 0 || pnmv->iSubItem == 6);
                    }
                    SortEntries();
                    RefreshListView();
                } else if (nmhdr->code == NM_DBLCLK) {
                    if (!g_app.isScanning.load()) {
                        int idx = GetSelectedEntryIndex();
                        if (idx >= 0 && g_app.entries[static_cast<size_t>(idx)].isDirectory) {
                            StartScan(g_app.entries[static_cast<size_t>(idx)].fullPath);
                        }
                    }
                } else if (nmhdr->code == NM_RCLICK) {
                    int idx = GetSelectedEntryIndex();
                    if (idx >= 0) {
                        HMENU hMenu = CreatePopupMenu();
                        if (g_app.entries[static_cast<size_t>(idx)].isDirectory && !g_app.isScanning.load()) {
                            AppendMenuW(hMenu, MF_STRING, IDM_CTX_SCAN_FOLDER, L"Analizar esta carpeta");
                        }
                        AppendMenuW(hMenu, MF_STRING, IDM_CTX_OPEN_EXPLORER, L"Abrir en el Explorador");
                        AppendMenuW(hMenu, MF_STRING, IDM_CTX_COPY_PATH, L"Copiar ruta");

                        POINT pt{};
                        GetCursorPos(&pt);
                        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
                        DestroyMenu(hMenu);
                    }
                }
            }
            return 0;
        }

        case WM_APP_PROGRESS: {
            std::unique_ptr<xcaler::ScanProgress> prog(reinterpret_cast<xcaler::ScanProgress*>(lParam));
            if (g_app.isScanning.load()) {
                std::wstring status = L"Escaneando (" + xcaler::FormatBytes(prog->scannedBytes) + L"): " + prog->currentPath;
                SetWindowTextW(g_app.hStatusLabel, status.c_str());
            }
            return 0;
        }

        case WM_APP_ENTRY_DONE: {
            std::unique_ptr<xcaler::ScanEntry> entry(reinterpret_cast<xcaler::ScanEntry*>(lParam));
            g_app.totalScannedBytes += entry->sizeBytes;
            g_app.entries.push_back(std::move(*entry));
            SortEntries();
            RefreshListView();
            UpdateSummaryText();
            return 0;
        }

        case WM_APP_SCAN_DONE: {
            g_app.isScanning.store(false);
            bool cancelled = g_app.cancelRequested.load();
            g_app.cancelRequested.store(false);

            SendMessageW(g_app.hProgressBar, PBM_SETMARQUEE, FALSE, 0);
            ShowWindow(g_app.hProgressBar, SW_HIDE);

            SetWindowTextW(g_app.hScanBtn, L"Analizar");
            EnableWindow(g_app.hDriveCombo, TRUE);
            EnableWindow(g_app.hBrowseBtn, TRUE);
            EnableWindow(g_app.hUpBtn, TRUE);
            EnableWindow(g_app.hExportBtn, TRUE);

            double elapsedSec = static_cast<double>(GetTickCount64() - g_app.scanStartTick) / 1000.0;
            WCHAR statusBuf[256]{};
            swprintf(
                statusBuf,
                256,
                L"%ls: %zu elementos (%ls) en %.1f s. Doble clic en una carpeta para explorar su interior.",
                cancelled ? L"Análisis detenido" : L"Análisis completado",
                g_app.entries.size(),
                xcaler::FormatBytes(g_app.totalScannedBytes).c_str(),
                elapsedSec);
            SetWindowTextW(g_app.hStatusLabel, statusBuf);
            UpdateSummaryText();
            return 0;
        }

        case WM_DESTROY: {
            g_app.cancelRequested.store(true);
            if (g_app.hWorkerThread != nullptr) {
                WaitForSingleObject(g_app.hWorkerThread, 2000);
                CloseHandle(g_app.hWorkerThread);
                g_app.hWorkerThread = nullptr;
            }
            if (g_app.hFont && g_app.hFont != GetStockObject(DEFAULT_GUI_FONT)) {
                DeleteObject(g_app.hFont);
            }
            if (g_app.hBoldFont && g_app.hBoldFont != g_app.hFont && g_app.hBoldFont != GetStockObject(DEFAULT_GUI_FONT)) {
                DeleteObject(g_app.hBoldFont);
            }
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    const wchar_t* kClassName = L"XcalerDiskAnalyzerWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        0,
        kClassName,
        L"Xcaler Disk Analyzer — Analizador de Uso de Disco",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 620,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
