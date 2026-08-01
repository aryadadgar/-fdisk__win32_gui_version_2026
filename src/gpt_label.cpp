// gpt_label.cpp - see gpt_label.h. Mirrors libfdisk's gpt.c:
//   gpt_probe_label(), gpt_write_disklabel(), gpt_add_partition(),
//   gpt_delete_partition(), gpt_recompute_crc()
#include "../include/gpt_label.h"
#include "../include/crc32.h"
#include <algorithm>
#include <random>

static constexpr uint64_t GPT_HEADER_LBA = 1;
static constexpr uint32_t GPT_DEFAULT_ENTRIES = 128;
static constexpr uint32_t GPT_ENTRY_SIZE = sizeof(GptEntry);
// Entries occupy ceil(numEntries*entrySize / sectorSize) sectors, spec
// guarantees a minimum of 16384 bytes (32 sectors at 512B) reserved.
static constexpr uint32_t GPT_ENTRY_ARRAY_MIN_BYTES = 16384;

static GptGuid GenerateGuid() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(gen), b = dist(gen);
    GptGuid g;
    std::memcpy(&g, &a, 8);
    std::memcpy(reinterpret_cast<uint8_t*>(&g) + 8, &b, 8);
    // Set version 4 / variant bits per RFC 4122 (cosmetic, GPT doesn't enforce)
    g.data3 = (g.data3 & 0x0FFF) | 0x4000;
    g.data4[0] = (g.data4[0] & 0x3F) | 0x80;
    return g;
}

FdiskStatus GptLabel::ReadHeaderAndEntries(uint64_t headerLBA, GptHeader& hdr,
                                            std::vector<GptEntry>& entries, bool isBackup) {
    std::vector<uint8_t> sector;
    FdiskStatus st = m_io.ReadSectors(headerLBA, 1, sector);
    if (st != FdiskStatus::OK) return st;
    if (sector.size() < sizeof(GptHeader)) return FdiskStatus::IO_ERROR;

    std::memcpy(&hdr, sector.data(), sizeof(GptHeader));

    if (std::memcmp(hdr.signature, "EFI PART", 8) != 0)
        return FdiskStatus::BAD_SIGNATURE;

    uint32_t storedCrc = hdr.headerCrc32;
    GptHeader tmp = hdr;
    tmp.headerCrc32 = 0;
    uint32_t calc = Crc32::Compute(reinterpret_cast<uint8_t*>(&tmp), hdr.headerSize);
    if (calc != storedCrc) {
        if (m_log) m_log((isBackup ? L"[WARN] Backup GPT header CRC mismatch.\r\n"
                                    : L"[WARN] Primary GPT header CRC mismatch.\r\n"));
        return FdiskStatus::BAD_CRC;
    }

    uint32_t bps = m_io.Geometry().bytesPerSector ? m_io.Geometry().bytesPerSector : 512;
    uint32_t arrayBytes = hdr.numPartitionEntries * hdr.sizeOfPartitionEntry;
    uint32_t arraySectors = (arrayBytes + bps - 1) / bps;

    std::vector<uint8_t> raw;
    st = m_io.ReadSectors(hdr.partitionEntryLBA, arraySectors, raw);
    if (st != FdiskStatus::OK) return st;

    uint32_t calcArrayCrc = Crc32::Compute(raw.data(), arrayBytes);
    if (calcArrayCrc != hdr.partitionEntryArrayCrc32) {
        if (m_log) m_log(L"[WARN] Partition entry array CRC mismatch.\r\n");
        return FdiskStatus::BAD_CRC;
    }

    entries.resize(hdr.numPartitionEntries);
    for (uint32_t i = 0; i < hdr.numPartitionEntries; ++i) {
        std::memcpy(&entries[i], raw.data() + (size_t)i * hdr.sizeOfPartitionEntry,
                    sizeof(GptEntry));
    }
    return FdiskStatus::OK;
}

