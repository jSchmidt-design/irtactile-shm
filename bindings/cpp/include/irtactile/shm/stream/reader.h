// irtactile/shm/stream/reader.h - the consumer API: BlockView, StreamRequest,
// AttachResult, StreamState, and Reader, which implements all of it over a
// caller-supplied buffer.
//
// SPDX-License-Identifier: MIT
//
// A foreign-language binding reimplements the protocol sections of README.md
// in its own language.
//
// Reader is bytes-in-a-buffer to blocks. The OS layer (mapping, event, QPC)
// is Client in win32.h.
//
// Minimal consumer:
//
//     BlockView b;
//     while (reader.next(b))
//         for (uint32_t ch = 0; ch < reader.channelCount(); ++ch) {
//             const float* s = b.channel(ch);
//             for (uint32_t i = 0; i < b.sampleCount; ++i) use(s[i]);
//         }

#ifndef IRTACTILE_SHM_STREAM_READER_H
#define IRTACTILE_SHM_STREAM_READER_H

#include <irtactile/shm/common/atomic.h>
#include "layout.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace irtactile {
namespace shm {
namespace stream {

/// A block, copied out of the ring: the protocol requires a copy bracketed
/// by descriptor re-checks.
struct BlockView {
    uint64_t seq              = 0;
    uint64_t firstSampleIndex = 0;
    uint64_t timestampQpc     = 0;
    uint32_t sampleCount      = 0;
    uint32_t flags            = 0;   ///< BlockFlags
    uint32_t silentChannelMask = 0;

    /// sampleCount floats, contiguous. Converts from int16 when the segment
    /// carries int16, so a consumer never branches on sampleFormat.
    const float* channel(uint32_t ch) const noexcept
    {
        return ch < m_channelCount ? m_samples.data() + static_cast<size_t>(ch) * m_stride
                                   : nullptr;
    }

