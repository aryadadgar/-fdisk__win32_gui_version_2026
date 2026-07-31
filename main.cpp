// main.cpp - Win32 GUI front-end for the fdisk C++ port.
// Ports the *menu/command surface* of util-linux's fdisk.c onto GUI buttons
// and dialogs instead of a text prompt loop, driven by FdiskContext.
#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <memory>
#include <cstdint>

#include "../include/fdisk_context.h"
#include "../include/format_engine.h"
#include "../include/fmifs.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "rpcrt4.lib")

// ---- Control IDs --------------------------------------------------------
#define ID_DEVICE_COMBO     201
#define ID_LOG_EDIT         202
#define ID_BTN_GET_GEOM     203
#define ID_BTN_DUMP_FIRST   204
#define ID_BTN_CREATE_GPT   205
#define ID_BTN_CREATE_MBR   206
#define ID_BTN_ADD_PART     207
#define ID_BTN_DEL_PART     208
#define ID_BTN_CHANGE_TYPE  209
#define ID_BTN_TOGGLE_FLAG  210
#define ID_BTN_LIST_PART    211
#define ID_BTN_WRITE        212
#define ID_BTN_VERIFY       213
#define ID_BTN_CLEAR_LOG    214
#define ID_BTN_FORMAT_FAT32 215
#define ID_BTN_FORMAT_NTFS  216
#define ID_BTN_FORMAT_FAT32 215
#define ID_BTN_FORMAT_NTFS  216

// ---- Globals -------------------------------------------------------------
HWND g_hComboDevice = NULL;
HWND g_hEditLog     = NULL;
std::unique_ptr<FdiskContext> g_ctx;

// ---- Logging --------------------------------------------------------------
void AppendLog(const std::wstring& text) {
    if (!g_hEditLog) return;
    int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hEditLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g_hEditLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_hEditLog, EM_SCROLLCARET, 0, 0);
}

// ---- Simple modal text-input prompt (stand-in for fdisk's get_user_reply) -
static bool s_promptOk = false;
static std::wstring s_promptResult;

LRESULT CALLBACK PromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit, hOk, hCancel;
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        const wchar_t* label = reinterpret_cast<const wchar_t*>(cs->lpCreateParams);
        CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE, 10, 10, 320, 20, hwnd, NULL, NULL, NULL);
        hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 10, 35, 320, 24, hwnd, NULL, NULL, NULL);
        hOk = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                               170, 70, 75, 25, hwnd, (HMENU)1, NULL, NULL);
        hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                                   255, 70, 75, 25, hwnd, (HMENU)2, NULL, NULL);
        SetFocus(hEdit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) {
            wchar_t buf[256] = {0};
            GetWindowTextW(hEdit, buf, 256);
            s_promptResult = buf;
            s_promptOk = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wParam) == 2) {
            s_promptOk = false;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        s_promptOk = false;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Blocking modal prompt. Returns true + fills 'out' if user pressed OK.
bool PromptInput(HWND parent, const std::wstring& label, std::wstring& out) {
    static bool classRegistered = false;
    const wchar_t CLASS_NAME[] = L"FdiskPromptClass";
    if (!classRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PromptWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = CLASS_NAME;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        classRegistered = true;
    }

    s_promptOk = false;
    s_promptResult.clear();

    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME, L"Input",
                                 WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 360, 145,
                                 parent, NULL, GetModuleHandle(NULL), (LPVOID)label.c_str());
    EnableWindow(parent, FALSE);
    ShowWindow(hDlg, SW_SHOW);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hDlg)) break;
    }
    EnableWindow(parent, TRUE);
    SetFocus(parent);

    out = s_promptResult;
    return s_promptOk;
}

// ---- Device selection ------------------------------------------------------
std::wstring GetTargetDevice() {
    if (!g_hComboDevice) return L"\\\\.\\PhysicalDrive0";
    int selIndex = (int)SendMessageW(g_hComboDevice, CB_GETCURSEL, 0, 0);
    if (selIndex != CB_ERR) {
        wchar_t buf[256] = {0};
        SendMessageW(g_hComboDevice, CB_GETLBTEXT, (WPARAM)selIndex, (LPARAM)buf);
        return std::wstring(buf);
    }
    wchar_t buf[256] = {0};
    GetWindowTextW(g_hComboDevice, buf, 256);
    if (wcslen(buf) > 0) return std::wstring(buf);
    return L"\\\\.\\PhysicalDrive0";
}