FdiskStatus GptLabel::Probe() {
    m_headerValid = (ReadHeaderAndEntries(GPT_HEADER_LBA, m_primary, m_entries, false) == FdiskStatus::OK);

    if (!m_headerValid) {
        // Try the backup header at the last LBA of the disk.
        uint64_t lastLBA = m_io.Geometry().totalSectors ? m_io.Geometry().totalSectors - 1 : 0;
        std::vector<GptEntry> backupEntries;
        m_backupValid = (ReadHeaderAndEntries(lastLBA, m_backup, backupEntries, true) == FdiskStatus::OK);
        if (m_backupValid) {
            if (m_log) m_log(L"[RECOVER] Primary GPT header invalid; using backup header.\r\n");
            m_primary = m_backup;
            m_entries = backupEntries;
            m_headerValid = true;
        }
    }

    if (!m_headerValid) return FdiskStatus::NO_LABEL;
    RebuildView();
    return FdiskStatus::OK;
}

FdiskStatus GptLabel::CreateEmpty() {
    uint32_t bps = m_io.Geometry().bytesPerSector ? m_io.Geometry().bytesPerSector : 512;
    uint64_t totalSectors = m_io.Geometry().totalSectors;
    if (totalSectors < 64) return FdiskStatus::OUT_OF_RANGE;

    uint32_t entryArrayBytes = std::max<uint32_t>(GPT_ENTRY_ARRAY_MIN_BYTES,
                                                    GPT_DEFAULT_ENTRIES * GPT_ENTRY_SIZE);
    uint32_t entryArraySectors = entryArrayBytes / bps;

    m_primary = GptHeader{};
    std::memcpy(m_primary.signature, "EFI PART", 8);
    m_primary.revision = 0x00010000;
    m_primary.headerSize = sizeof(GptHeader);
    m_primary.myLBA = GPT_HEADER_LBA;
    m_primary.alternateLBA = totalSectors - 1;
    m_primary.partitionEntryLBA = 2;
    m_primary.firstUsableLBA = 2 + entryArraySectors;
    m_primary.lastUsableLBA = totalSectors - 1 - entryArraySectors - 1;
    m_primary.diskGuid = GenerateGuid();
    m_primary.numPartitionEntries = GPT_DEFAULT_ENTRIES;
    m_primary.sizeOfPartitionEntry = GPT_ENTRY_SIZE;

    m_entries.assign(GPT_DEFAULT_ENTRIES, GptEntry{});
    m_headerValid = true;
    RebuildView();
    return FdiskStatus::OK;
}

void GptLabel::RebuildView() {
    m_view.clear();
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const GptEntry& e = m_entries[i];
        if (e.partitionTypeGuid.IsZero()) continue;

        PartitionEntry pe;
        pe.index = static_cast<int>(i);
        pe.used = true;
        pe.startLBA = e.startingLBA;
        pe.endLBA = e.endingLBA;
        pe.sizeSectors = (e.endingLBA >= e.startingLBA) ? (e.endingLBA - e.startingLBA + 1) : 0;
        pe.guidType = GuidToString(e.partitionTypeGuid);
        pe.guidUnique = GuidToString(e.uniquePartitionGuid);
        pe.typeName = GptTypeName(e.partitionTypeGuid);
        pe.attributes = static_cast<uint32_t>(e.attributes);

        std::wstring name;
        for (int c = 0; c < 36 && e.name[c] != 0; ++c) name.push_back(static_cast<wchar_t>(e.name[c]));
        pe.name = name;

        m_view.push_back(pe);
    }
}

