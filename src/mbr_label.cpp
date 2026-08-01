// mbr_label.cpp - see mbr_label.h. Mirrors libfdisk's dos.c:
//   dos_probe_label(), dos_add_partition(), dos_delete_partition(),
//   and the EBR-chain walk for logical partitions.
#include "../include/mbr_label.h"
#include <cstring>
#include <random>
#include <sstream>
#include <iomanip>

static constexpr uint8_t TYPE_EXTENDED = 0x05;
static constexpr uint8_t TYPE_EXTENDED_LBA = 0x0F;
static constexpr uint8_t TYPE_EMPTY = 0x00;

static bool IsExtendedType(uint8_t t) { return t == TYPE_EXTENDED || t == TYPE_EXTENDED_LBA; }

std::wstring MbrLabel::TypeName(uint8_t type) {
    switch (type) {
    case 0x00: return L"Empty";
    case 0x01: return L"FAT12";
    case 0x04: return L"FAT16 <32M";
    case 0x05: return L"Extended";
    case 0x06: return L"FAT16";
    case 0x07: return L"NTFS/exFAT";
    case 0x0B: return L"FAT32 (CHS)";
    case 0x0C: return L"FAT32 (LBA)";
    case 0x0E: return L"FAT16 (LBA)";
    case 0x0F: return L"Extended (LBA)";
    case 0x82: return L"Linux swap";
    case 0x83: return L"Linux";
    case 0x8E: return L"Linux LVM";
    case 0xA5: return L"FreeBSD";
    case 0xA8: return L"macOS UFS";
    case 0xAF: return L"macOS HFS+";
    case 0xEE: return L"GPT protective";
    case 0xEF: return L"EFI System (FAT)";
    case 0xFB: return L"VMware VMFS";
    case 0xFC: return L"VMware swap";
    default: {
        wchar_t buf[16];
        swprintf(buf, 16, L"Type 0x%02X", type);
        return buf;
    }
    }
}

void MbrLabel::WriteChs(uint64_t lba, MbrChsAddr& out) const {
    // Classic CHS packing capped at the standard 1023/254/63 limits;
    // modern tools rely on the LBA fields, this is kept for on-disk fidelity.
    const uint32_t heads = m_io.Geometry().heads ? m_io.Geometry().heads : 255;
    const uint32_t spt = m_io.Geometry().sectorsPerTrack ? m_io.Geometry().sectorsPerTrack : 63;

    uint32_t cyl = static_cast<uint32_t>(lba / (heads * spt));
    uint32_t head = static_cast<uint32_t>((lba / spt) % heads);
    uint32_t sector = static_cast<uint32_t>((lba % spt) + 1);

    if (cyl > 1023) { cyl = 1023; head = heads - 1; sector = spt; } // saturate, LBA fields are authoritative

    out.head = static_cast<uint8_t>(head);
    out.sectorCyl = static_cast<uint8_t>((sector & 0x3F) | ((cyl >> 2) & 0xC0));
    out.cylLow = static_cast<uint8_t>(cyl & 0xFF);
}