    uint32_t channelCount() const noexcept { return m_channelCount; }

private:
    friend class Reader;
    std::vector<float> m_samples;     ///< channelCount × stride, planar
    uint32_t           m_channelCount = 0;
    uint32_t           m_stride       = 0;   ///< == sampleCount
};

/// Structural assertion for attach(); never a request. Any field left at zero
/// is not checked. On mismatch, mismatchDetail() reports the disagreements.
///
/// Assert what would break you, not everything you know. A consumer draining
/// with `while (next(b))` handles any block size it is given, and pinning
/// samplesPerBlock turns the operator's next latency tweak into a hard
/// Mismatch at open - so leave it 0 unless the processing downstream is
/// genuinely fixed-size (an FFT, a fixed hop). To confirm the cadence rather
/// than assert it, attach without pinning and read publishHz().
struct StreamRequest {
    uint32_t     channelCount      = 0;  ///< 0 ⇒ do not check
    uint32_t     sampleRateMilliHz = 0;  ///< 0 ⇒ do not check
    uint32_t     samplesPerBlock   = 0;  ///< 0 ⇒ do not check
    SampleFormat sampleFormat      = static_cast<SampleFormat>(0);  ///< 0 ⇒ do not check
};

/// Why an attach did not succeed - and whether to try again.
enum class AttachResult : uint32_t {
    Ok,
    NotReady,    ///< magic == 0: segment exists, header not written yet. RETRY.
    BadVersion,  ///< versionMajor mismatch, or an unknown enum value.
    Malformed,   ///< wrong magic, or an offset that does not fit mappedBytes.
    Mismatch,    ///< valid, but not the structure this consumer expects.
                 ///< Waiting will not fix it.
};

constexpr bool shouldRetry(AttachResult r) noexcept { return r == AttachResult::NotReady; }

inline const char* toCString(AttachResult r) noexcept
{
    switch (r) {
        case AttachResult::Ok:         return "Ok";
        case AttachResult::NotReady:   return "NotReady";
        case AttachResult::BadVersion: return "BadVersion";
        case AttachResult::Malformed:  return "Malformed";
        case AttachResult::Mismatch:   return "Mismatch";
    }
    return "?";
}

/// deviceState with the staleness question already answered.
enum class StreamState : uint32_t {
    Live,     ///< Active, and publishing on time
    Quiet,    ///< Inactive - engine up, this device not driven. Legitimate.
    Stalled,  ///< Active, but lastPublishQpc is stale. Wedged or crashed.
    Closed,   ///< stop() ran
};

// ---------------------------------------------------------------------------
// Reader - bytes in a buffer to blocks. The liveness helpers take nowQpc as a
// parameter so they are unit-testable.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#  pragma warning(push)
// C4324: struct padded due to alignas - expected, m_hdr is alignas(64).
#  pragma warning(disable: 4324)
#endif
class Reader {
public:
    /// `expected` may be null ⇒ no structural assertion.
    AttachResult attach(const void* base, size_t mappedBytes,
                        const StreamRequest* expected = nullptr) noexcept
    {
        m_base        = static_cast<const uint8_t*>(base);
        m_mappedBytes = mappedBytes;
        m_attached    = false;
        m_dropped     = 0;
        m_mismatch[0] = '\0';

        if (!m_base || mappedBytes < sizeof(ShmStreamHeader)) return AttachResult::Malformed;

        const ShmStreamHeader& live = liveHeader();

        // A segment is zero-filled at creation: magic == 0 means "header not
        // written yet", not an error.
        const uint32_t magic = irtactile::shm::detail::acquireLoad(live.magic);
        if (magic == 0)      return AttachResult::NotReady;
        if (magic != kMagic) return AttachResult::Malformed;

        if (!snapshotHeader()) return AttachResult::NotReady;

        if (m_hdr.versionMajor != kVersionMajor) return AttachResult::BadVersion;
        // A higher versionMinor is tolerated: additives live behind runtime
        // offset fields. Unknown enum values are NOT - they would misread the
        // payload silently. Checked here for the BadVersion result, and again
        // inside validateLayout() so a rewrite cannot smuggle one past a
        // reader that is already attached.
        if (!knownEnums(m_hdr)) return AttachResult::BadVersion;

        if (!validateLayout()) return AttachResult::Malformed;

        if (expected) {
            const AttachResult r = checkRequest(*expected);
            if (r != AttachResult::Ok) return r;
        }

        m_attached = true;

        // Start at the head, not 0: the producer may have been running for
        // hours. rewindToOldest() reads whatever history the ring holds.
        m_cursor = irtactile::shm::detail::acquireLoad(live.writeSeq);
        return AttachResult::Ok;
    }

    /// After Mismatch: which fields disagreed and by how much, ready to log.
    /// Empty in every other case. Valid until the next attach().
    const char* mismatchDetail() const noexcept { return m_mismatch; }

    bool attached() const noexcept { return m_attached; }

    /// Start from the oldest block the ring still holds rather than from the
    /// head. The readable window is ringBlocks - 1: the writer owns slot
    /// head % ringBlocks for the whole of the current publish period.
    void rewindToOldest() noexcept
    {
        if (!m_attached) return;
        const uint64_t head    = irtactile::shm::detail::acquireLoad(liveHeader().writeSeq);
        const uint64_t history = m_hdr.ringBlocks - 1;
        m_cursor = head > history ? head - history : 0;
    }

    /// Lossless: the next block in sequence. False when caught up.
    bool next(BlockView& out) noexcept
    {
        if (!m_attached) return false;
        if (!refreshIfReconfigured()) return false;

        const uint64_t head = irtactile::shm::detail::acquireLoad(liveHeader().writeSeq);
        if (head == m_cursor) return false;

        const uint64_t window = m_hdr.ringBlocks - 1;

        // Unsigned differences, never a '<': correct under wrap and under a
        // cursor briefly ahead of head (torn 32-bit read of writeSeq).
        if (head - m_cursor > window) {
            m_dropped += (head - m_cursor) - window;
            m_cursor = head - window;
        }

        while (m_cursor != head) {
            const uint64_t seq = m_cursor;
            ++m_cursor;
            if (copyBlock(seq, out)) return true;
            // Recycled or under construction: count it and move on.
            ++m_dropped;
        }
        return false;
    }

