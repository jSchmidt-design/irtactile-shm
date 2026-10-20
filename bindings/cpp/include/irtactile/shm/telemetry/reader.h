// irtactile/shm/telemetry/reader.h - the consumer side of telemetry-shm v1
// (irTactile uses it; a tool that inspects a producer can too), plus the
// return region: ReturnWriter (irTactile) and ReturnReader (producer).
//
// SPDX-License-Identifier: MIT
//
// Portable: works on a mapped buffer of known size. The buffer is untrusted.

#ifndef IRTACTILE_SHM_TELEMETRY_READER_H
#define IRTACTILE_SHM_TELEMETRY_READER_H

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <irtactile/shm/common/atomic.h>

#include "layout.h"

namespace irtactile {
namespace shm {
namespace telemetry {

/// What readFrame() did.
enum class ReadResult : uint32_t {
    NewFrame,       ///< the out-parameters hold a new, consistent frame
    NoNewFrame,     ///< frameSeq did not advance
    Torn,           ///< no consistent copy within the retry budget; keep the previous frame
    LayoutChanged,  ///< layoutSeq moved: call refreshLayout() (the frame was not copied)
    LayoutBusy,     ///< layoutSeq is odd: a rewrite is in progress
    Detached,       ///< re-validation of the layout failed; the reader is detached
};

/// One consistent frame, in the producer's slot numbering. Vectors are reused between calls.
struct Frame {
    std::vector<uint64_t> valid;
    std::vector<uint32_t> eventCount;
    std::vector<float>    values;
    uint64_t              frameQpc{0};
    uint64_t              frameSeq{0};
};

class Reader {
public:
    /// Validates and snapshots header + descriptors. `base` must be 8-byte aligned.
    AttachResult attach(const void* base, size_t mappedBytes)
    {
        m_base = static_cast<const uint8_t*>(base);
        m_mapped = mappedBytes;
        m_attached = false;
        if (mappedBytes < kHeaderBytes) return AttachResult::Malformed;
        return snapshot();
    }

    void detach() noexcept { m_attached = false; m_base = nullptr; }
    [[nodiscard]] bool attached() const noexcept { return m_attached; }

    /// Re-checks layoutSeq. Returns Ok when the snapshot is current (re-taken if it moved).
    /// NotReady while a rewrite is in progress (odd layoutSeq).
    AttachResult refreshLayout()
    {
        if (!m_attached) return AttachResult::Malformed;
        const uint32_t ls = layoutSeqLive();
        if (ls & 1u) return AttachResult::NotReady;
        if (ls == m_snapshotSeq) return AttachResult::Ok;
        return snapshot();
    }

    // --- layout snapshot (valid until the next refreshLayout()) -------------
    [[nodiscard]] const Header& header() const noexcept { return m_header; }
    [[nodiscard]] const std::vector<ChannelDesc>& descriptors() const noexcept { return m_descs; }
    [[nodiscard]] uint32_t snapshotLayoutSeq() const noexcept { return m_snapshotSeq; }

    // --- concurrent fields: live loads only ---------------------------------
    [[nodiscard]] uint32_t layoutSeqLive() const noexcept { return shm::detail::acquireLoad(liveHeader().layoutSeq); }
    [[nodiscard]] uint64_t lastPublishQpc() const noexcept { return shm::detail::acquireLoad(liveHeader().lastPublishQpc); }
    [[nodiscard]] uint64_t frameSeqLive() const noexcept { return shm::detail::acquireLoad(liveHeader().frameSeq); }
    /// Unknown values read as Closed.
    [[nodiscard]] ProducerState producerState() const noexcept
    {
        const uint32_t s = shm::detail::acquireLoad(liveHeader().producerState);
        return s <= static_cast<uint32_t>(ProducerState::Closed) ? static_cast<ProducerState>(s) : ProducerState::Closed;
    }

