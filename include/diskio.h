// diskio.h - raw sector-level disk I/O on Win32 physical drives / volumes.
// Equivalent of libfdisk's low-level fd read/write/lseek wrapper (blkdev.c
// counterpart), but built on Win32 HANDLEs instead of POSIX fds.
#pragma once

#include <windows.h>
#include <winioctl.h>
#include <rpc.h>
#include <string>
#include <cwchar>
#include <vector>
#include <cstdint>
#include "disklabel.h"

class DiskIo {
public:
    DiskIo() = default;
    ~DiskIo() { Close(); }

    DiskIo(const DiskIo&) = delete;
    DiskIo& operator=(const DiskIo&) = delete;

    // Opens \\.\PhysicalDriveN (or a volume path). writeAccess=true requests
    // GENERIC_READ|GENERIC_WRITE; caller must be elevated for physical drives.
    FdiskStatus Open(const std::wstring& path, bool writeAccess) {
        Close();
        DWORD access = GENERIC_READ | (writeAccess ? GENERIC_WRITE : 0);
        m_handle = CreateFileW(
            path.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);

        if (m_handle == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            m_handle = nullptr;
            return (err == ERROR_ACCESS_DENIED) ? FdiskStatus::ACCESS_DENIED
                                                 : FdiskStatus::IO_ERROR;
        }
        m_path = path;
        m_writeAccess = writeAccess;
        QueryGeometry();
        return FdiskStatus::OK;
    }

    void Close() {
        if (m_handle) {
            CloseHandle(m_handle);
            m_handle = nullptr;
        }
    }

    bool IsOpen() const { return m_handle != nullptr; }
    const DiskGeometry& Geometry() const { return m_geom; }
    const std::wstring& Path() const { return m_path; }

