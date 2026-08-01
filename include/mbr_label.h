// mbr_label.h - classic DOS/MBR partition table engine (primary + one level
// of extended/logical chain). Equivalent of libfdisk's libfdisk/src/dos.c.
#pragma once
#include "disklabel.h"
#include "diskio.h"
#include <cstdint>

#pragma pack(push, 1)
struct MbrChsAddr { uint8_t head; uint8_t sectorCyl; uint8_t cylLow; };

struct MbrPartRecord {
    uint8_t     bootIndicator;   // 0x80 = active/bootable
    MbrChsAddr  startChs;
    uint8_t     type;
    MbrChsAddr  endChs;
    uint32_t    startLba;
    uint32_t    sizeLba;
};
#pragma pack(pop)
static_assert(sizeof(MbrPartRecord) == 16, "MbrPartRecord must be 16 bytes");

class MbrLabel : public IDiskLabel {
public:
    explicit MbrLabel(DiskIo& io, LogFn log) : m_io(io), m_log(std::move(log)) {}

    LabelType Type() const override { return LabelType::MBR; }

    FdiskStatus Probe() override;
    FdiskStatus CreateEmpty() override;
    const std::vector<PartitionEntry>& Partitions() const override { return m_view; }

    FdiskStatus AddPartition(uint64_t startLBA, uint64_t sizeSectors,
                              const std::wstring& typeCodeHex,
                              PartitionEntry* outEntry) override;
    FdiskStatus DeletePartition(int index) override;
    FdiskStatus ChangeType(int index, const std::wstring& typeCodeHex) override;
    FdiskStatus TogglePartitionFlag(int index, const std::wstring& flagName) override;
    FdiskStatus Write() override;
    std::wstring VerifyReport() const override;

    // Logical/extended chain support (secondary EBR entries).
    FdiskStatus AddLogicalPartition(uint64_t startLBA, uint64_t sizeSectors,
                                     uint8_t typeCode, PartitionEntry* outEntry);

private:
    void RebuildView();
    static std::wstring TypeName(uint8_t type);
    void WriteChs(uint64_t lba, MbrChsAddr& out) const;

    DiskIo&  m_io;
    LogFn    m_log;

    uint8_t         m_bootcode[440]{};
    uint32_t        m_diskSignature = 0;
    MbrPartRecord   m_primary[4]{};
    bool            m_valid = false;

    // Logical partitions discovered/created inside an extended partition.
    struct LogicalEntry {
        MbrPartRecord rec;      // relative to its own EBR (absolute start stored separately)
        uint64_t absoluteStart; // absolute LBA of this logical partition
        uint64_t ebrLba;        // LBA of the EBR sector that describes it
    };
    std::vector<LogicalEntry> m_logicals;
    int m_extendedIndex = -1; // which of m_primary[] is the extended container, or -1

    std::vector<PartitionEntry> m_view;
};