    /// Copies the newest frame of the slots [0, channelCount) of the snapshot.
    ReadResult readFrame(Frame& out, uint64_t lastSeenFrameSeq)
    {
        if (!m_attached) return ReadResult::Detached;
        const uint32_t ls = layoutSeqLive();
        if (ls & 1u) return ReadResult::LayoutBusy;
        if (ls != m_snapshotSeq) return ReadResult::LayoutChanged;

        const uint32_t words = m_header.capacity / 64;
        out.valid.resize(words);
        out.eventCount.resize(m_header.capacity);
        out.values.resize(m_header.capacity);

        for (uint32_t attempt = 0; attempt < kMaxFrameRetries; ++attempt) {
            const uint64_t f1 = shm::detail::acquireLoad(liveHeader().frameSeq);
            if (f1 & 1u) continue;
            if (f1 == lastSeenFrameSeq) return ReadResult::NoNewFrame;

            std::memcpy(out.valid.data(), m_base + m_header.validOffset, static_cast<size_t>(words) * 8);
            std::memcpy(out.eventCount.data(), m_base + m_header.eventCountOffset, static_cast<size_t>(m_header.capacity) * 4);
            std::memcpy(out.values.data(), m_base + m_header.valuesOffset, static_cast<size_t>(m_header.capacity) * 4);
            const uint64_t qpc = liveHeader().frameQpc;

            const uint64_t f2 = shm::detail::acquireLoad(liveHeader().frameSeqEnd);
            if (f2 != f1 || shm::detail::acquireLoad(liveHeader().frameSeq) != f1) continue;
            if (layoutSeqLive() != m_snapshotSeq) return ReadResult::LayoutChanged;
            out.frameQpc = qpc;
            out.frameSeq = f1;
            return ReadResult::NewFrame;
        }
        return ReadResult::Torn;
    }

private:
    const Header& liveHeader() const noexcept { return *reinterpret_cast<const Header*>(m_base); }

    AttachResult snapshot()
    {
        for (uint32_t attempt = 0; attempt < kMaxFrameRetries; ++attempt) {
            const uint32_t s1 = layoutSeqLive();
            if (s1 & 1u) return AttachResult::NotReady;
            Header h;
            std::memcpy(&h, m_base, kHeaderBytes);
            const AttachResult v = validateHeader(h, m_mapped);
            if (v != AttachResult::Ok && layoutSeqLive() != s1) continue;   // torn copy, not a verdict
            if (v != AttachResult::Ok) { m_attached = false; return v; }

            std::vector<ChannelDesc> descs(h.capacity);
            std::memcpy(descs.data(), m_base + h.descOffset, static_cast<size_t>(h.capacity) * kDescBytes);
            if (layoutSeqLive() != s1) continue;

            m_header = h;
            m_descs = std::move(descs);
            m_snapshotSeq = s1;
            m_attached = true;
            return AttachResult::Ok;
        }
        m_attached = false;
        return AttachResult::NotReady;
    }

    const uint8_t*           m_base{nullptr};
    size_t                   m_mapped{0};
    bool                     m_attached{false};
    Header                   m_header{};
    std::vector<ChannelDesc> m_descs;
    uint32_t                 m_snapshotSeq{0};
};

// ---------------------------------------------------------------------------
// Return region
// ---------------------------------------------------------------------------

/// Masks and consumer state as a producer sees them.
struct ReturnView {
    uint32_t              capacity{0};
    uint32_t              forLayoutSeq{0};
    ConsumerState         consumerState{ConsumerState::Idle};
    uint64_t              lastConsumeQpc{0};
    uint32_t              consumerPid{0};
    std::vector<uint64_t> accepted;
    std::vector<uint64_t> demanded;

    [[nodiscard]] bool isAccepted(uint32_t slot) const noexcept
    {
        return (slot >> 6) < accepted.size() && ((accepted[slot >> 6] >> (slot & 63)) & 1u) != 0;
    }
    /// While the consumer is not Active every accepted channel counts as demanded.
    [[nodiscard]] bool isDemanded(uint32_t slot) const noexcept
    {
        if (consumerState != ConsumerState::Active) return isAccepted(slot);
        return (slot >> 6) < demanded.size() && ((demanded[slot >> 6] >> (slot & 63)) & 1u) != 0;
    }
};

/// The irTactile side: creates and publishes the return region. `base` must be 8-byte aligned and at least
/// kReturnBytes long.
class ReturnWriter {
public:
    bool init(void* base, size_t bytes, uint32_t consumerPid)
    {
        if (bytes < kReturnBytes) return false;
        m_base = static_cast<uint8_t*>(base);
        ReturnHeader* h = header();
        if (h->magic != kReturnMagic) {
            std::memset(m_base, 0, kReturnBytes);
            h->versionMajor = kVersionMajor;
            h->versionMinor = kVersionMinor;
            h->headerBytes = kReturnHeaderBytes;
            h->totalBytes = kReturnBytes;
            h->acceptedOffset = returnAcceptedOffset();
            h->demandedOffset = returnDemandedOffset(kMaxCapacity);
        }
        shm::detail::releaseStore(h->consumerPid, consumerPid);
        shm::detail::releaseStore(h->magic, kReturnMagic);
        return true;
    }