    // Sector-aligned read. lba is in units of Geometry().bytesPerSector.
    FdiskStatus ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) {
        if (!m_handle) return FdiskStatus::IO_ERROR;
        uint32_t bps = m_geom.bytesPerSector ? m_geom.bytesPerSector : 512;
        out.assign(static_cast<size_t>(count) * bps, 0);

        LARGE_INTEGER off;
        off.QuadPart = static_cast<LONGLONG>(lba) * bps;
        if (!SetFilePointerEx(m_handle, off, nullptr, FILE_BEGIN))
            return FdiskStatus::IO_ERROR;

        DWORD bytesRead = 0;
        if (!ReadFile(m_handle, out.data(), (DWORD)out.size(), &bytesRead, nullptr))
            return FdiskStatus::IO_ERROR;
        if (bytesRead != out.size())
            return FdiskStatus::IO_ERROR;
        return FdiskStatus::OK;
    }

    // Sector-aligned write. buf.size() must be a multiple of bytesPerSector.
    FdiskStatus WriteSectors(uint64_t lba, const std::vector<uint8_t>& buf) {
        if (!m_handle || !m_writeAccess) return FdiskStatus::ACCESS_DENIED;
        uint32_t bps = m_geom.bytesPerSector ? m_geom.bytesPerSector : 512;
        if (buf.size() % bps != 0) return FdiskStatus::INVALID_ARG;

        LARGE_INTEGER off;
        off.QuadPart = static_cast<LONGLONG>(lba) * bps;
        if (!SetFilePointerEx(m_handle, off, nullptr, FILE_BEGIN))
            return FdiskStatus::IO_ERROR;

        DWORD bytesWritten = 0;
        if (!WriteFile(m_handle, buf.data(), (DWORD)buf.size(), &bytesWritten, nullptr))
            return FdiskStatus::IO_ERROR;
        if (bytesWritten != buf.size())
            return FdiskStatus::IO_ERROR;
        return FdiskStatus::OK;
    }

    // Commit a partition layout the Windows-native way: IOCTL_DISK_SET_DRIVE_LAYOUT_EX.
    // This is the documented, correct API for telling the OS "here is the new
    // table" — it updates Windows' internal view atomically, unlike poking raw
    // sectors and hoping IOCTL_DISK_UPDATE_PROPERTIES catches up.
    // gptDiskGuid is only used when isGpt == true.
    FdiskStatus SetDriveLayoutEx(bool isGpt, const GUID& gptDiskGuid,
                                  const std::vector<PartitionEntry>& parts) {
        if (!m_handle || !m_writeAccess) return FdiskStatus::ACCESS_DENIED;

        size_t n = parts.size();
        size_t totalSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                            (n > 0 ? (n - 1) * sizeof(PARTITION_INFORMATION_EX) : 0);
        std::vector<uint8_t> buf(totalSize, 0);
        auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());

        layout->PartitionStyle = isGpt ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
        layout->PartitionCount = static_cast<DWORD>(n);

        if (isGpt) {
            layout->Gpt.DiskId = gptDiskGuid;
            layout->Gpt.StartingUsableOffset.QuadPart =
                static_cast<LONGLONG>(m_geom.bytesPerSector) * 34; // conservative default
            layout->Gpt.UsableLength.QuadPart =
                static_cast<LONGLONG>(m_geom.totalSectors - 68) * m_geom.bytesPerSector;
            layout->Gpt.MaxPartitionCount = 128;
        } else {
            layout->Mbr.Signature = static_cast<DWORD>(GetTickCount64()); // caller may overwrite
        }

        for (size_t i = 0; i < n; ++i) {
            auto& dst = layout->PartitionEntry[i];
            const auto& src = parts[i];
            dst.PartitionStyle = isGpt ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
            dst.StartingOffset.QuadPart = static_cast<LONGLONG>(src.startLBA) * m_geom.bytesPerSector;
            dst.PartitionLength.QuadPart = static_cast<LONGLONG>(src.sizeSectors) * m_geom.bytesPerSector;
            dst.PartitionNumber = static_cast<DWORD>(src.index + 1);
            dst.RewritePartition = TRUE;

            if (isGpt) {
                GUID typeGuid{};
                UuidFromStringW(reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(src.guidType.c_str())), &typeGuid);
                GUID idGuid{};
                UuidFromStringW(reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(src.guidUnique.c_str())), &idGuid);
                dst.Gpt.PartitionType = typeGuid;
                dst.Gpt.PartitionId = idGuid;
                dst.Gpt.Attributes = src.attributes;
                wcsncpy_s(dst.Gpt.Name, src.name.c_str(), _TRUNCATE);
            } else {
                dst.Mbr.PartitionType = static_cast<BYTE>(src.typeCode);
                dst.Mbr.BootIndicator = src.bootable ? TRUE : FALSE;
                dst.Mbr.RecognizedPartition = TRUE;
            }
        }

        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(m_handle, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                                   layout, static_cast<DWORD>(buf.size()),
                                   nullptr, 0, &bytesReturned, nullptr);
        return ok ? FdiskStatus::OK : FdiskStatus::IO_ERROR;
    }

    // Flush OS caches / re-read partition info so Windows notices new layout.
    FdiskStatus ReReadPartitionTable() {
        if (!m_handle) return FdiskStatus::IO_ERROR;
        DWORD bytesReturned = 0;
        FlushFileBuffers(m_handle);
        BOOL ok = DeviceIoControl(m_handle, IOCTL_DISK_UPDATE_PROPERTIES,
                                   nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
        return ok ? FdiskStatus::OK : FdiskStatus::IO_ERROR;
    }

private:
    void QueryGeometry() {
        DISK_GEOMETRY_EX dgx = {};
        DWORD bytesReturned = 0;
        if (DeviceIoControl(m_handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                             nullptr, 0, &dgx, sizeof(dgx), &bytesReturned, nullptr)) {
            m_geom.bytesPerSector   = dgx.Geometry.BytesPerSector;
            m_geom.heads            = dgx.Geometry.TracksPerCylinder;
            m_geom.sectorsPerTrack  = dgx.Geometry.SectorsPerTrack;
            m_geom.cylinders        = static_cast<uint32_t>(dgx.Geometry.Cylinders.QuadPart);
            m_geom.totalSectors     = static_cast<uint64_t>(dgx.DiskSize.QuadPart) / m_geom.bytesPerSector;
        } else {
            // Fallback to basic IOCTL
            DISK_GEOMETRY dg = {};
            if (DeviceIoControl(m_handle, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                 nullptr, 0, &dg, sizeof(dg), &bytesReturned, nullptr)) {
                m_geom.bytesPerSector  = dg.BytesPerSector;
                m_geom.heads           = dg.TracksPerCylinder;
                m_geom.sectorsPerTrack = dg.SectorsPerTrack;
                m_geom.cylinders       = static_cast<uint32_t>(dg.Cylinders.QuadPart);
                m_geom.totalSectors    = dg.Cylinders.QuadPart * dg.TracksPerCylinder * dg.SectorsPerTrack;
            } else {
                m_geom = DiskGeometry{}; // defaults: 512 bytes/sector
            }
        }
    }

    HANDLE       m_handle = nullptr;
    std::wstring m_path;
    bool         m_writeAccess = false;
    DiskGeometry m_geom;
};
