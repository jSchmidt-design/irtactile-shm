// irtactile/shm/stream/layout.h - the wire format: structs, constants, size
// arithmetic, addressing.
//
// SPDX-License-Identifier: MIT
//
// The half of the contract a foreign-language binding is generated from.

#ifndef IRTACTILE_SHM_STREAM_LAYOUT_H
#define IRTACTILE_SHM_STREAM_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace irtactile {
namespace shm {
namespace stream {

// ===========================================================================
// Constants and enumerations
// ===========================================================================

inline constexpr uint32_t kMagic        = 0x41535449u; // 'ITSA'
inline constexpr uint16_t kVersionMajor = 1;           // layout break
inline constexpr uint16_t kVersionMinor = 0;           // additive only

/// Stored in ShmBlockDesc::seq while the producer is filling the slot, and
/// replaced by the real block index on publish. The producer must store it
/// before the first sample of the new block lands; a reader that sees any
/// value other than the requested seq must discard the slot and resynchronise.
inline constexpr uint64_t kSlotBusy = ~uint64_t{0};

// Publish period derives from header fields only:
//
//     periodSeconds = samplesPerBlock / (sampleRateMilliHz / 1000)

enum class SampleFormat : uint32_t {
    Float32 = 1,
    Int16   = 2,
};

enum class ChannelLayout : uint32_t {
    Planar      = 1,
    Interleaved = 2,   // reserved; never produced by a v1.0 producer
};

/// What the producer is doing with this device.
///
/// Inactive is 0 so a freshly created, zero-filled section reads as "not
/// being driven" before the header is written.
enum class DeviceState : uint32_t {
    Inactive = 0,  ///< engine alive, this device is not being driven
    Active   = 1,  ///< being driven; blocks should be arriving
    Closed   = 2,  ///< stop() ran - clean shutdown
};

enum BlockFlags : uint32_t {
    BlockSilence       = 1u << 0,  ///< every channel is zero in this block
    BlockDiscontinuity = 1u << 1,  ///< first block after start/reconfigure
};

// ===========================================================================
// Layout
// ===========================================================================
//
// Little-endian, IEEE-754 float32, two's complement int16, natural alignment,
// no packing.
//
// Concurrency classes, as contiguous byte ranges:
//
//   Bytes [0, 80) and [112, 256)
//                   - pinned + layout fields, protected by the layoutSeq
//                     seqlock. Read them under the seqlock and discard any
//                     snapshot taken before the most recent even layoutSeq.
//   Bytes [80, 112) - the only concurrently-written fields. The seqlock does
//                     not cover them: read each with its own acquire load, at
//                     any time - even mid-rewrite, while layoutSeq is odd.
//                     Never take their values from a header snapshot.
//
// Pinned: magic, versionMajor, headerBytes, totalBytes, slotArrayOffset must
// never change within a section; consumers map once against them and never
// remap. Changing any of them requires a different section (different size
// class, different name). All other layout fields are rewritable under a
// layoutSeq bump, subject to computeTotalSize(new layout) <= totalBytes.

struct alignas(64) ShmStreamHeader {
    // --- pinned + layout fields; seqlock-protected (*) = pinned ---
    //                            byte offset - pinned by static_assert below
    uint32_t magic;               //   0  *pinned; kMagic; 0 ⇒ header not yet written
    uint16_t versionMajor;        //   4  *pinned; reader must reject a mismatch
    uint16_t versionMinor;        //   6  reader must tolerate a higher value
    uint32_t headerBytes;         //   8  *pinned; size of THIS header; never copy more
    uint32_t totalBytes;          //  12  *pinned; size of the whole section

    uint32_t sampleFormat;        //  16  SampleFormat; reject an unknown value
    uint32_t channelLayout;       //  20  ChannelLayout; reject an unknown value
    uint32_t channelCount;        //  24  1..32
    uint32_t sampleRateMilliHz;   //  28  operative rate ×1000; exact at every rate
    uint32_t sourceSampleRate;    //  32  provenance: rate before decimation (Hz)
    uint32_t rateDivisor;         //  36  provenance: decimation ratio, 1..1024
    uint32_t samplesPerBlock;     //  40  samples per channel in one block
    uint32_t ringBlocks;          //  44  ring depth, in slots, 2..256
    uint32_t bytesPerSample;      //  48  4 (Float32) or 2 (Int16)