    /// Latest-value-wins: the newest complete block, or false if there is none
    /// newer than the one already returned.
    bool latest(BlockView& out) noexcept
    {
        if (!m_attached) return false;
        if (!refreshIfReconfigured()) return false;

        const uint64_t head = irtactile::shm::detail::acquireLoad(liveHeader().writeSeq);
        if (head == 0 || head == m_cursor) return false;

        const uint64_t seq = head - 1;
        m_cursor = head;
        if (!copyBlock(seq, out)) { ++m_dropped; return false; }
        return true;
    }

    // --- layout-derived accessors, from the seqlock snapshot ---
    uint32_t channelCount() const noexcept      { return m_hdr.channelCount; }
    uint32_t samplesPerBlock() const noexcept   { return m_hdr.samplesPerBlock; }
    uint32_t ringBlocks() const noexcept        { return m_hdr.ringBlocks; }
    uint32_t sampleRateMilliHz() const noexcept { return m_hdr.sampleRateMilliHz; }
    uint32_t sourceSampleRate() const noexcept  { return m_hdr.sourceSampleRate; }
    uint32_t rateDivisor() const noexcept       { return m_hdr.rateDivisor; }
    SampleFormat sampleFormat() const noexcept  { return static_cast<SampleFormat>(m_hdr.sampleFormat); }
    uint64_t qpcFrequency() const noexcept      { return m_hdr.qpcFrequency; }
    const char* deviceName() const noexcept     { return m_hdr.deviceName; }
    const char* deviceGuid() const noexcept     { return m_hdr.deviceGuid; }
    uint32_t producerPid() const noexcept       { return m_hdr.producerPid; }

    /// Bit c ⇒ channel c has a source in the live graph. Read live, not from
    /// the snapshot: it is a concurrent field, not seqlock-protected.
    uint32_t wiredChannelMask() const noexcept
    {
        return m_attached ? irtactile::shm::detail::acquireLoad(liveHeader().wiredChannelMask) : 0;
    }

    uint64_t droppedBlocks() const noexcept { return m_dropped; }
    uint64_t writeSeq() const noexcept
    {
        return m_attached ? irtactile::shm::detail::acquireLoad(liveHeader().writeSeq) : 0;
    }
    uint32_t layoutSeq() const noexcept
    {
        return m_attached ? irtactile::shm::detail::acquireLoad(liveHeader().layoutSeq) : 0;
    }

    DeviceState deviceState() const noexcept
    {
        return m_attached ? static_cast<DeviceState>(irtactile::shm::detail::acquireLoad(liveHeader().deviceState))
                          : DeviceState::Inactive;
    }

    uint64_t lastPublishQpc() const noexcept
    {
        return m_attached ? irtactile::shm::detail::acquireLoad(liveHeader().lastPublishQpc) : 0;
    }

    /// Liveness, evaluated in one place. A stale timestamp is only meaningful
    /// under Active; Inactive and Closed are legitimate quiet.
    StreamState state(uint64_t nowQpc) const noexcept
    {
        switch (deviceState()) {
            case DeviceState::Closed:   return StreamState::Closed;
            case DeviceState::Inactive: return StreamState::Quiet;
            case DeviceState::Active:   break;
        }
        const uint64_t last = lastPublishQpc();
        const uint64_t freq = m_hdr.qpcFrequency;
        if (freq == 0 || last == 0 || nowQpc <= last) return StreamState::Live;
        const double elapsedMs = static_cast<double>(nowQpc - last) * 1000.0 / static_cast<double>(freq);
        return elapsedMs >= staleAfterMs() ? StreamState::Stalled : StreamState::Live;
    }

    /// Publish period, from samplesPerBlock and sampleRateMilliHz alone.
    double periodMs() const noexcept { return periodMsOf(m_hdr); }

    /// Blocks published per second - the figure the device configuration calls
    /// the update rate, and the reciprocal of periodMs().
    double publishHz() const noexcept { return publishHzOf(m_hdr); }

    double waitTimeoutMs() const noexcept { return waitTimeoutMsOf(m_hdr); }