FdiskStatus GptLabel::AddPartition(uint64_t startLBA, uint64_t sizeSectors,
                                    const std::wstring& typeGuidStr,
                                    PartitionEntry* outEntry) {
    if (!m_headerValid) return FdiskStatus::NO_LABEL;
    if (sizeSectors == 0) return FdiskStatus::INVALID_ARG;

    uint64_t endLBA = startLBA + sizeSectors - 1;
    if (startLBA < m_primary.firstUsableLBA || endLBA > m_primary.lastUsableLBA)
        return FdiskStatus::OUT_OF_RANGE;

    // Overlap check against existing used entries
    for (const auto& e : m_entries) {
        if (e.partitionTypeGuid.IsZero()) continue;
        if (startLBA <= e.endingLBA && endLBA >= e.startingLBA)
            return FdiskStatus::OVERLAP;
    }

    // Find a free slot
    int slot = -1;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].partitionTypeGuid.IsZero()) { slot = static_cast<int>(i); break; }
    }
    if (slot < 0) return FdiskStatus::OUT_OF_RANGE; // table full

    GptGuid typeGuid;
    if (!StringToGuid(typeGuidStr, typeGuid)) {
        // allow shorthand: try matching against known type names
        bool matched = false;
        for (auto& t : KnownGptTypes()) {
            if (typeGuidStr == t.name) { StringToGuid(t.guid, typeGuid); matched = true; break; }
        }
        if (!matched) return FdiskStatus::INVALID_ARG;
    }

    GptEntry e{};
    e.partitionTypeGuid = typeGuid;
    e.uniquePartitionGuid = GenerateGuid();
    e.startingLBA = startLBA;
    e.endingLBA = endLBA;
    m_entries[slot] = e;

    RebuildView();
    if (outEntry) {
        for (auto& v : m_view) if (v.index == slot) { *outEntry = v; break; }
    }
    if (m_log) m_log(L"[OK] GPT partition added in slot " + std::to_wstring(slot + 1) + L".\r\n");
    return FdiskStatus::OK;
}

FdiskStatus GptLabel::DeletePartition(int index) {
    if (index < 0 || static_cast<size_t>(index) >= m_entries.size()) return FdiskStatus::OUT_OF_RANGE;
    if (m_entries[index].partitionTypeGuid.IsZero()) return FdiskStatus::OUT_OF_RANGE;
    m_entries[index] = GptEntry{};
    RebuildView();
    if (m_log) m_log(L"[OK] GPT partition " + std::to_wstring(index + 1) + L" deleted.\r\n");
    return FdiskStatus::OK;
}

FdiskStatus GptLabel::ChangeType(int index, const std::wstring& typeGuidStr) {
    if (index < 0 || static_cast<size_t>(index) >= m_entries.size()) return FdiskStatus::OUT_OF_RANGE;
    if (m_entries[index].partitionTypeGuid.IsZero()) return FdiskStatus::OUT_OF_RANGE;

    GptGuid typeGuid;
    if (!StringToGuid(typeGuidStr, typeGuid)) {
        bool matched = false;
        for (auto& t : KnownGptTypes()) {
            if (typeGuidStr == t.name) { StringToGuid(t.guid, typeGuid); matched = true; break; }
        }
        if (!matched) return FdiskStatus::INVALID_ARG;
    }
    m_entries[index].partitionTypeGuid = typeGuid;
    RebuildView();
    return FdiskStatus::OK;
}

FdiskStatus GptLabel::TogglePartitionFlag(int index, const std::wstring& flagName) {
    if (index < 0 || static_cast<size_t>(index) >= m_entries.size()) return FdiskStatus::OUT_OF_RANGE;
    if (m_entries[index].partitionTypeGuid.IsZero()) return FdiskStatus::OUT_OF_RANGE;

    // GPT attribute bit 60 = "read-only", bit 62 = "hidden", bit 63 = "no auto-mount" (MS defined)
    uint64_t bit = 0;
    if (flagName == L"ro") bit = 1ULL << 60;
    else if (flagName == L"hidden") bit = 1ULL << 62;
    else if (flagName == L"noautomount") bit = 1ULL << 63;
    else if (flagName == L"legacyboot") bit = 1ULL << 2;
    else return FdiskStatus::INVALID_ARG;

    m_entries[index].attributes ^= bit;
    RebuildView();
    return FdiskStatus::OK;
}

void GptLabel::RecomputeCrcs(GptHeader& hdr, const std::vector<GptEntry>& entries) {
    uint32_t arrayBytes = hdr.numPartitionEntries * hdr.sizeOfPartitionEntry;
    std::vector<uint8_t> raw(arrayBytes, 0);
    for (size_t i = 0; i < entries.size(); ++i)
        std::memcpy(raw.data() + i * hdr.sizeOfPartitionEntry, &entries[i], sizeof(GptEntry));

    hdr.partitionEntryArrayCrc32 = Crc32::Compute(raw.data(), arrayBytes);
    hdr.headerCrc32 = 0;
    hdr.headerCrc32 = Crc32::Compute(reinterpret_cast<uint8_t*>(&hdr), hdr.headerSize);
}

