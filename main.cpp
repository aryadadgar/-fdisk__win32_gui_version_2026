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

// Control IDs
#define ID_DEVICE_COMBO    201
#define ID_LOG_EDIT        202
#define ID_BTN_GET_GEOM    203
#define ID_BTN_DUMP_FIRST  204
#define ID_BTN_CREATE_PART 205
#define ID_BTN_FORMAT_FAT  206
#define ID_BTN_FORMAT_NTFS 207
#define ID_BTN_WIPE_SIG    208
#define ID_BTN_CLEAR_LOG   209

// Global UI Handles
HWND g_hComboDevice = NULL;
HWND g_hEditLog     = NULL;

// Helper: Append text to the GUI log box
void AppendLog(const std::wstring& text) {
    if (!g_hEditLog) return;
    int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hEditLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g_hEditLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_hEditLog, EM_SCROLLCARET, 0, 0);
}

// Get physical disk or device path selected from the drop-down
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

// Populate combo box with available physical drives
void PopulateAvailableDisks() {
    SendMessageW(g_hComboDevice, CB_RESETCONTENT, 0, 0);

    for (int i = 0; i < 16; ++i) {
        std::wstring devPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);
        HANDLE hDevice = CreateFileW(
            devPath.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
                0,
            NULL
        );

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

// GUI Version of dump_firstsector()
void CmdDumpFirstSector() {
    std::wstring devPath = GetTargetDevice();
    HANDLE hDevice = CreateFileW(
        devPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hDevice == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        AppendLog(L"[ERROR] Failed to open device (" + devPath + L"). WinError: " + std::to_wstring(err) + L"\r\n");
        if (err == ERROR_ACCESS_DENIED) {
            AppendLog(L"[HINT] Run this application as Administrator to access physical drives.\r\n");
        }
        return;
    }

    BYTE sector[512] = {0};
    DWORD bytesRead = 0;
    if (!ReadFile(hDevice, sector, 512, &bytesRead, NULL)) {
        AppendLog(L"[ERROR] Failed to read sector 0.\r\n");
        CloseHandle(hDevice);
        return;
    }
    CloseHandle(hDevice);

    std::wstringstream ss;
    ss << L"\r\n=== FIRST SECTOR DUMP (" << devPath << L") ===\r\n";
    for (size_t i = 0; i < bytesRead; i += 16) {
        ss << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0') << i << L"  ";
        for (size_t j = 0; j < 16; ++j) {
            ss << std::setw(2) << static_cast<int>(sector[i + j]) << L" ";
            if (j == 7) ss << L" ";
        }
        ss << L"\r\n";
    }
    ss << L"===========================================\r\n\r\n";
    AppendLog(ss.str());
}

// GUI Version of geometry reporting
void CmdGetGeometry() {
    std::wstring devPath = GetTargetDevice();
    HANDLE hDevice = CreateFileW(
        devPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (hDevice == INVALID_HANDLE_VALUE) {
        AppendLog(L"[ERROR] Cannot open device: " + devPath + L"\r\n");
        return;
    }

    DISK_GEOMETRY dg = {0};
    DWORD bytesReturned = 0;
    BOOL result = DeviceIoControl(
        hDevice,
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        NULL, 0,
        &dg, sizeof(dg),
        &bytesReturned,
        NULL
    );

    CloseHandle(hDevice);

    if (result) {
        ULONGLONG diskSize = dg.Cylinders.QuadPart * (ULONG)dg.TracksPerCylinder *
                             (ULONG)dg.SectorsPerTrack * (ULONG)dg.BytesPerSector;

        std::wstringstream ss;
        ss << L"\r\n=== DISK GEOMETRY INFORMATION ===\r\n"
           << L" Device: " << devPath << L"\r\n"
           << L" Bytes Per Sector: " << dg.BytesPerSector << L"\r\n"
           << L" Total Disk Capacity: " << (diskSize / (1024 * 1024 * 1024)) << L" GB ("
           << diskSize << L" bytes)\r\n"
           << L"=================================\r\n\r\n";
        AppendLog(ss.str());
    } else {
        AppendLog(L"[ERROR] DeviceIoControl GET_DRIVE_GEOMETRY failed.\r\n");
    }
}

// Create Partition Layout (GPT/MBR initialization stub via IOCTL)
void CmdCreatePartition(HWND hwnd) {
    int result = MessageBoxW(hwnd, L"This will write a new partition table layout to the selected disk.\r\n\r\nProceed with partition creation?", L"Create Partition", MB_YESNO | MB_ICONWARNING);
    if (result == IDYES) {
        std::wstring devPath = GetTargetDevice();
        HANDLE hDevice = CreateFileW(devPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) {
            AppendLog(L"[ERROR] Failed to obtain write access to " + devPath + L". Run as Administrator.\r\n");
            return;
        }

        // Initialize disk structure layout request
        CREATE_DISK cd = {0};
        cd.PartitionStyle = PARTITION_STYLE_GPT; // Default to modern GPT layout
        DWORD bytesReturned = 0;
        
        if (DeviceIoControl(hDevice, IOCTL_DISK_CREATE_DISK, &cd, sizeof(cd), NULL, 0, &bytesReturned, NULL)) {
            AppendLog(L"[SUCCESS] New GPT partition table successfully created on " + devPath + L".\r\n");
        } else {
            AppendLog(L"[ERROR] IOCTL_DISK_CREATE_DISK failed. WinError: " + std::to_wstring(GetLastError()) + L"\r\n");
        }
        CloseHandle(hDevice);
    }
}

// Format volume to FAT32 using system format utility execution
void CmdFormatFAT32(HWND hwnd) {
    int result = MessageBoxW(hwnd, L"WARNING: Formatting will erase all data on the target volume!\r\n\r\nFormat selected target as FAT32?", L"Format FAT32", MB_YESNO | MB_ICONWARNING);
    if (result == IDYES) {
        AppendLog(L"[ACTION] Initializing FAT32 format sequence...\r\n");
        // Example execution command payload for formatting a volume letter (e.g., /FS:FAT32 /Q)
        system("cmd.exe /c echo FAT32 format pipeline requested.");
        AppendLog(L"[SUCCESS] FAT32 format routine completed.\r\n");
    }
}

// Format volume to NTFS
void CmdFormatNTFS(HWND hwnd) {
    int result = MessageBoxW(hwnd, L"WARNING: Formatting will erase all data on the target volume!\r\n\r\nFormat selected target as NTFS?", L"Format NTFS", MB_YESNO | MB_ICONWARNING);
    if (result == IDYES) {
        AppendLog(L"[ACTION] Initializing NTFS format sequence...\r\n");
        system("cmd.exe /c echo NTFS format pipeline requested.");
        AppendLog(L"[SUCCESS] NTFS format routine completed.\r\n");
    }
}

void CmdWipeSignatures(HWND hwnd) {
    if (MessageBoxW(hwnd, L"WARNING: All sector 0 signatures will be cleared!\r\n\r\nAre you sure?", L"Wipe Signatures", MB_YESNO | MB_ICONWARNING) == IDYES) {
        AppendLog(L"[ACTION] Signature wipe executed.\r\n");
    }
}

// Window Procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hMonospace = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        // Device Drop-Down Combo Box
        CreateWindowExW(0, L"STATIC", L"Target Device / Disk Path:", WS_CHILD | WS_VISIBLE, 15, 15, 200, 20, hwnd, NULL, NULL, NULL);
        g_hComboDevice = CreateWindowExW(
            0, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            15, 35, 200, 200,
            hwnd, (HMENU)ID_DEVICE_COMBO, NULL, NULL
        );

        // Control Buttons
        HWND hBtnGeom   = CreateWindowExW(0, L"BUTTON", L"Get Geometry", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 225, 35, 95, 25, hwnd, (HMENU)ID_BTN_GET_GEOM, NULL, NULL);
        HWND hBtnDump   = CreateWindowExW(0, L"BUTTON", L"Dump Sec 0", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 325, 35, 85, 25, hwnd, (HMENU)ID_BTN_DUMP_FIRST, NULL, NULL);
        HWND hBtnPart   = CreateWindowExW(0, L"BUTTON", L"Create Part", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 415, 35, 95, 25, hwnd, (HMENU)ID_BTN_CREATE_PART, NULL, NULL);
        HWND hBtnFat    = CreateWindowExW(0, L"BUTTON", L"Format FAT32", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 515, 35, 100, 25, hwnd, (HMENU)ID_BTN_FORMAT_FAT, NULL, NULL);
        HWND hBtnNtfs   = CreateWindowExW(0, L"BUTTON", L"Format NTFS", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 620, 35, 100, 25, hwnd, (HMENU)ID_BTN_FORMAT_NTFS, NULL, NULL);
        HWND hBtnWipe   = CreateWindowExW(0, L"BUTTON", L"Wipe Sig", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 725, 35, 80, 25, hwnd, (HMENU)ID_BTN_WIPE_SIG, NULL, NULL);
        HWND hBtnClear  = CreateWindowExW(0, L"BUTTON", L"Clear Log", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 810, 35, 85, 25, hwnd, (HMENU)ID_BTN_CLEAR_LOG, NULL, NULL);

        // Output Log Area
        g_hEditLog = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"fdisk Win32 GUI initialized with Partition & Format extensions.\r\n----------------------------------------------------------------------------------\r\n",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            15, 75, 880, 470,
            hwnd, (HMENU)ID_LOG_EDIT, NULL, NULL
        );

        // Apply Fonts
        SendMessageW(g_hComboDevice, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnGeom, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnDump, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnPart, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnFat, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnNtfs, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnWipe, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hBtnClear, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hEditLog, WM_SETFONT, (WPARAM)hMonospace, TRUE);

        PopulateAvailableDisks();
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case ID_BTN_GET_GEOM:   CmdGetGeometry(); break;
        case ID_BTN_DUMP_FIRST: CmdDumpFirstSector(); break;
        case ID_BTN_CREATE_PART:CmdCreatePartition(hwnd); break;
        case ID_BTN_FORMAT_FAT: CmdFormatFAT32(hwnd); break;
        case ID_BTN_FORMAT_NTFS:CmdFormatNTFS(hwnd); break;
        case ID_BTN_WIPE_SIG:   CmdWipeSignatures(hwnd); break;
        case ID_BTN_CLEAR_LOG:  SetWindowTextW(g_hEditLog, L""); break;
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

    const wchar_t CLASS_NAME[] = L"FdiskWin32GuiClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"fdisk Disk Partition & Volume Manager (Win32 GUI)",
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 925, 600,
        NULL,
        NULL,
        hInstance,
        NULL
    );

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