    double staleAfterMs() const noexcept { return staleAfterMsOf(m_hdr); }

    /// Classification of the most recent snapshot change.
    enum class ReconfigureKind : uint32_t {
        None,     ///< No change since the last snapshot was taken
        Geometry, ///< An addressing-relevant field changed
        Cosmetic, ///< Only diagnostic / non-addressing fields changed
    };

    /// The kind of change detected by the last successful refreshSnapshot().
    ReconfigureKind lastReconfigureKind() const noexcept { return m_lastReconfigureKind; }

    /// Re-read layoutSeq and, if it moved, take a fresh header snapshot under
    /// the seqlock. Returns false if the new snapshot fails validation (the
    /// reader is detached). The change kind is available via
    /// lastReconfigureKind().
    bool refreshSnapshot() noexcept
    {
        if (irtactile::shm::detail::acquireLoad(liveHeader().layoutSeq) == m_snapLayoutSeq) {
            m_lastReconfigureKind = ReconfigureKind::None;
            return true;
        }
        if (!snapshotHeader())  { m_attached = false; return false; }
        if (!validateLayout())  { m_attached = false; return false; }
        return true;
    }

    /// The current header snapshot (layout fields only; the concurrent range
    /// is always read live). Valid for the layoutSeq it was taken under -
    /// re-read after wait() reports Reconfigured or ProducerRestarted.
    const ShmStreamHeader& header() const noexcept { return m_hdr; }

    /// Classify one header snapshot against a later one: Geometry if any
    /// addressing-relevant field moved, Cosmetic otherwise.
    ///
    /// Public because lastReconfigureKind() cannot answer "what changed since
    /// *I* last looked" - it reports the most recent refresh, and next()
    /// refreshes on every read cycle, so a reconfigure landing mid-drain is
    /// consumed before the caller asks and reads back as None. A consumer
    /// tracking reconfigures itself (its own wait loop over nativeEvent(), say)
    /// keeps a copy of header() as its watermark and calls this against it,
    /// which is what Client does internally. Every observer then gets an answer
    /// relative to its own watermark, with nothing to consume and no ordering
    /// to get wrong.
    ///
    /// Never returns None: gate the call on layoutSeq having moved. A producer
    /// restart that rewrites nothing still cycles the seqlock, and that is a
    /// restart the consumer must hear about - Cosmetic.
    static ReconfigureKind classifyReconfigure(const ShmStreamHeader& before,
                                               const ShmStreamHeader& after) noexcept
    {
        return classifyReconfigureKind(before, after);
    }

#ifdef IRTACTILE_SHM_TEST_ACCESS
    /// Test-only: drives one cursor step with an injected stale snapshot
    /// layoutSeq and skips the refresh, to reproduce a two-thread
    /// interleaving single-threaded.
    bool testCopyStepWithStaleSnapLayoutSeq(uint32_t staleSnapLayoutSeq, BlockView& out) noexcept
    {
        if (!m_attached) return false;
        m_snapLayoutSeq = staleSnapLayoutSeq;

        const uint64_t head = irtactile::shm::detail::acquireLoad(liveHeader().writeSeq);
        if (head == m_cursor) return false;

        const uint64_t seq = m_cursor;
        ++m_cursor;
        if (copyBlock(seq, out)) return true;
        ++m_dropped;
        return false;
    }
#endif

private:
    const ShmStreamHeader& liveHeader() const noexcept
    {
        return *reinterpret_cast<const ShmStreamHeader*>(m_base);
    }