FdiskStatus GptLabel::WriteProtectiveMbr() {
    uint32_t bps = m_io.Geometry().bytesPerSector ? m_io.Geometry().bytesPerSector : 512;
    std::vector<uint8_t> mbr(bps, 0);

    // Boot signature
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    // Single partition entry covering the whole disk (type 0xEE)
    uint64_t totalSectors = m_io.Geometry().totalSectors;
    uint32_t sizeInLba = (totalSectors > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : static_cast<uint32_t>(totalSectors - 1);

    size_t off = 446;
    mbr[off + 0] = 0x00;              // status (non-bootable)
    mbr[off + 1] = 0x00; mbr[off + 2] = 0x02; mbr[off + 3] = 0x00; // CHS start (dummy)
    mbr[off + 4] = 0xEE;              // type = GPT protective
    mbr[off + 5] = 0xFF; mbr[off + 6] = 0xFF; mbr[off + 7] = 0xFF; // CHS end (dummy)
    uint32_t startLba = 1;
    std::memcpy(&mbr[off + 8], &startLba, 4);
    std::memcpy(&mbr[off + 12], &sizeInLba, 4);

    return m_io.WriteSectors(0, mbr);
}

FdiskStatus GptLabel::Write() {
    if (!m_headerValid) return FdiskStatus::NO_LABEL;

    uint32_t bps = m_io.Geometry().bytesPerSector ? m_io.Geometry().bytesPerSector : 512;
    uint64_t totalSectors = m_io.Geometry().totalSectors;

    // Build backup header (mirror of primary at the end of the disk)
    m_backup = m_primary;
    m_backup.myLBA = totalSectors - 1;
    m_backup.alternateLBA = GPT_HEADER_LBA;
    uint32_t arrayBytes = m_primary.numPartitionEntries * m_primary.sizeOfPartitionEntry;
    uint32_t arraySectors = arrayBytes / bps;
    m_backup.partitionEntryLBA = totalSectors - 1 - arraySectors;

    RecomputeCrcs(m_primary, m_entries);
    RecomputeCrcs(m_backup, m_entries);

    FdiskStatus st = WriteProtectiveMbr();
    if (st != FdiskStatus::OK) return st;

    // Primary header + entries
    std::vector<uint8_t> hdrSector(bps, 0);
    std::memcpy(hdrSector.data(), &m_primary, sizeof(GptHeader));
    st = m_io.WriteSectors(m_primary.myLBA, hdrSector);
    if (st != FdiskStatus::OK) return st;

    std::vector<uint8_t> entryBuf(arrayBytes, 0);
    for (size_t i = 0; i < m_entries.size(); ++i)
        std::memcpy(entryBuf.data() + i * m_primary.sizeOfPartitionEntry, &m_entries[i], sizeof(GptEntry));
    st = m_io.WriteSectors(m_primary.partitionEntryLBA, entryBuf);
    if (st != FdiskStatus::OK) return st;

    // Backup entries + header
    st = m_io.WriteSectors(m_backup.partitionEntryLBA, entryBuf);
    if (st != FdiskStatus::OK) return st;

    std::vector<uint8_t> backupHdrSector(bps, 0);
    std::memcpy(backupHdrSector.data(), &m_backup, sizeof(GptHeader));
    st = m_io.WriteSectors(m_backup.myLBA, backupHdrSector);
    if (st != FdiskStatus::OK) return st;

    m_io.ReReadPartitionTable();
    if (m_log) m_log(L"[OK] GPT written: primary+backup headers and partition arrays.\r\n");
    return FdiskStatus::OK;
}

std::wstring GptLabel::VerifyReport() const {
    std::wstring r = L"GPT disk GUID: " + GuidToString(m_primary.diskGuid) + L"\r\n";
    r += L"First usable LBA: " + std::to_wstring(m_primary.firstUsableLBA) + L"\r\n";
    r += L"Last usable LBA: " + std::to_wstring(m_primary.lastUsableLBA) + L"\r\n";
    r += L"Entries in use: " + std::to_wstring(m_view.size()) + L" / " +
         std::to_wstring(m_primary.numPartitionEntries) + L"\r\n";
    return r;
}