void PopulateAvailableDisks() {
    SendMessageW(g_hComboDevice, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 16; ++i) {
        std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);
        HANDLE hDevice = CreateFileW(devPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice != INVALID_HANDLE_VALUE) {
            CloseHandle(hDevice);
            SendMessageW(g_hComboDevice, CB_ADDSTRING, 0, (LPARAM)devPath.c_str());
        }
    }
    if (SendMessageW(g_hComboDevice, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(g_hComboDevice, CB_SETCURSEL, 0, 0);
    } else {
        SendMessageW(g_hComboDevice, CB_ADDSTRING, 0, (LPARAM)L"\\\\.\\PhysicalDrive0");
        SendMessageW(g_hComboDevice, CB_SETCURSEL, 0, 0);
    }
}

// Ensures g_ctx is assigned to the currently selected device (read-write).
bool EnsureAssigned(bool writeAccess) {
    std::wstring dev = GetTargetDevice();
    if (g_ctx && g_ctx->IsAssigned() && g_ctx->DevicePath() == dev) return true;

    g_ctx = std::make_unique<FdiskContext>(AppendLog);
    FdiskStatus st = g_ctx->AssignDevice(dev, writeAccess);
    if (st == FdiskStatus::ACCESS_DENIED) {
        AppendLog(L"[ERROR] Access denied opening " + dev + L". Run as Administrator.\r\n");
        g_ctx.reset();
        return false;
    }
    if (st == FdiskStatus::IO_ERROR) {
        AppendLog(L"[ERROR] Failed to open " + dev + L".\r\n");
        g_ctx.reset();
        return false;
    }
    if (st == FdiskStatus::NO_LABEL) {
        AppendLog(L"[INFO] " + dev + L" opened; no recognized partition table (use Create GPT/MBR).\r\n");
    } else if (st == FdiskStatus::OK) {
        AppendLog(L"[OK] " + dev + L" opened; detected " +
                  (g_ctx->CurrentLabelType() == LabelType::GPT ? L"GPT" : L"MBR") + L" label.\r\n");
    }
    return true;
}

// ---- Commands ---------------------------------------------------------------
void CmdDumpFirstSector() {
    std::wstring devPath = GetTargetDevice();
    if (!EnsureAssigned(false)) return;

    std::vector<uint8_t> sector;
    if (g_ctx->ReadSector0(sector) != FdiskStatus::OK) {
        AppendLog(L"[ERROR] Failed to read sector 0.\r\n");
        return;
    }

    std::wstringstream ss;
    ss << L"\r\n=== FIRST SECTOR DUMP (" << devPath << L") ===\r\n";
    for (size_t i = 0; i < sector.size(); i += 16) {
        ss << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0') << i << L"  ";
        for (size_t j = 0; j < 16 && i + j < sector.size(); ++j) {
            ss << std::setw(2) << static_cast<int>(sector[i + j]) << L" ";
            if (j == 7) ss << L" ";
        }
        ss << L"\r\n";
    }
    ss << L"===========================================\r\n\r\n";
    AppendLog(ss.str());
}

void CmdGetGeometry() {
    if (!EnsureAssigned(false)) return;
    const DiskGeometry& g = g_ctx->Geometry();
    ULONGLONG diskSize = g.totalSectors * (ULONGLONG)g.bytesPerSector;

    std::wstringstream ss;
    ss << L"\r\n=== DISK GEOMETRY INFORMATION ===\r\n"
       << L" Device: " << g_ctx->DevicePath() << L"\r\n"
       << L" Bytes Per Sector: " << g.bytesPerSector << L"\r\n"
       << L" Heads: " << g.heads << L"  Sectors/Track: " << g.sectorsPerTrack
       << L"  Cylinders: " << g.cylinders << L"\r\n"
       << L" Total Sectors: " << g.totalSectors << L"\r\n"
       << L" Total Disk Capacity: " << (diskSize / (1024ULL * 1024 * 1024)) << L" GB ("
       << diskSize << L" bytes)\r\n"
       << L" Current label: " << (g_ctx->HasLabel()
             ? (g_ctx->CurrentLabelType() == LabelType::GPT ? L"GPT" : L"MBR")
             : L"(none)") << L"\r\n"
       << L"=================================\r\n\r\n";
    AppendLog(ss.str());
}