    /// The layoutSeq seqlock: odd → rewrite → even.
    bool snapshotHeader() noexcept
    {
        const ShmStreamHeader& live = liveHeader();
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint32_t s1 = irtactile::shm::detail::acquireLoad(live.layoutSeq);
            if (s1 & 1u) continue;                       // rewrite in progress

            // Clamp to sizeof(ShmStreamHeader): a future producer may write
            // a larger headerBytes, but every offset we read is a runtime
            // field, so the v1 struct size is enough.
            uint32_t producerHeaderBytes = irtactile::shm::detail::acquireLoad(live.headerBytes);
            if (producerHeaderBytes == 0) producerHeaderBytes = static_cast<uint32_t>(sizeof(ShmStreamHeader));
            if (producerHeaderBytes > sizeof(ShmStreamHeader)) producerHeaderBytes = static_cast<uint32_t>(sizeof(ShmStreamHeader));
            if (producerHeaderBytes > m_mappedBytes) return false;
            const uint32_t bytes = producerHeaderBytes;

            const ShmStreamHeader prev = m_hdr;
            const bool hadSnapshot = m_attached;

            std::memset(&m_hdr, 0, sizeof(m_hdr));
            std::memcpy(&m_hdr, m_base, bytes);

            const uint32_t s2 = irtactile::shm::detail::acquireLoad(live.layoutSeq);
            if (s1 == s2) {
                m_snapLayoutSeq = s2;
                m_lastReconfigureKind = hadSnapshot ? classifyReconfigureKind(prev, m_hdr)
                                                    : ReconfigureKind::None;
                return true;
            }
        }
        return false;
    }

    /// The two enums a minor version may legitimately extend. A reader that
    /// falls through to a default instead of refusing misreads the payload
    /// silently (../../common/versioning.md, reader rule 3).
    static bool knownEnums(const ShmStreamHeader& h) noexcept
    {
        const SampleFormat fmt = static_cast<SampleFormat>(h.sampleFormat);
        if (fmt != SampleFormat::Float32 && fmt != SampleFormat::Int16) return false;
        // Unknown layouts and Interleaved (known, but reserved - no v1 producer
        // emits it) both land here: this reader only decodes planar payloads.
        return static_cast<ChannelLayout>(h.channelLayout) == ChannelLayout::Planar;
    }

    /// Every offset validated against the real mapped size, not against itself.
    bool validateLayout() noexcept
    {
        const ShmStreamHeader& h = m_hdr;
        // Re-checked on every snapshot, not only at attach: a producer restart
        // may rewrite either enum, and bytesPerSampleOf() answers 4 for any
        // unknown format - so without this an unknown sampleFormat would decode
        // as Float32, and Interleaved would decode as Planar, in both cases
        // silently.
        if (!knownEnums(h))                                     return false;
        if (h.totalBytes > m_mappedBytes)                       return false;
        if (h.headerBytes < 64 || h.headerBytes > h.totalBytes)  return false;
        if (h.channelCount < kMinChannels || h.channelCount > kMaxChannels) return false;
        if (h.ringBlocks < kMinRingBlocks || h.ringBlocks > kMaxRingBlocks) return false;
        if (h.samplesPerBlock == 0)                             return false;
        if (h.sampleRateMilliHz == 0)                           return false;
        if (h.rateDivisor == 0 || h.rateDivisor > kMaxRateDivisor) return false;
        if (h.bytesPerSample != bytesPerSampleOf(static_cast<SampleFormat>(h.sampleFormat)))
            return false;
        if (h.slotPayloadOffset < sizeof(ShmBlockDesc))          return false;

        // The plane-alignment guarantee ("planes are bytesPerSample-aligned")
        // is what lets a binding build a typed view - for Rust a *safety*
        // requirement - so it is validated, not assumed. All four terms of the
        // plane address must be multiples of bytesPerSample.
        if (h.slotArrayOffset   % h.bytesPerSample != 0)         return false;
        if (h.slotBytes         % h.bytesPerSample != 0)         return false;
        if (h.slotPayloadOffset % h.bytesPerSample != 0)         return false;
        if (h.channelPlaneBytes % h.bytesPerSample != 0)         return false;

        // >= rather than ==: a future producer may pad planes; the consumer
        // reads the stride it is given.
        const uint64_t minPlane = static_cast<uint64_t>(h.samplesPerBlock) * h.bytesPerSample;
        if (h.channelPlaneBytes < minPlane)                      return false;

        const uint64_t minSlot = static_cast<uint64_t>(h.slotPayloadOffset) +
                                 static_cast<uint64_t>(h.channelCount) * h.channelPlaneBytes;
        if (h.slotBytes < minSlot)                               return false;
        if (h.slotArrayOffset < h.headerBytes)                   return false;

        // The composite, not just the parts: each can be in range alone while
        // the array overruns.
        const uint64_t end = static_cast<uint64_t>(h.slotArrayOffset) +
                             static_cast<uint64_t>(h.ringBlocks) * h.slotBytes;
        if (end > m_mappedBytes || end > h.totalBytes)           return false;
        return true;
    }

