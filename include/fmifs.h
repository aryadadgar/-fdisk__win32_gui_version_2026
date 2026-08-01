// fmifs.h - minimal declarations for fmifs.dll's FormatEx entry point.
// This is the same DLL/function rundll32.exe and Explorer's Format dialog
// use internally. Not in the public Windows SDK headers, so we declare the
// ABI ourselves and load it dynamically with LoadLibrary/GetProcAddress.
#pragma once
#include <windows.h>
#include <string>

enum class FmifsMediaType : DWORD {
    Unknown,
    F5_160_512, F5_180_512, F5_320_512, F5_320_1024, F5_360_512,
    F5_640_512, F5_640_1024, F3_720_512, F5_1Pt2_512, F3_1Pt44_512,
    F3_2Pt88_512, F3_20Pt8_512, F3_720_1024, F5_360_1024, F3_1Pt2_512,
    F3_1Pt23_1024, F3_128Mb_512, F3_230Mb_512, F8_256_128, F3_200Mb_512,
    RemovableMedia = 11, FixedMedia = 12
};

enum class FmifsPacketType {
    IsSpaceFree, Percent, IncompatibleFileSystem, AccessDenied,
    MediaWriteProtected, CantQuickFormat, IoError, FinishedFormat,
    Insufficient, ClusterSizeTooBig, ClusterSizeTooSmall,
    LabelTooLong, IncompatibleMedia, DeviceNotReady,
    CheckingDevice, PlatformNotSupported
};

using FmifsCallback = BOOLEAN(WINAPI*)(FmifsPacketType command, DWORD subAction, PVOID data);

// void FormatEx(PWCHAR DriveRoot, MEDIA_TYPE MediaType, PWCHAR FileSystemTypeName,
//               PWCHAR Label, BOOLEAN QuickFormat, ULONG ClusterSize, FMIFSCALLBACK Callback);
using FormatExFn = void(WINAPI*)(PCWSTR DriveRoot, FmifsMediaType MediaType,
                                  PCWSTR FileSystemTypeName, PCWSTR Label,
                                  BOOLEAN QuickFormat, ULONG ClusterSize,
                                  FmifsCallback Callback);

class FmifsFormatter {
public:
    // Returns true if fmifs.dll and FormatEx were located successfully.
    bool Load() {
        if (m_hFmifs) return true;
        m_hFmifs = LoadLibraryW(L"fmifs.dll");
        if (!m_hFmifs) return false;
        m_formatEx = reinterpret_cast<FormatExFn>(GetProcAddress(m_hFmifs, "FormatEx"));
        return m_formatEx != nullptr;
    }

    ~FmifsFormatter() { if (m_hFmifs) FreeLibrary(m_hFmifs); }

    // driveRoot must be like L"D:\\" (a mounted volume root), fsName is
    // L"FAT32" or L"NTFS", quick=true skips the full surface scan.
    // The callback receives progress/errors during formatting.
    bool Format(const std::wstring& driveRoot, const std::wstring& fsName,
                const std::wstring& label, bool quick, FmifsCallback callback) {
        if (!m_formatEx && !Load()) return false;
        m_formatEx(driveRoot.c_str(), FmifsMediaType::FixedMedia,
                   fsName.c_str(), label.empty() ? L"" : label.c_str(),
                   quick ? TRUE : FALSE, 0, callback);
        return true;
    }

private:
    HMODULE     m_hFmifs = nullptr;
    FormatExFn  m_formatEx = nullptr;
};
