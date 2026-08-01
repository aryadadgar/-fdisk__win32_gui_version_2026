// gpt_label.h - GPT partition-table engine. Equivalent of libfdisk's
// libfdisk/src/gpt.c, condensed into a self-contained C++ class.
#pragma once
#include "disklabel.h"
#include "diskio.h"
#include "gpt_structs.h"
#include <memory>

class GptLabel : public IDiskLabel {
public:
    explicit GptLabel(DiskIo& io, LogFn log) : m_io(io), m_log(std::move(log)) {}

    LabelType Type() const override { return LabelType::GPT; }

    FdiskStatus Probe() override;
    FdiskStatus CreateEmpty() override;
    const std::vector<PartitionEntry>& Partitions() const override { return m_view; }

    FdiskStatus AddPartition(uint64_t startLBA, uint64_t sizeSectors,
                              const std::wstring& typeGuid,
                              PartitionEntry* outEntry) override;
    FdiskStatus DeletePartition(int index) override;
    FdiskStatus ChangeType(int index, const std::wstring& typeGuid) override;
    FdiskStatus TogglePartitionFlag(int index, const std::wstring& flagName) override;
    FdiskStatus Write() override;
    std::wstring VerifyReport() const override;

    // Also writes a "protective MBR" at LBA0 as the UEFI spec requires.
    FdiskStatus WriteProtectiveMbr();

private:
    void RebuildView();
    FdiskStatus ReadHeaderAndEntries(uint64_t headerLBA, GptHeader& hdr,
                                      std::vector<GptEntry>& entries, bool isBackup);
    void RecomputeCrcs(GptHeader& hdr, const std::vector<GptEntry>& entries);

    DiskIo&  m_io;
    LogFn    m_log;

    GptHeader              m_primary{};
    GptHeader              m_backup{};
    std::vector<GptEntry>  m_entries;
    std::vector<PartitionEntry> m_view; // label-agnostic view for the GUI
    bool                   m_headerValid = false;
    bool                   m_backupValid = false;
};