    static ReconfigureKind classifyReconfigureKind(const ShmStreamHeader& old,
                                                   const ShmStreamHeader& cur) noexcept
    {
        if (old.channelCount        != cur.channelCount ||
            old.sampleFormat        != cur.sampleFormat ||
            old.channelLayout       != cur.channelLayout ||
            old.samplesPerBlock     != cur.samplesPerBlock ||
            old.ringBlocks          != cur.ringBlocks ||
            old.bytesPerSample      != cur.bytesPerSample ||
            old.slotPayloadOffset   != cur.slotPayloadOffset ||
            old.channelArrayOffset  != cur.channelArrayOffset ||
            old.slotArrayOffset     != cur.slotArrayOffset ||
            old.slotBytes           != cur.slotBytes ||
            old.channelPlaneBytes   != cur.channelPlaneBytes)
            return ReconfigureKind::Geometry;
        return ReconfigureKind::Cosmetic;
    }

    AttachResult checkRequest(const StreamRequest& want) noexcept
    {
        char   buf[256];
        size_t used = 0;
        auto note = [&](const char* field, unsigned long long got, unsigned long long expect) {
            if (used >= sizeof(buf) - 1) return;
            const int n = std::snprintf(buf + used, sizeof(buf) - used, "%s%s: segment has %llu, this build expects %llu",
                                        used ? "; " : "", field, got, expect);
            if (n > 0) used += static_cast<size_t>(n);
        };

        if (want.channelCount && want.channelCount != m_hdr.channelCount)
            note("channelCount", m_hdr.channelCount, want.channelCount);
        if (want.sampleRateMilliHz && want.sampleRateMilliHz != m_hdr.sampleRateMilliHz)
            note("sampleRateMilliHz", m_hdr.sampleRateMilliHz, want.sampleRateMilliHz);
        if (want.samplesPerBlock && want.samplesPerBlock != m_hdr.samplesPerBlock)
            note("samplesPerBlock", m_hdr.samplesPerBlock, want.samplesPerBlock);
        if (static_cast<uint32_t>(want.sampleFormat) &&
            static_cast<uint32_t>(want.sampleFormat) != m_hdr.sampleFormat)
            note("sampleFormat", m_hdr.sampleFormat, static_cast<unsigned long long>(want.sampleFormat));

        if (used == 0) return AttachResult::Ok;
        std::memcpy(m_mismatch, buf, used + 1 <= sizeof(m_mismatch) ? used + 1 : sizeof(m_mismatch));
        m_mismatch[sizeof(m_mismatch) - 1] = '\0';
        return AttachResult::Mismatch;
    }

    /// Re-check layoutSeq on every read cycle, so a producer restart rewriting
    /// the header is noticed. Cheap: the concurrent range shares writeSeq's
    /// cache line.
    bool refreshIfReconfigured() noexcept { return refreshSnapshot(); }

