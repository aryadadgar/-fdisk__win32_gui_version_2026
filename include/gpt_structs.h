// gpt_structs.h - on-disk layout per the UEFI spec, byte-for-byte compatible
// with libfdisk's include/pt-gpt.h.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#pragma pack(push, 1)

struct GptGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];

    bool operator==(const GptGuid& o) const {
        return std::memcmp(this, &o, sizeof(GptGuid)) == 0;
    }
    bool IsZero() const {
        static const GptGuid z{};
        return *this == z;
    }
};

struct GptHeader {
    char     signature[8];      // "EFI PART"
    uint32_t revision;          // 0x00010000
    uint32_t headerSize;        // 92
    uint32_t headerCrc32;
    uint32_t reserved1;
    uint64_t myLBA;
    uint64_t alternateLBA;
    uint64_t firstUsableLBA;
    uint64_t lastUsableLBA;
    GptGuid  diskGuid;
    uint64_t partitionEntryLBA;
    uint32_t numPartitionEntries;
    uint32_t sizeOfPartitionEntry;
    uint32_t partitionEntryArrayCrc32;
    // remainder of the sector is reserved/zero
};

struct GptEntry {
    GptGuid  partitionTypeGuid;
    GptGuid  uniquePartitionGuid;
    uint64_t startingLBA;
    uint64_t endingLBA;         // inclusive
    uint64_t attributes;
    char16_t name[36];          // UTF-16LE, not necessarily NUL-terminated
};

#pragma pack(pop)

static_assert(sizeof(GptHeader) == 92, "GptHeader must be 92 bytes");
static_assert(sizeof(GptEntry) == 128, "GptEntry must be 128 bytes");

// ---- GUID <-> string helpers -----------------------------------------

inline std::wstring GuidToString(const GptGuid& g) {
    wchar_t buf[40];
    swprintf(buf, 40, L"%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g.data1, g.data2, g.data3,
             g.data4[0], g.data4[1], g.data4[2], g.data4[3],
             g.data4[4], g.data4[5], g.data4[6], g.data4[7]);
    return std::wstring(buf);
}

inline bool StringToGuid(const std::wstring& s, GptGuid& out) {
    unsigned int d1;
    unsigned int d2, d3, b[8];
    int n = swscanf(s.c_str(), L"%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                     &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
    if (n != 11) return false;
    out.data1 = d1; out.data2 = (uint16_t)d2; out.data3 = (uint16_t)d3;
    for (int i = 0; i < 8; ++i) out.data4[i] = (uint8_t)b[i];
    return true;
}

// A small subset of the well-known GPT partition type GUIDs, matching
// libfdisk's gpt-type table for the entries fdisk users touch most.
struct GptTypeDef { const wchar_t* guid; const wchar_t* name; };

inline const std::vector<GptTypeDef>& KnownGptTypes() {
    static const std::vector<GptTypeDef> types = {
        { L"00000000-0000-0000-0000-000000000000", L"Unused entry" },
        { L"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", L"EFI System" },
        { L"E3C9E316-0B5C-4DB8-817D-F92DF00215AE", L"Microsoft Reserved" },
        { L"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", L"Microsoft basic data" },
        { L"5808C8AA-7E8F-42E0-85D2-E1E90434CFB3", L"Microsoft LDM metadata" },
        { L"AF9B60A0-1431-4F62-BC68-3311714A69AD", L"Microsoft LDM data" },
        { L"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC", L"Windows Recovery Environment" },
        { L"0FC63DAF-8483-4772-8E79-3D69D8477DE4", L"Linux filesystem" },
        { L"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", L"Linux swap" },
        { L"E6D6D379-F507-44C2-A23C-238F2A3DF928", L"Linux LVM" },
        { L"A19D880F-05FC-4D3B-A006-743F0F84911E", L"Linux RAID" },
        { L"9E1A2D38-C612-4316-AA26-8B49521E5A8B", L"PowerPC PReP boot" },
        { L"48465300-0000-11AA-AA11-00306543ECAC", L"Apple HFS/HFS+" },
    };
    return types;
}

inline std::wstring GptTypeName(const GptGuid& g) {
    std::wstring gs = GuidToString(g);
    for (auto& t : KnownGptTypes()) {
        GptGuid tg;
        StringToGuid(t.guid, tg);
        if (tg == g) return t.name;
    }
    return L"Unknown (" + gs + L")";
}