    // --- layout fields; seqlock-protected (addressing). Read, never recompute ---
    uint32_t slotPayloadOffset;   //  52  slot base → plane 0
    uint32_t channelArrayOffset;  //  56  0 in v1: no channel table
    uint32_t slotArrayOffset;     //  60  *pinned; section base → slot[0]
    uint32_t slotBytes;           //  64  slot to slot; multiple of 64
    uint32_t channelPlaneBytes;   //  68  plane to plane inside a slot

    // --- layout fields; seqlock-protected (diagnostics). Never derive behaviour from these ---
    uint32_t diagEngineBlockSize; //  72  irTactile DSP block: 32/64/128
    uint32_t diagFramesPerPublish;//  76  irTactile frames batched per publish

    // --- concurrent fields; read directly with per-field acquire loads ---
    uint64_t writeSeq;            //  80  blocks published [acq/rel]
    uint64_t lastPublishQpc;      //  88  QPC at the last publish [acq/rel]
    uint32_t deviceState;         //  96  DeviceState [acq/rel]
    uint32_t layoutSeq;           // 100  odd ⇒ header being rewritten [acq/rel];
                                  //      a change invalidates every cached
                                  //      snapshot of a layout field
    uint32_t wiredChannelMask;    // 104  bit c ⇒ channel c has a source [acq/rel]
    uint32_t reserved1;           // 108  producer zeroes; reader ignores

    // --- layout fields again; seqlock-protected ---
    uint64_t qpcFrequency;        // 112  ticks/s, for both QPC fields
    char     deviceGuid[40];      // 120  UTF-8, NUL-terminated
    char     deviceName[64];      // 160  UTF-8, NUL-terminated
    uint32_t producerPid;         // 224  producer's OS process id; 0 ⇒ unknown.
                                  //      Diagnostics and zombie adjudication
                                  //      only - PIDs are reused, so never act
                                  //      on it without a staleness check.