    /// The copy, with the two descriptor checks around it. Both are plain
    /// equality against the requested seq: kSlotBusy can never compare equal.
    bool copyBlock(uint64_t seq, BlockView& out) noexcept
    {
        const uint8_t*      slot = slotAt(m_base, m_hdr, seq);
        const ShmBlockDesc* desc = reinterpret_cast<const ShmBlockDesc*>(slot);

        if (irtactile::shm::detail::acquireLoad(desc->seq) != seq) return false;

        const uint32_t nCh = m_hdr.channelCount;

        // Every published block is full: a producer that cannot finish one
        // abandons it rather than publishing a short one, and reports the loss
        // through firstSampleIndex. So the bound is the header's configured
        // constant, taken from the seqlock snapshot whose extent
        // validateLayout() has already checked against channelPlaneBytes -
        // never from an unvalidated per-block field.
        const uint32_t count = m_hdr.samplesPerBlock;

        out.m_channelCount = nCh;
        out.m_stride       = count;
        out.m_samples.resize(static_cast<size_t>(nCh) * count);

        const bool isInt16 = static_cast<SampleFormat>(m_hdr.sampleFormat) == SampleFormat::Int16;

        // The whole payload is one run unless the producer pads its planes.
        // No v1 producer does - channelPlaneBytesOf() is exact - but
        // validateLayout() tolerates channelPlaneBytes > the minimum so a
        // future one could, and the strided fallback below is what makes that
        // tolerance honest. Drop the tolerance and this branch goes with it.
        const bool contiguous = static_cast<uint64_t>(m_hdr.channelPlaneBytes) ==
                                static_cast<uint64_t>(count) * m_hdr.bytesPerSample;

        float* const out0 = out.m_samples.data();

        if (contiguous) {
            const uint8_t* payload = slot + m_hdr.slotPayloadOffset;
            const size_t   total   = static_cast<size_t>(nCh) * count;
            if (isInt16) {
                // Unaligned loads: planes are only bytesPerSample-aligned.
                for (size_t i = 0; i < total; ++i) {
                    int16_t v;
                    std::memcpy(&v, payload + i * 2, 2);
                    out0[i] = int16ToFloat(v);
                }
            } else {
                std::memcpy(out0, payload, total * sizeof(float));
            }
        } else {
            for (uint32_t ch = 0; ch < nCh; ++ch) {
                const uint8_t* plane = planeAt(slot, m_hdr, ch);
                float*         dst   = out0 + static_cast<size_t>(ch) * count;
                if (isInt16) {
                    for (uint32_t i = 0; i < count; ++i) {
                        int16_t v;
                        std::memcpy(&v, plane + static_cast<size_t>(i) * 2, 2);
                        dst[i] = int16ToFloat(v);
                    }
                } else {
                    std::memcpy(dst, plane, static_cast<size_t>(count) * sizeof(float));
                }
            }
        }

        // The descriptor is copied INSIDE the bracket, with the planes: the
        // producer claims a recycled slot (kSlotBusy) before it rewrites these
        // fields, so reading them after the re-check could pair this block's
        // samples with the next block's firstSampleIndex - which is what
        // gap_samples is computed from.
        const uint64_t firstSampleIndex  = desc->firstSampleIndex;
        const uint64_t timestampQpc      = desc->timestampQpc;
        const uint32_t flags             = desc->flags;
        const uint32_t silentChannelMask = desc->silentChannelMask;

        // The slot may have been recycled, or the layout rewritten, mid-copy:
        // re-check the descriptor seq, the live layoutSeq, and the
        // descriptor's layoutEpoch.
        if (irtactile::shm::detail::acquireLoad(desc->seq) != seq) return false;
        if (irtactile::shm::detail::acquireLoad(liveHeader().layoutSeq) != m_snapLayoutSeq) return false;
        if (irtactile::shm::detail::acquireLoad(desc->layoutEpoch) != m_snapLayoutSeq) return false;

        out.seq               = seq;
        out.firstSampleIndex  = firstSampleIndex;
        out.timestampQpc      = timestampQpc;
        out.sampleCount       = count;
        out.flags             = flags;
        out.silentChannelMask = silentChannelMask;
        return true;
    }

    const uint8_t*  m_base        = nullptr;
    size_t          m_mappedBytes = 0;
    bool            m_attached    = false;
    ShmStreamHeader m_hdr{};
    uint32_t        m_snapLayoutSeq = 0;
    ReconfigureKind m_lastReconfigureKind = ReconfigureKind::None;
    uint64_t        m_cursor      = 0;
    uint64_t        m_dropped     = 0;
    char            m_mismatch[256] = {0};
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

} // namespace stream
} // namespace shm
} // namespace irtactile

#endif // IRTACTILE_SHM_STREAM_READER_H
