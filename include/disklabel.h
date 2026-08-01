// disklabel.h - Common types for the fdisk C++ port (Win32)
// Mirrors the label-agnostic parts of libfdisk's struct fdisk_partition /
// struct fdisk_label so the GUI can talk to GPT or MBR through one interface.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <windows.h>

// Logging callback type - GUI supplies AppendLog-compatible function
using LogFn = std::function<void(const std::wstring&)>;

enum class LabelType {
    NONE,
    MBR,   // DOS/MBR partition table
    GPT,   // GUID Partition Table
    // Legacy label types recognized but not yet editable in this port:
    BSD,
    SGI,
    SUN
};

// One partition entry, label-agnostic view (like fdisk_partition)
struct PartitionEntry {
    int         index = -1;          // 0-based slot index
    bool        used = false;        // slot occupied
    uint64_t    startLBA = 0;
    uint64_t    endLBA = 0;          // inclusive
    uint64_t    sizeSectors = 0;
    uint32_t    typeCode = 0;        // MBR: 1-byte type; GPT: index into known-type table
    std::wstring typeName;           // human readable
    std::wstring guidType;           // GPT only
    std::wstring guidUnique;         // GPT only
    std::wstring name;               // GPT partition name (36 UTF-16 chars max)
    bool        bootable = false;    // MBR active flag
    uint32_t    attributes = 0;      // GPT attribute bits
};

// Disk geometry / addressing context shared across label types
struct DiskGeometry {
    uint32_t bytesPerSector = 512;
    uint64_t totalSectors = 0;
    uint32_t heads = 255;
    uint32_t sectorsPerTrack = 63;
    uint32_t cylinders = 0;
};

// Result codes mirroring libfdisk's negative-errno convention, but as an enum
enum class FdiskStatus {
    OK = 0,
    IO_ERROR,
    NO_LABEL,
    BAD_SIGNATURE,
    BAD_CRC,
    OUT_OF_RANGE,
    OVERLAP,
    NOT_SUPPORTED,
    ACCESS_DENIED,
    INVALID_ARG
};

inline std::wstring StatusToString(FdiskStatus s) {
    switch (s) {
    case FdiskStatus::OK: return L"OK";
    case FdiskStatus::IO_ERROR: return L"I/O error";
    case FdiskStatus::NO_LABEL: return L"No partition table present";
    case FdiskStatus::BAD_SIGNATURE: return L"Bad signature";
    case FdiskStatus::BAD_CRC: return L"CRC32 checksum mismatch";
    case FdiskStatus::OUT_OF_RANGE: return L"Value out of range";
    case FdiskStatus::OVERLAP: return L"Partitions overlap";
    case FdiskStatus::NOT_SUPPORTED: return L"Not supported";
    case FdiskStatus::ACCESS_DENIED: return L"Access denied (run as Administrator)";
    case FdiskStatus::INVALID_ARG: return L"Invalid argument";
    }
    return L"Unknown error";
}

// Abstract interface implemented by GptLabel and MbrLabel.
// Mirrors the function-pointer table inside libfdisk's struct fdisk_label.
class IDiskLabel {
public:
    virtual ~IDiskLabel() = default;

    virtual LabelType Type() const = 0;

    // Parse an already-loaded raw image (first N sectors + full table region)
    // read via IDiskIo. Returns NO_LABEL if signature doesn't match.
    virtual FdiskStatus Probe() = 0;

    // Create a brand new, empty label of this type on the in-memory image.
    virtual FdiskStatus CreateEmpty() = 0;

    virtual const std::vector<PartitionEntry>& Partitions() const = 0;

    virtual FdiskStatus AddPartition(uint64_t startLBA, uint64_t sizeSectors,
                                      const std::wstring& typeGuidOrCode,
                                      PartitionEntry* outEntry) = 0;

    virtual FdiskStatus DeletePartition(int index) = 0;

    virtual FdiskStatus ChangeType(int index, const std::wstring& typeGuidOrCode) = 0;

    virtual FdiskStatus TogglePartitionFlag(int index, const std::wstring& flagName) = 0;

    // Recompute checksums/headers and write the label back to disk via IDiskIo.
    virtual FdiskStatus Write() = 0;

    virtual std::wstring VerifyReport() const = 0;
};