void CmdListPartitions() {
    if (!EnsureAssigned(false)) return;
    if (!g_ctx->HasLabel()) { AppendLog(L"[INFO] No partition table on this device.\r\n"); return; }

    auto* label = g_ctx->Label();
    std::wstringstream ss;
    ss << L"\r\n=== PARTITION TABLE (" << (label->Type() == LabelType::GPT ? L"GPT" : L"MBR") << L") ===\r\n";
    for (const auto& p : label->Partitions()) {
        ss << L" [" << (p.index + 1) << L"] "
           << L"Start=" << p.startLBA << L" End=" << p.endLBA
           << L" Sectors=" << p.sizeSectors
           << L" Type=" << p.typeName;
        if (p.bootable) ss << L" [BOOT]";
        if (!p.name.empty()) ss << L" Name=\"" << p.name << L"\"";
        ss << L"\r\n";
    }
    ss << L"=========================================\r\n\r\n";
    AppendLog(ss.str());
}

void CmdCreateLabel(HWND hwnd, LabelType type) {
    const wchar_t* typeName = (type == LabelType::GPT) ? L"GPT" : L"MBR";
    std::wstring msg = L"This will DISCARD any existing partition table on the selected disk "
                        L"and create a new empty " + std::wstring(typeName) + L" label.\r\n\r\n"
                        L"This is only written to disk when you press Write.\r\nProceed?";
    if (MessageBoxW(hwnd, msg.c_str(), L"Create Partition Table", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    if (!EnsureAssigned(true)) return;

    FdiskStatus st = g_ctx->CreateDisklabel(type);
    if (st == FdiskStatus::OK)
        AppendLog(L"[OK] New empty " + std::wstring(typeName) + L" label created in memory. Use Write to commit.\r\n");
    else
        AppendLog(L"[ERROR] Failed to create label: " + StatusToString(st) + L"\r\n");
}

void CmdAddPartition(HWND hwnd) {
    if (!EnsureAssigned(true)) return;
    if (!g_ctx->HasLabel()) { AppendLog(L"[ERROR] Create a GPT or MBR label first.\r\n"); return; }

    std::wstring startStr, sizeStr, typeStr;
    if (!PromptInput(hwnd, L"Start LBA (sector number):", startStr)) return;
    if (!PromptInput(hwnd, L"Size in sectors:", sizeStr)) return;

    bool isGpt = (g_ctx->Label()->Type() == LabelType::GPT);
    if (!PromptInput(hwnd, isGpt ? L"Type GUID (or name, e.g. \"Microsoft basic data\"):"
                                  : L"Type code hex (e.g. 07 for NTFS, 0B for FAT32):", typeStr))
        return;

    uint64_t startLBA = _wcstoui64(startStr.c_str(), nullptr, 10);
    uint64_t sizeSectors = _wcstoui64(sizeStr.c_str(), nullptr, 10);

    PartitionEntry pe;
    FdiskStatus st = g_ctx->Label()->AddPartition(startLBA, sizeSectors, typeStr, &pe);
    if (st == FdiskStatus::OK)
        AppendLog(L"[OK] Partition added (not yet written to disk). Use Write to commit.\r\n");
    else
        AppendLog(L"[ERROR] AddPartition failed: " + StatusToString(st) + L"\r\n");
}

void CmdDeletePartition(HWND hwnd) {
    if (!EnsureAssigned(true) || !g_ctx->HasLabel()) return;
    std::wstring idxStr;
    if (!PromptInput(hwnd, L"Partition number to delete (as shown in List):", idxStr)) return;
    int idx = _wtoi(idxStr.c_str()) - 1;
    FdiskStatus st = g_ctx->Label()->DeletePartition(idx);
    if (st == FdiskStatus::OK)
        AppendLog(L"[OK] Partition marked for deletion. Use Write to commit.\r\n");
    else
        AppendLog(L"[ERROR] DeletePartition failed: " + StatusToString(st) + L"\r\n");
}

void CmdChangeType(HWND hwnd) {
    if (!EnsureAssigned(true) || !g_ctx->HasLabel()) return;
    std::wstring idxStr, typeStr;
    if (!PromptInput(hwnd, L"Partition number:", idxStr)) return;
    bool isGpt = (g_ctx->Label()->Type() == LabelType::GPT);
    if (!PromptInput(hwnd, isGpt ? L"New type GUID or name:" : L"New type code hex:", typeStr)) return;
    int idx = _wtoi(idxStr.c_str()) - 1;
    FdiskStatus st = g_ctx->Label()->ChangeType(idx, typeStr);
    if (st == FdiskStatus::OK)
        AppendLog(L"[OK] Partition type changed (pending write).\r\n");
    else
        AppendLog(L"[ERROR] ChangeType failed: " + StatusToString(st) + L"\r\n");
}

void CmdToggleFlag(HWND hwnd) {
    if (!EnsureAssigned(true) || !g_ctx->HasLabel()) return;
    std::wstring idxStr, flagStr;
    if (!PromptInput(hwnd, L"Partition number:", idxStr)) return;
    bool isGpt = (g_ctx->Label()->Type() == LabelType::GPT);
    if (!PromptInput(hwnd, isGpt ? L"Flag (boot/ro/hidden/noautomount/legacyboot):" : L"Flag (boot):", flagStr))
        return;
    int idx = _wtoi(idxStr.c_str()) - 1;
    FdiskStatus st = g_ctx->Label()->TogglePartitionFlag(idx, flagStr);
    if (st == FdiskStatus::OK)
        AppendLog(L"[OK] Flag toggled (pending write).\r\n");
    else
        AppendLog(L"[ERROR] ToggleFlag failed: " + StatusToString(st) + L"\r\n");
}

void CmdVerify() {
    if (!EnsureAssigned(false) || !g_ctx->HasLabel()) { AppendLog(L"[INFO] No label to verify.\r\n"); return; }
    AppendLog(L"\r\n=== VERIFY ===\r\n" + g_ctx->Label()->VerifyReport() + L"==============\r\n\r\n");
}

void CmdWrite(HWND hwnd) {
    if (!g_ctx || !g_ctx->HasLabel()) { AppendLog(L"[ERROR] Nothing to write.\r\n"); return; }
    int result = MessageBoxW(hwnd,
        L"This will WRITE the in-memory partition table to the physical disk.\r\n"
        L"All previous partition data on affected sectors will be overwritten.\r\n\r\n"
        L"Proceed?", L"Write Partition Table", MB_YESNO | MB_ICONWARNING);
    if (result != IDYES) return;

    FdiskStatus st = g_ctx->WriteLabel();
    if (st == FdiskStatus::OK) {
        AppendLog(L"[SUCCESS] Partition table written to disk (on-disk structures).\r\n");

        // Tell Windows about the new layout the native way. Only meaningful
        // for MBR here - see CommitLayoutViaIoctl()'s comment for why.
        FdiskStatus ioctlSt = g_ctx->CommitLayoutViaIoctl();
        if (ioctlSt == FdiskStatus::OK)
            AppendLog(L"[OK] IOCTL_DISK_SET_DRIVE_LAYOUT_EX accepted the new layout.\r\n");
        else if (ioctlSt == FdiskStatus::NOT_SUPPORTED)
            AppendLog(L"[INFO] GPT layout committed via raw sectors + IOCTL_DISK_UPDATE_PROPERTIES.\r\n");
        else
            AppendLog(L"[WARN] IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed (" + StatusToString(ioctlSt) +
                      L"); on-disk table is still correct, Windows may need a rescan/reboot to see it.\r\n");
    } else if (st == FdiskStatus::ACCESS_DENIED) {
        AppendLog(L"[ERROR] Write access denied. Reopen as Administrator (device was opened read-only).\r\n");
    } else {
        AppendLog(L"[ERROR] Write failed: " + StatusToString(st) + L"\r\n");
    }
}

// ---- Real filesystem formatting via fmifs.dll's FormatEx -----------------
// This is the same undocumented-but-stable API rundll32 shell32,SHFormatDrive
// and Explorer's own Format dialog ultimately call into.
BOOLEAN WINAPI FormatCallback(FmifsPacketType command, DWORD subAction, PVOID data) {
    switch (command) {
    case FmifsPacketType::Percent:
        // data points to a DWORD percent-complete value.
        if (data) {
            DWORD pct = *reinterpret_cast<DWORD*>(data);
            static DWORD lastReported = 0xFFFFFFFF;
            if (pct != lastReported && pct % 10 == 0) {
                AppendLog(L"[FORMAT] " + std::to_wstring(pct) + L"%\r\n");
                lastReported = pct;
            }
        }
        break;
    case FmifsPacketType::FinishedFormat:
        AppendLog(L"[FORMAT] FinishedFormat callback received.\r\n");
        break;
    case FmifsPacketType::IncompatibleFileSystem:
        AppendLog(L"[FORMAT ERROR] Incompatible file system for this volume/media.\r\n");
        break;
    case FmifsPacketType::AccessDenied:
        AppendLog(L"[FORMAT ERROR] Access denied — run as Administrator.\r\n");
        break;
    case FmifsPacketType::MediaWriteProtected:
        AppendLog(L"[FORMAT ERROR] Media is write-protected.\r\n");
        break;
    case FmifsPacketType::IoError:
        AppendLog(L"[FORMAT ERROR] I/O error during format.\r\n");
        break;
    case FmifsPacketType::DeviceNotReady:
        AppendLog(L"[FORMAT ERROR] Device not ready.\r\n");
        break;
    case FmifsPacketType::ClusterSizeTooBig:
        AppendLog(L"[FORMAT ERROR] Requested cluster size too big for this file system.\r\n");
        break;
    case FmifsPacketType::ClusterSizeTooSmall:
        AppendLog(L"[FORMAT ERROR] Requested cluster size too small for this file system.\r\n");
        break;
    default:
        break;
    }
    return TRUE; // TRUE = continue formatting
}

void CmdFormatVolume(HWND hwnd, const wchar_t* fsName) {
    std::wstring volInput;
    if (!PromptInput(hwnd, L"Volume to format, e.g. D:  (must be a mounted drive letter, not \\\\.\\PhysicalDriveN):", volInput))
        return;
    if (volInput.empty()) return;
    if (volInput.back() != L'\\') {
        if (volInput.back() != L':') volInput += L':';
        volInput += L'\\';
    }

    std::wstring warnMsg = L"WARNING: This will ERASE ALL DATA on volume " + volInput +
                            L" and format it as " + fsName + L".\r\n\r\nProceed?";
    if (MessageBoxW(hwnd, warnMsg.c_str(), L"Format Volume", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    std::wstring labelInput;
    PromptInput(hwnd, L"Volume label (optional, leave blank for none):", labelInput);

    FmifsFormatter formatter;
    if (!formatter.Load()) {
        AppendLog(L"[ERROR] Could not load fmifs.dll / locate FormatEx — formatting unavailable on this system.\r\n");
        return;
    }

    AppendLog(L"[ACTION] Calling FormatEx(" + volInput + L", " + fsName + L") — this runs synchronously...\r\n");
    bool called = formatter.Format(volInput, fsName, labelInput, /*quick=*/true, FormatCallback);
    if (called)
        AppendLog(L"[SUCCESS] FormatEx call completed for " + volInput + L".\r\n");
    else
        AppendLog(L"[ERROR] FormatEx entry point not available.\r\n");
}

void CmdFormatFAT32(HWND hwnd) { CmdFormatVolume(hwnd, L"FAT32"); }
void CmdFormatNTFS(HWND hwnd)  { CmdFormatVolume(hwnd, L"NTFS"); }

// Real format: locks/dismounts/FormatEx's a volume by drive letter, via
// FormatEngine (FSCTL_LOCK_VOLUME -> FSCTL_DISMOUNT_VOLUME -> fmifs!FormatEx
// -> FSCTL_UNLOCK_VOLUME).
void CmdFormatVolume(HWND hwnd, const std::wstring& fsName) {
    std::wstring driveStr;
    if (!PromptInput(hwnd, L"Drive letter to format (e.g. D):", driveStr)) return;
    if (driveStr.empty()) { AppendLog(L"[ERROR] No drive letter given.\r\n"); return; }
    wchar_t driveLetter = towupper(driveStr[0]);
    if (driveLetter < L'A' || driveLetter > L'Z') {
        AppendLog(L"[ERROR] Invalid drive letter.\r\n");
        return;
    }
    if (driveLetter == L'C') {
        AppendLog(L"[ERROR] Refusing to format the system drive from here.\r\n");
        return;
    }

    std::wstring labelStr;
    PromptInput(hwnd, L"Volume label (optional):", labelStr); // ok if user cancels -> empty label

    std::wstring msg = L"WARNING: This will ERASE ALL DATA on " + std::wstring(1, driveLetter) +
                        L":\\ and format it as " + fsName + L".\r\n\r\nProceed?";
    if (MessageBoxW(hwnd, msg.c_str(), L"Format Volume", MB_YESNO | MB_ICONWARNING) != IDYES) return;

    FormatEngine fmt(AppendLog);
    FdiskStatus st = fmt.Format(driveLetter, fsName, labelStr, /*quickFormat=*/true);
    if (st != FdiskStatus::OK)
        AppendLog(L"[ERROR] Format did not complete successfully: " + StatusToString(st) + L"\r\n");
}

// ---- Window procedure ---------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hMonospace = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        CreateWindowExW(0, L"STATIC", L"Target Device / Disk Path:", WS_CHILD | WS_VISIBLE, 15, 15, 200, 20, hwnd, NULL, NULL, NULL);
        g_hComboDevice = CreateWindowExW(0, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            15, 35, 220, 200, hwnd, (HMENU)ID_DEVICE_COMBO, NULL, NULL);

        struct BtnDef { int id; const wchar_t* label; int x; int w; };
        BtnDef row1[] = {
            { ID_BTN_GET_GEOM,    L"Geometry",     245, 85 },
            { ID_BTN_DUMP_FIRST,  L"Dump Sec0",    335, 85 },
            { ID_BTN_LIST_PART,   L"List Parts",   425, 85 },
            { ID_BTN_VERIFY,      L"Verify",       515, 85 },
            { ID_BTN_CLEAR_LOG,   L"Clear Log",    605, 85 },
        };
        BtnDef row2[] = {
            { ID_BTN_CREATE_GPT,  L"Create GPT",    15, 100 },
            { ID_BTN_CREATE_MBR,  L"Create MBR",    120, 100 },
            { ID_BTN_ADD_PART,    L"Add Partition", 225, 110 },
            { ID_BTN_DEL_PART,    L"Delete Part",   340, 100 },
            { ID_BTN_CHANGE_TYPE, L"Change Type",   445, 100 },
            { ID_BTN_TOGGLE_FLAG, L"Toggle Flag",   550, 100 },
            { ID_BTN_WRITE,       L"WRITE",         655, 90 },
            { ID_BTN_FORMAT_FAT32,L"Format FAT32",  750, 100 },
            { ID_BTN_FORMAT_NTFS, L"Format NTFS",   855, 95 },
        };

        std::vector<HWND> allButtons;
        for (auto& b : row1) {
            HWND h = CreateWindowExW(0, L"BUTTON", b.label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      b.x, 35, b.w, 25, hwnd, (HMENU)(INT_PTR)b.id, NULL, NULL);
            allButtons.push_back(h);
        }
        for (auto& b : row2) {
            HWND h = CreateWindowExW(0, L"BUTTON", b.label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      b.x, 65, b.w, 28, hwnd, (HMENU)(INT_PTR)b.id, NULL, NULL);
            allButtons.push_back(h);
        }

        g_hEditLog = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT",
            L"fdisk C++ port initialized (GPT + MBR engine, real IOCTL + FormatEx).\r\n"
            L"----------------------------------------------------------------------------------\r\n",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            15, 105, 1015, 440, hwnd, (HMENU)ID_LOG_EDIT, NULL, NULL);

        SendMessageW(g_hComboDevice, WM_SETFONT, (WPARAM)hFont, TRUE);
        for (HWND h : allButtons) SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)hMonospace, TRUE);

        PopulateAvailableDisks();
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case ID_BTN_GET_GEOM:    CmdGetGeometry(); break;
        case ID_BTN_DUMP_FIRST:  CmdDumpFirstSector(); break;
        case ID_BTN_LIST_PART:   CmdListPartitions(); break;
        case ID_BTN_VERIFY:      CmdVerify(); break;
        case ID_BTN_CREATE_GPT:  CmdCreateLabel(hwnd, LabelType::GPT); break;
        case ID_BTN_CREATE_MBR:  CmdCreateLabel(hwnd, LabelType::MBR); break;
        case ID_BTN_ADD_PART:    CmdAddPartition(hwnd); break;
        case ID_BTN_DEL_PART:    CmdDeletePartition(hwnd); break;
        case ID_BTN_CHANGE_TYPE: CmdChangeType(hwnd); break;
        case ID_BTN_TOGGLE_FLAG: CmdToggleFlag(hwnd); break;
        case ID_BTN_WRITE:       CmdWrite(hwnd); break;
        case ID_BTN_FORMAT_FAT32:CmdFormatFAT32(hwnd); break;
        case ID_BTN_FORMAT_NTFS: CmdFormatNTFS(hwnd); break;
        case ID_BTN_CLEAR_LOG:   SetWindowTextW(g_hEditLog, L""); break;
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    InitCommonControls();
    const wchar_t CLASS_NAME[] = L"FdiskCppPortClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"fdisk C++ Port - Disk Partition Manager (Win32 GUI)",
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1060, 600, NULL, NULL, hInstance, NULL);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

int main(int argc, char* argv[]) {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    return wWinMain(hInstance, NULL, GetCommandLineW(), SW_SHOWDEFAULT);
}