    /// Publishes both masks for a producer layout (`words` = capacity / 64 each).
    void publishMasks(uint32_t capacity, uint32_t forLayoutSeq, const uint64_t* accepted, const uint64_t* demanded)
    {
        if (m_base == nullptr || !isValidCapacity(capacity)) return;
        ReturnHeader* h = header();
        const uint64_t s = shm::detail::acquireLoad(h->retSeq);
        const uint64_t begin = s + 1 + (s & 1u);
        shm::detail::releaseStore(h->retSeq, begin);
        h->capacity = capacity;
        h->acceptedOffset = returnAcceptedOffset();
        h->demandedOffset = returnDemandedOffset(capacity);
        h->forLayoutSeq = forLayoutSeq;
        const size_t bytes = (capacity / 64) * 8;
        std::memcpy(m_base + h->acceptedOffset, accepted, bytes);
        std::memcpy(m_base + h->demandedOffset, demanded, bytes);
        shm::detail::releaseStore(h->retSeqEnd, begin + 1);
        shm::detail::releaseStore(h->retSeq, begin + 1);
    }

    void setState(ConsumerState state) noexcept
    {
        if (m_base != nullptr) shm::detail::releaseStore(header()->consumerState, static_cast<uint32_t>(state));
    }
    void touch(uint64_t qpc) noexcept
    {
        if (m_base != nullptr) shm::detail::releaseStore(header()->lastConsumeQpc, qpc);
    }

private:
    ReturnHeader* header() noexcept { return reinterpret_cast<ReturnHeader*>(m_base); }
    uint8_t* m_base{nullptr};
};

/// The producer side: reads the return region (optional).
class ReturnReader {
public:
    AttachResult attach(const void* base, size_t mappedBytes)
    {
        m_base = static_cast<const uint8_t*>(base);
        m_mapped = mappedBytes;
        m_attached = false;
        if (mappedBytes < kReturnHeaderBytes) return AttachResult::Malformed;
        ReturnHeader h;
        std::memcpy(&h, m_base, kReturnHeaderBytes);
        const AttachResult r = validateReturnHeader(h, mappedBytes);
        m_attached = (r == AttachResult::Ok);
        return r;
    }
    [[nodiscard]] bool attached() const noexcept { return m_attached; }

    /// Latest consistent state; false when none could be read this time (keep the previous view).
    bool read(ReturnView& out) const
    {
        if (!m_attached) return false;
        const ReturnHeader& live = *reinterpret_cast<const ReturnHeader*>(m_base);
        for (uint32_t attempt = 0; attempt < kMaxFrameRetries; ++attempt) {
            const uint64_t r1 = shm::detail::acquireLoad(live.retSeq);
            if (r1 & 1u) continue;
            ReturnHeader h;
            std::memcpy(&h, m_base, kReturnHeaderBytes);
            if (validateReturnFrame(h, m_mapped) != AttachResult::Ok) return false;
            const uint32_t words = h.capacity / 64;
            out.accepted.assign(words, 0);
            out.demanded.assign(words, 0);
            if (words != 0) {
                std::memcpy(out.accepted.data(), m_base + h.acceptedOffset, static_cast<size_t>(words) * 8);
                std::memcpy(out.demanded.data(), m_base + h.demandedOffset, static_cast<size_t>(words) * 8);
            }
            if (shm::detail::acquireLoad(live.retSeqEnd) != r1 || shm::detail::acquireLoad(live.retSeq) != r1) continue;
            // Concurrent fields: live loads, never the copy. An unknown state is rejected (as at attach).
            const uint32_t cs = shm::detail::acquireLoad(live.consumerState);
            if (cs > static_cast<uint32_t>(ConsumerState::Closed)) return false;
            out.capacity = h.capacity;
            out.forLayoutSeq = h.forLayoutSeq;
            out.consumerState = static_cast<ConsumerState>(cs);
            out.lastConsumeQpc = shm::detail::acquireLoad(live.lastConsumeQpc);
            out.consumerPid = shm::detail::acquireLoad(live.consumerPid);
            return true;
        }
        return false;
    }

private:
    const uint8_t* m_base{nullptr};
    size_t         m_mapped{0};
    bool           m_attached{false};
};

}  // namespace telemetry
}  // namespace shm
}  // namespace irtactile

#endif  // IRTACTILE_SHM_TELEMETRY_READER_H