FdiskStatus MbrLabel::Probe() {
    std::vector<uint8_t> sector;
    FdiskStatus st = m_io.ReadSectors(0, 1, sector);
    if (st != FdiskStatus::OK) return st;
    if (sector.size() < 512) return FdiskStatus::IO_ERROR;

    if (sector[510] != 0x55 || sector[511] != 0xAA) return FdiskStatus::BAD_SIGNATURE;

    std::memcpy(m_bootcode, sector.data(), 440);
    std::memcpy(&m_diskSignature, sector.data() + 440, 4);
    for (int i = 0; i < 4; ++i)
        std::memcpy(&m_primary[i], sector.data() + 446 + i * 16, 16);

    // Reject: this looks like a GPT protective MBR, not a real DOS table.
    for (int i = 0; i < 4; ++i) {
        if (m_primary[i].type == 0xEE) return FdiskStatus::NOT_SUPPORTED;
    }

    m_valid = true;
    m_extendedIndex = -1;
    m_logicals.clear();

    for (int i = 0; i < 4; ++i) {
        if (IsExtendedType(m_primary[i].type)) { m_extendedIndex = i; break; }
    }

    if (m_extendedIndex >= 0) {
        uint64_t extendedBase = m_primary[m_extendedIndex].startLba;
        uint64_t ebrLba = extendedBase;
        // Walk the EBR linked list; cap iterations defensively.
        for (int guard = 0; guard < 256 && ebrLba != 0; ++guard) {
            std::vector<uint8_t> ebr;
            if (m_io.ReadSectors(ebrLba, 1, ebr) != FdiskStatus::OK) break;
            if (ebr.size() < 512 || ebr[510] != 0x55 || ebr[511] != 0xAA) break;

            MbrPartRecord rec0{}, rec1{};
            std::memcpy(&rec0, ebr.data() + 446, 16);
            std::memcpy(&rec1, ebr.data() + 462, 16);

            if (rec0.type != TYPE_EMPTY) {
                LogicalEntry le;
                le.rec = rec0;
                le.ebrLba = ebrLba;
                le.absoluteStart = ebrLba + rec0.startLba;
                m_logicals.push_back(le);
            }

            if (IsExtendedType(rec1.type)) {
                ebrLba = extendedBase + rec1.startLba;
            } else {
                break;
            }
        }
    }

    RebuildView();
    return FdiskStatus::OK;
}

FdiskStatus MbrLabel::CreateEmpty() {
    std::memset(m_bootcode, 0, sizeof(m_bootcode));
    std::random_device rd;
    m_diskSignature = rd();
    std::memset(m_primary, 0, sizeof(m_primary));
    m_logicals.clear();
    m_extendedIndex = -1;
    m_valid = true;
    RebuildView();
    return FdiskStatus::OK;
}

void MbrLabel::RebuildView() {
    m_view.clear();
    int idx = 0;
    for (int i = 0; i < 4; ++i, ++idx) {
        if (m_primary[i].type == TYPE_EMPTY) continue;
        PartitionEntry pe;
        pe.index = i;
        pe.used = true;
        pe.startLBA = m_primary[i].startLba;
        pe.sizeSectors = m_primary[i].sizeLba;
        pe.endLBA = pe.sizeSectors ? (pe.startLBA + pe.sizeSectors - 1) : pe.startLBA;
        pe.typeCode = m_primary[i].type;
        pe.typeName = TypeName(m_primary[i].type);
        pe.bootable = (m_primary[i].bootIndicator == 0x80);
        m_view.push_back(pe);
    }
    for (size_t i = 0; i < m_logicals.size(); ++i) {
        const auto& le = m_logicals[i];
        PartitionEntry pe;
        pe.index = 4 + static_cast<int>(i); // logical partitions addressed from slot 4 up
        pe.used = true;
        pe.startLBA = le.absoluteStart;
        pe.sizeSectors = le.rec.sizeLba;
        pe.endLBA = pe.sizeSectors ? (pe.startLBA + pe.sizeSectors - 1) : pe.startLBA;
        pe.typeCode = le.rec.type;
        pe.typeName = TypeName(le.rec.type) + L" (logical)";
        pe.bootable = (le.rec.bootIndicator == 0x80);
        m_view.push_back(pe);
    }
}

