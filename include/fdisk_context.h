// fdisk_context.h - top-level object mirroring libfdisk's struct
// fdisk_context / fdisk_new_context() / fdisk_assign_device(). Owns the
// DiskIo handle and the active IDiskLabel (GPT or MBR), auto-detected.
#pragma once
#include "disklabel.h"
#include "diskio.h"
#include "gpt_label.h"
#include "mbr_label.h"
#include <memory>

class FdiskContext {
public:
    explicit FdiskContext(LogFn log) : m_log(std::move(log)) {}

    // Equivalent of fdisk_assign_device(cxt, devname, readonly)
    FdiskStatus AssignDevice(const std::wstring& devicePath, bool writeAccess) {
        m_label.reset();
        FdiskStatus st = m_io.Open(devicePath, writeAccess);
        if (st != FdiskStatus::OK) return st;

        // Try GPT first (it always carries a protective MBR at LBA0 too),
        // matching libfdisk's label-probe order.
        auto gpt = std::make_unique<GptLabel>(m_io, m_log);
        if (gpt->Probe() == FdiskStatus::OK) {
            m_label = std::move(gpt);
            m_labelType = LabelType::GPT;
            return FdiskStatus::OK;
        }

        auto mbr = std::make_unique<MbrLabel>(m_io, m_log);
        FdiskStatus mbrSt = mbr->Probe();
        if (mbrSt == FdiskStatus::OK) {
            m_label = std::move(mbr);
            m_labelType = LabelType::MBR;
            return FdiskStatus::OK;
        }

        m_labelType = LabelType::NONE;
        return FdiskStatus::NO_LABEL; // device opened fine; just no recognized label
    }

    void Deassign() { m_label.reset(); m_io.Close(); m_labelType = LabelType::NONE; }

    bool IsAssigned() const { return m_io.IsOpen(); }
    bool HasLabel() const { return m_label != nullptr; }
    LabelType CurrentLabelType() const { return m_labelType; }

    // Equivalent of fdisk_create_disklabel(cxt, "gpt"|"dos")
    FdiskStatus CreateDisklabel(LabelType type) {
        if (type == LabelType::GPT) {
            auto gpt = std::make_unique<GptLabel>(m_io, m_log);
            FdiskStatus st = gpt->CreateEmpty();
            if (st == FdiskStatus::OK) { m_label = std::move(gpt); m_labelType = LabelType::GPT; }
            return st;
        }
        if (type == LabelType::MBR) {
            auto mbr = std::make_unique<MbrLabel>(m_io, m_log);
            FdiskStatus st = mbr->CreateEmpty();
            if (st == FdiskStatus::OK) { m_label = std::move(mbr); m_labelType = LabelType::MBR; }
            return st;
        }
        return FdiskStatus::NOT_SUPPORTED; // BSD/SGI/SUN: future phase
    }

    IDiskLabel* Label() { return m_label.get(); }
    const DiskGeometry& Geometry() const { return m_io.Geometry(); }
    const std::wstring& DevicePath() const { return m_io.Path(); }

    FdiskStatus ReadSector0(std::vector<uint8_t>& out) { return m_io.ReadSectors(0, 1, out); }

    FdiskStatus WriteLabel() {
        if (!m_label) return FdiskStatus::NO_LABEL;
        return m_label->Write();
    }

    // Windows-native commit via IOCTL_DISK_SET_DRIVE_LAYOUT_EX, called after
    // WriteLabel() has already written the on-disk structures.
    //
    // Only used for MBR here: the label-agnostic PartitionEntry view doesn't
    // carry the GPT disk GUID (only per-partition GUIDs), so re-submitting a
    // GPT layout through this IOCTL risks the OS seeing a mismatched disk
    // identity. For GPT, WriteLabel() already wrote correct primary+backup
    // headers directly and called IOCTL_DISK_UPDATE_PROPERTIES, which is the
    // right way for Windows to pick up the change.
    FdiskStatus CommitLayoutViaIoctl() {
        if (!m_label) return FdiskStatus::NO_LABEL;
        if (m_labelType != LabelType::MBR) return FdiskStatus::NOT_SUPPORTED;
        GUID unused{};
        return m_io.SetDriveLayoutEx(false, unused, m_label->Partitions());
    }

private:
    LogFn        m_log;
    DiskIo       m_io;
    std::unique_ptr<IDiskLabel> m_label;
    LabelType    m_labelType = LabelType::NONE;
};