    uint8_t  reserved[28];        // 228  → 256 B. Producer zeroes; reader IGNORES.
};

struct alignas(64) ShmBlockDesc {
    uint64_t seq;                 //   0  block index, or kSlotBusy while being
                                  //      filled; slot = seq % ringBlocks [acq/rel]
    uint64_t firstSampleIndex;    //   8  stream-rate samples emitted before this
                                  //      block, INCLUDING any later discarded
    uint64_t timestampQpc;        //  16  QPC at publish
    // There is no per-block sample count: every published block carries exactly
    // samplesPerBlock, so the loop bound comes from the header, whose extent
    // validateLayout() has already checked against the plane. A block that
    // cannot be finished is abandoned, never published short.
    uint32_t flags;               //  24  BlockFlags
    uint32_t silentChannelMask;   //  28  bit c set ⇒ channel c is all-zero here
    uint32_t layoutEpoch;         //  32  layoutSeq snapshot at publish; reader
                                  //      must reject a mismatch
    uint8_t  reserved[28];        //  36  → 64 B. Producer zeroes; reader IGNORES.
};

// ---------------------------------------------------------------------------
// ABI: every offset written out literally, so a binding's hardcoded numbers
// fail the build here when a change shifts them.
// ---------------------------------------------------------------------------
static_assert(sizeof(ShmStreamHeader) == 256, "ShmStreamHeader must be 256 bytes");
static_assert(alignof(ShmStreamHeader) == 64, "ShmStreamHeader must be 64-byte aligned");

static_assert(offsetof(ShmStreamHeader, magic)                ==   0, "");
static_assert(offsetof(ShmStreamHeader, versionMajor)         ==   4, "");
static_assert(offsetof(ShmStreamHeader, versionMinor)         ==   6, "");
static_assert(offsetof(ShmStreamHeader, headerBytes)          ==   8, "");
static_assert(offsetof(ShmStreamHeader, totalBytes)           ==  12, "");
static_assert(offsetof(ShmStreamHeader, sampleFormat)         ==  16, "");
static_assert(offsetof(ShmStreamHeader, channelLayout)        ==  20, "");
static_assert(offsetof(ShmStreamHeader, channelCount)         ==  24, "");
static_assert(offsetof(ShmStreamHeader, sampleRateMilliHz)    ==  28, "");
static_assert(offsetof(ShmStreamHeader, sourceSampleRate)     ==  32, "");
static_assert(offsetof(ShmStreamHeader, rateDivisor)          ==  36, "");
static_assert(offsetof(ShmStreamHeader, samplesPerBlock)      ==  40, "");
static_assert(offsetof(ShmStreamHeader, ringBlocks)           ==  44, "");
static_assert(offsetof(ShmStreamHeader, bytesPerSample)       ==  48, "");
static_assert(offsetof(ShmStreamHeader, slotPayloadOffset)    ==  52, "");
static_assert(offsetof(ShmStreamHeader, channelArrayOffset)   ==  56, "");
static_assert(offsetof(ShmStreamHeader, slotArrayOffset)      ==  60, "");
static_assert(offsetof(ShmStreamHeader, slotBytes)            ==  64, "");
static_assert(offsetof(ShmStreamHeader, channelPlaneBytes)    ==  68, "");
static_assert(offsetof(ShmStreamHeader, diagEngineBlockSize)  ==  72, "");
static_assert(offsetof(ShmStreamHeader, diagFramesPerPublish) ==  76, "");
static_assert(offsetof(ShmStreamHeader, writeSeq)             ==  80, "");
static_assert(offsetof(ShmStreamHeader, lastPublishQpc)       ==  88, "");
static_assert(offsetof(ShmStreamHeader, deviceState)          ==  96, "");
static_assert(offsetof(ShmStreamHeader, layoutSeq)            == 100, "");
static_assert(offsetof(ShmStreamHeader, wiredChannelMask)     == 104, "");
static_assert(offsetof(ShmStreamHeader, reserved1)            == 108, "");
static_assert(offsetof(ShmStreamHeader, qpcFrequency)         == 112, "");
static_assert(offsetof(ShmStreamHeader, deviceGuid)           == 120, "");
static_assert(offsetof(ShmStreamHeader, deviceName)           == 160, "");
static_assert(offsetof(ShmStreamHeader, producerPid)          == 224, "");
static_assert(offsetof(ShmStreamHeader, reserved)             == 228, "");

// 64-bit fields naturally aligned - the precondition for a tear-free read.
static_assert(offsetof(ShmStreamHeader, writeSeq)       % 8 == 0, "");
static_assert(offsetof(ShmStreamHeader, lastPublishQpc) % 8 == 0, "");
static_assert(offsetof(ShmStreamHeader, qpcFrequency)   % 8 == 0, "");

// No implicit padding: the member sum equals sizeof.
static_assert(sizeof(uint32_t) * 2 + sizeof(uint16_t) * 2 + sizeof(uint32_t) * 17 +
                  sizeof(uint64_t) * 2 + sizeof(uint32_t) * 4 + sizeof(uint64_t) +
                  40 + 64 + sizeof(uint32_t) + 28 == sizeof(ShmStreamHeader),
              "ShmStreamHeader has implicit padding");

static_assert(sizeof(ShmBlockDesc) == 64, "ShmBlockDesc must be 64 bytes");
static_assert(alignof(ShmBlockDesc) == 64, "ShmBlockDesc must be 64-byte aligned");
static_assert(offsetof(ShmBlockDesc, seq)               ==  0, "");
static_assert(offsetof(ShmBlockDesc, firstSampleIndex)  ==  8, "");
static_assert(offsetof(ShmBlockDesc, timestampQpc)      == 16, "");
static_assert(offsetof(ShmBlockDesc, flags)             == 24, "");
static_assert(offsetof(ShmBlockDesc, silentChannelMask) == 28, "");
static_assert(offsetof(ShmBlockDesc, layoutEpoch)       == 32, "");
static_assert(offsetof(ShmBlockDesc, reserved)          == 36, "");
static_assert(sizeof(uint64_t) * 3 + sizeof(uint32_t) * 3 + 28 == sizeof(ShmBlockDesc),
              "ShmBlockDesc has implicit padding");

// Plain layout, trivially copyable: what a foreign binding relies on.
static_assert(std::is_standard_layout<ShmStreamHeader>::value, "");
static_assert(std::is_trivially_copyable<ShmStreamHeader>::value, "");
static_assert(std::is_standard_layout<ShmBlockDesc>::value, "");
static_assert(std::is_trivially_copyable<ShmBlockDesc>::value, "");

// Accessing bytes [80, 112) correctly (acquire/release): see
// common/atomic.h.

// ===========================================================================
// Size arithmetic - for the producer and tests. Consumers read the header
// fields and never re-derive them.
// ===========================================================================

constexpr uint32_t align64(uint32_t v) noexcept { return (v + 63u) & ~uint32_t{63}; }

constexpr uint32_t bytesPerSampleOf(SampleFormat f) noexcept
{
    return f == SampleFormat::Int16 ? 2u : 4u;
}

/// Everything the layout is a function of.
struct LayoutParams {
    uint32_t     channelCount    = 1;
    uint32_t     samplesPerBlock = 1;
    uint32_t     ringBlocks      = 2;
    SampleFormat sampleFormat    = SampleFormat::Float32;
};

constexpr uint32_t channelPlaneBytesOf(const LayoutParams& p) noexcept
{
    return p.samplesPerBlock * bytesPerSampleOf(p.sampleFormat);
}

constexpr uint32_t slotPayloadOffsetOf() noexcept
{
    return static_cast<uint32_t>(sizeof(ShmBlockDesc));
}

/// Padded to a multiple of 64 so slots never share a cache line.
constexpr uint32_t slotBytesOf(const LayoutParams& p) noexcept
{
    return align64(slotPayloadOffsetOf() + p.channelCount * channelPlaneBytesOf(p));
}

constexpr uint32_t slotArrayOffsetOf() noexcept
{
    return align64(static_cast<uint32_t>(sizeof(ShmStreamHeader)));
}

/// 64-bit so an out-of-range config is detectable rather than wrapped.
constexpr uint64_t computeTotalSize(const LayoutParams& p) noexcept
{
    return static_cast<uint64_t>(slotArrayOffsetOf()) +
           static_cast<uint64_t>(p.ringBlocks) * slotBytesOf(p);
}

// ---------------------------------------------------------------------------
// Derived quantities - for consumers as well as producers
// ---------------------------------------------------------------------------

inline constexpr double kWaitTimeoutPeriods = 4.0;   ///< publish periods a wait covers
inline constexpr double kStaleAfterPeriods  = 5.0;   ///< publish periods that count as stalled

constexpr double periodSecondsOf(const ShmStreamHeader& h) noexcept
{
    if (h.sampleRateMilliHz == 0) return 0.0;
    return static_cast<double>(h.samplesPerBlock) * 1000.0 /
           static_cast<double>(h.sampleRateMilliHz);
}

constexpr double periodMsOf(const ShmStreamHeader& h) noexcept
{
    if (h.sampleRateMilliHz == 0) return 0.0;
    return static_cast<double>(h.samplesPerBlock) * 1000.0 * 1000.0 /
           static_cast<double>(h.sampleRateMilliHz);
}

constexpr double publishHzOf(const ShmStreamHeader& h) noexcept
{
    if (h.samplesPerBlock == 0) return 0.0;
    return static_cast<double>(h.sampleRateMilliHz) / 1000.0 /
           static_cast<double>(h.samplesPerBlock);
}

constexpr double ringSecondsOf(const ShmStreamHeader& h) noexcept
{
    return static_cast<double>(h.ringBlocks) * periodSecondsOf(h);
}

constexpr double waitTimeoutMsOf(const ShmStreamHeader& h) noexcept
{
    const double p = kWaitTimeoutPeriods * periodMsOf(h);
    return p > 50.0 ? p : 50.0;
}

constexpr double staleAfterMsOf(const ShmStreamHeader& h) noexcept
{
    const double p = kStaleAfterPeriods * periodMsOf(h);
    return p > 100.0 ? p : 100.0;
}

// ---------------------------------------------------------------------------
// Size classes
// ---------------------------------------------------------------------------
//
// Sections are sized into three discrete buckets; the suffix is part of the
// section and event names. A restart needing a larger layout switches name
// instead of colliding with a consumer still holding the old section open.

enum class SizeClass : uint32_t {
    S256K,   ///< 256 KiB section
    S512K,   ///< 512 KiB section
    S1M,     ///< 1 MiB section
};

inline constexpr uint64_t kSizeClassBytes[3] = {
    256ull * 1024,
    512ull * 1024,
    1024ull * 1024,
};

inline constexpr const char* kSizeClassSuffix[3] = {
    "_256k",
    "_512k",
    "_1m",
};

/// The config spelling of a size class. False on anything else, including null.
inline bool sizeClassFromString(const char* s, SizeClass& out) noexcept
{
    if (!s) return false;
    auto eq = [](const char* a, const char* b) noexcept {
        while (*a && *a == *b) { ++a; ++b; }
        return *a == '\0' && *b == '\0';
    };
    if (eq(s, "256k")) { out = SizeClass::S256K; return true; }
    if (eq(s, "512k")) { out = SizeClass::S512K; return true; }
    if (eq(s, "1m"))   { out = SizeClass::S1M;   return true; }
    return false;
}

/// The smallest class whose envelope holds `p`, sized against Float32 so an
/// int16→float32 switch keeps the same section name. Returns S1M when nothing
/// fits; the caller must still range-check computeTotalSize() against
/// kSizeClassBytes for the chosen class.
constexpr SizeClass smallestSizeClassFor(const LayoutParams& p) noexcept
{
    const LayoutParams envelope{p.channelCount, p.samplesPerBlock, p.ringBlocks,
                                SampleFormat::Float32};
    const uint64_t need = computeTotalSize(envelope);
    if (need <= kSizeClassBytes[0]) return SizeClass::S256K;
    if (need <= kSizeClassBytes[1]) return SizeClass::S512K;
    return SizeClass::S1M;
}

// ---------------------------------------------------------------------------
// The rate ladder - normative
// ---------------------------------------------------------------------------
//
// The 11 valid rates. 48000 / divisor is exact at every rate, which is why
// sampleRateMilliHz carries no floating point.

inline constexpr uint32_t kRateDivisors[11] = {
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024
};

/// The rate matching `hz` within 0.001 Hz - well under the smallest gap on
/// the ladder (~47 Hz, between divisors 512 and 1024). False if `hz` is
/// off-ladder.
inline bool rateDivisorFromHz(double hz, uint32_t& divisorOut) noexcept
{
    for (const uint32_t d : kRateDivisors) {
        const double ladderHz = 48000.0 / static_cast<double>(d);
        const double diff     = hz > ladderHz ? hz - ladderHz : ladderHz - hz;
        if (diff < 0.001) { divisorOut = d; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Limits - normative
// ---------------------------------------------------------------------------
inline constexpr uint32_t kMinChannels   = 1;
inline constexpr uint32_t kMaxChannels   = 32;    ///< a uint32_t mask covers this exactly
inline constexpr uint32_t kMinRingBlocks = 2;     ///< 1 offers no protection at all
inline constexpr uint32_t kMaxRingBlocks = 256;
inline constexpr uint32_t kMaxRateDivisor = 1024;
inline constexpr uint64_t kMaxTotalBytes = kSizeClassBytes[2];  ///< largest size class
inline constexpr double   kWarnRingSeconds = 1.0;               ///< warning, not an error

// ===========================================================================
// Addressing - the normative arithmetic
// ===========================================================================
//
//     slot_index   = seq % ringBlocks
//     slot_base    = base + slotArrayOffset + slot_index × slotBytes
//     descriptor   = slot_base
//     plane(ch)    = slot_base + slotPayloadOffset + ch × channelPlaneBytes
//     sample(ch,i) = plane(ch) + i × bytesPerSample     // i < samplesPerBlock
//
// All values are byte offsets from `base`.

constexpr size_t slotOffsetOf(const ShmStreamHeader& h, uint64_t seq) noexcept
{
    return static_cast<size_t>(h.slotArrayOffset) +
           static_cast<size_t>(seq % h.ringBlocks) * h.slotBytes;
}

constexpr size_t planeOffsetInSlot(const ShmStreamHeader& h, uint32_t ch) noexcept
{
    return static_cast<size_t>(h.slotPayloadOffset) +
           static_cast<size_t>(ch) * h.channelPlaneBytes;
}

inline uint8_t* slotAt(uint8_t* base, const ShmStreamHeader& h, uint64_t seq) noexcept
{
    return base + slotOffsetOf(h, seq);
}

inline const uint8_t* slotAt(const uint8_t* base, const ShmStreamHeader& h, uint64_t seq) noexcept
{
    return base + slotOffsetOf(h, seq);
}

inline uint8_t* planeAt(uint8_t* slot, const ShmStreamHeader& h, uint32_t ch) noexcept
{
    return slot + planeOffsetInSlot(h, ch);
}

inline const uint8_t* planeAt(const uint8_t* slot, const ShmStreamHeader& h, uint32_t ch) noexcept
{
    return slot + planeOffsetInSlot(h, ch);
}

// ===========================================================================
// int16 conversion - clamping, not wrapping
// ===========================================================================
//
// The 32767 convention: ±1.0 maps to ±32767, so the round trip is exact for
// every value the producer emits. The asymmetric consequence is that a raw
// -32768 decodes to -1.00003, marginally outside [-1, 1]; a consumer that
// needs a hard bound clamps after conversion.

inline int16_t floatToInt16(float v) noexcept
{
    float s = v * 32767.0f;
    if (s > 32767.0f)  s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return static_cast<int16_t>(s < 0.0f ? s - 0.5f : s + 0.5f);
}

inline float int16ToFloat(int16_t v) noexcept
{
    return static_cast<float>(v) * (1.0f / 32767.0f);
}

} // namespace stream
} // namespace shm
} // namespace irtactile

#endif // IRTACTILE_SHM_STREAM_LAYOUT_H