FdiskStatus MbrLabel::AddPartition(uint64_t startLBA, uint64_t sizeSectors,
                                    const std::wstring& typeCodeHex,
                                    PartitionEntry* outEntry) {
    if (!m_valid) return FdiskStatus::NO_LABEL;
    if (sizeSectors == 0) return FdiskStatus::INVALID_ARG;
    if (startLBA + sizeSectors > m_io.Geometry().totalSectors) return FdiskStatus::OUT_OF_RANGE;
    if (startLBA + sizeSectors - 1 > 0xFFFFFFFFULL)
        return FdiskStatus::OUT_OF_RANGE; // classic MBR: 32-bit LBA fields

    for (int i = 0; i < 4; ++i) {
        if (m_primary[i].type == TYPE_EMPTY) continue;
        uint64_t s = m_primary[i].startLba, e = s + m_primary[i].sizeLba - 1;
        uint64_t newEnd = startLBA + sizeSectors - 1;
        if (startLBA <= e && newEnd >= s) return FdiskStatus::OVERLAP;
    }

    int slot = -1;
    for (int i = 0; i < 4; ++i) if (m_primary[i].type == TYPE_EMPTY) { slot = i; break; }
    if (slot < 0) return FdiskStatus::OUT_OF_RANGE; // only 4 primary slots; use AddLogicalPartition

    uint8_t typeCode = static_cast<uint8_t>(wcstoul(typeCodeHex.c_str(), nullptr, 16));
    MbrPartRecord rec{};
    rec.bootIndicator = 0x00;
    rec.type = typeCode;
    rec.startLba = static_cast<uint32_t>(startLBA);
    rec.sizeLba = static_cast<uint32_t>(sizeSectors);
    WriteChs(startLBA, rec.startChs);
    WriteChs(startLBA + sizeSectors - 1, rec.endChs);
    m_primary[slot] = rec;

    if (IsExtendedType(typeCode)) m_extendedIndex = slot;

    RebuildView();
    if (outEntry) for (auto& v : m_view) if (v.index == slot) { *outEntry = v; break; }
    if (m_log) m_log(L"[OK] MBR partition added in primary slot " + std::to_wstring(slot + 1) + L".\r\n");
    return FdiskStatus::OK;
}

FdiskStatus MbrLabel::AddLogicalPartition(uint64_t startLBA, uint64_t sizeSectors,
                                           uint8_t typeCode, PartitionEntry* outEntry) {
    if (m_extendedIndex < 0) return FdiskStatus::NOT_SUPPORTED; // need an extended partition first
    // Simplification: append the new EBR right before the requested partition,
    // chaining from the last known logical (or the extended partition start).
    uint64_t extendedBase = m_primary[m_extendedIndex].startLba;
    uint64_t ebrLba = startLBA - 1; // caller must leave 1 sector for the EBR
    if (ebrLba < extendedBase) return FdiskStatus::OUT_OF_RANGE;

    MbrPartRecord rec{};
    rec.type = typeCode;
    rec.startLba = 1; // relative to its own EBR
    rec.sizeLba = static_cast<uint32_t>(sizeSectors);
    WriteChs(startLBA, rec.startChs);
    WriteChs(startLBA + sizeSectors - 1, rec.endChs);

    LogicalEntry le;
    le.rec = rec;
    le.ebrLba = ebrLba;
    le.absoluteStart = startLBA;
    m_logicals.push_back(le);

    RebuildView();
    if (outEntry) for (auto& v : m_view) if (v.startLBA == startLBA) { *outEntry = v; break; }
    if (m_log) m_log(L"[OK] Logical partition added at LBA " + std::to_wstring(startLBA) + L".\r\n");
    return FdiskStatus::OK;
}

FdiskStatus MbrLabel::DeletePartition(int index) {
    if (index >= 0 && index < 4) {
        if (m_primary[index].type == TYPE_EMPTY) return FdiskStatus::OUT_OF_RANGE;
        bool wasExtended = IsExtendedType(m_primary[index].type);
        m_primary[index] = MbrPartRecord{};
        if (wasExtended) { m_extendedIndex = -1; m_logicals.clear(); }
        RebuildView();
        if (m_log) m_log(L"[OK] MBR partition " + std::to_wstring(index + 1) + L" deleted.\r\n");
        return FdiskStatus::OK;
    }
    int logIdx = index - 4;
    if (logIdx < 0 || static_cast<size_t>(logIdx) >= m_logicals.size()) return FdiskStatus::OUT_OF_RANGE;
    m_logicals.erase(m_logicals.begin() + logIdx);
    RebuildView();
    return FdiskStatus::OK;
}

FdiskStatus MbrLabel::ChangeType(int index, const std::wstring& typeCodeHex) {
    uint8_t typeCode = static_cast<uint8_t>(wcstoul(typeCodeHex.c_str(), nullptr, 16));
    if (index >= 0 && index < 4) {
        if (m_primary[index].type == TYPE_EMPTY) return FdiskStatus::OUT_OF_RANGE;
        m_primary[index].type = typeCode;
        RebuildView();
        return FdiskStatus::OK;
    }
    int logIdx = index - 4;
    if (logIdx < 0 || static_cast<size_t>(logIdx) >= m_logicals.size()) return FdiskStatus::OUT_OF_RANGE;
    m_logicals[logIdx].rec.type = typeCode;
    RebuildView();
    return FdiskStatus::OK;
}

FdiskStatus MbrLabel::TogglePartitionFlag(int index, const std::wstring& flagName) {
    if (flagName != L"boot") return FdiskStatus::INVALID_ARG;
    if (index >= 0 && index < 4) {
        if (m_primary[index].type == TYPE_EMPTY) return FdiskStatus::OUT_OF_RANGE;
        // Only one active partition is standard-compliant; clear others.
        for (int i = 0; i < 4; ++i) m_primary[i].bootIndicator = 0x00;
        m_primary[index].bootIndicator = 0x80;
        RebuildView();
        return FdiskStatus::OK;
    }
    return FdiskStatus::NOT_SUPPORTED; // logicals aren't bootable in classic MBR
}

FdiskStatus MbrLabel::Write() {
    if (!m_valid) return FdiskStatus::NO_LABEL;
    uint32_t bps = m_io.Geometry().bytesPerSector ? m_io.Geometry().bytesPerSector : 512;

    std::vector<uint8_t> sector(bps, 0);
    std::memcpy(sector.data(), m_bootcode, 440);
    std::memcpy(sector.data() + 440, &m_diskSignature, 4);
    for (int i = 0; i < 4; ++i)
        std::memcpy(sector.data() + 446 + i * 16, &m_primary[i], 16);
    sector[510] = 0x55; sector[511] = 0xAA;

    FdiskStatus st = m_io.WriteSectors(0, sector);
    if (st != FdiskStatus::OK) return st;

    // Write each EBR in the logical chain.
    for (size_t i = 0; i < m_logicals.size(); ++i) {
        std::vector<uint8_t> ebr(bps, 0);
        std::memcpy(ebr.data() + 446, &m_logicals[i].rec, 16);

        if (i + 1 < m_logicals.size()) {
            MbrPartRecord next{};
            next.type = TYPE_EXTENDED_LBA;
            uint64_t extendedBase = m_primary[m_extendedIndex].startLba;
            next.startLba = static_cast<uint32_t>(m_logicals[i + 1].ebrLba - extendedBase);
            next.sizeLba = 1;
            std::memcpy(ebr.data() + 462, &next, 16);
        }
        ebr[510] = 0x55; ebr[511] = 0xAA;
        st = m_io.WriteSectors(m_logicals[i].ebrLba, ebr);
        if (st != FdiskStatus::OK) return st;
    }

    m_io.ReReadPartitionTable();
    if (m_log) m_log(L"[OK] MBR partition table written.\r\n");
    return FdiskStatus::OK;
}

std::wstring MbrLabel::VerifyReport() const {
    std::wstringstream ss;
    ss << L"MBR disk signature: 0x" << std::hex << std::uppercase << m_diskSignature << L"\r\n";
    ss << L"Primary partitions in use: " << std::dec
       << [&] { int n=0; for (auto&p:m_primary) if (p.type) ++n; return n; }() << L" / 4\r\n";
    ss << L"Logical partitions: " << m_logicals.size() << L"\r\n";
    if (m_extendedIndex >= 0) ss << L"Extended partition in slot " << (m_extendedIndex + 1) << L"\r\n";
    return ss.str();
}
