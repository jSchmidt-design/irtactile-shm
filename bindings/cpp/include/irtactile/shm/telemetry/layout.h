// irtactile/shm/telemetry/layout.h - telemetry-shm v1 byte layout: the inbound
// header, the channel descriptor, the return header, the addressing arithmetic
// and the header validation. Normative text: spec/telemetry-shm/v1.
//
// SPDX-License-Identifier: MIT
//
// Portable, header-only. No <windows.h>: the OS half is win32.h.

#ifndef IRTACTILE_SHM_TELEMETRY_LAYOUT_H
#define IRTACTILE_SHM_TELEMETRY_LAYOUT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <irtactile/shm/channels/model.h>

namespace irtactile {
namespace shm {
namespace telemetry {

inline constexpr uint32_t kMagic          = 0x45545449u;   ///< 'I','T','T','E' in memory
inline constexpr uint32_t kReturnMagic    = 0x52545449u;   ///< 'I','T','T','R' in memory
inline constexpr uint16_t kVersionMajor   = 1;
inline constexpr uint16_t kVersionMinor   = 0;
inline constexpr uint32_t kHeaderBytes    = 256;
inline constexpr uint32_t kReturnHeaderBytes = 128;
inline constexpr uint32_t kReturnBytes    = 4096;          ///< fixed size of the return region
inline constexpr uint32_t kMinCapacity    = 64;
inline constexpr uint32_t kMaxCapacity    = 8192;
inline constexpr uint32_t kDescBytes      = 128;
inline constexpr uint32_t kSimIdBytes     = 24;
inline constexpr uint32_t kMaxAxles       = 8;             ///< entries of Header::wheelsPerAxle
inline constexpr uint32_t kMaxWheelsPerAxle = 8;
inline constexpr uint32_t kMaxFrameRetries = 4;

enum class ProducerState : uint32_t { Inactive = 0, Active = 1, Closed = 2 };
enum class ConsumerState : uint32_t { Idle = 0, Active = 1, Closed = 2 };

inline const char* toCString(ProducerState s) noexcept
{
    switch (s) {
        case ProducerState::Inactive: return "Inactive";
        case ProducerState::Active:   return "Active";
        case ProducerState::Closed:   return "Closed";
    }
    return "?";
}

#pragma pack(push, 8)

/// Inbound header, 256 bytes. Field classes: see fields.md.
struct Header {
    // pinned
    uint32_t magic;
    uint16_t versionMajor;
    uint16_t versionMinor;
    uint32_t headerBytes;
    uint32_t totalBytes;
    // layout
    uint32_t capacity;
    uint32_t channelCount;
    uint32_t descOffset;
    uint32_t validOffset;
    uint32_t eventCountOffset;
    uint32_t valuesOffset;
    uint64_t qpcFrequency;
    uint32_t producerPid;
    uint32_t reserved0;
    char     simId[kSimIdBytes];
    // concurrent [80, 120)
    uint64_t frameSeq;
    uint64_t lastPublishQpc;
    uint32_t producerState;
    uint32_t layoutSeq;
    uint64_t frameSeqEnd;
    uint64_t frameQpc;
    // layout: session identity
    char     simName[32];
    char     carId[64];
    char     carName[32];
    uint8_t  wheelsPerAxle[kMaxAxles];   ///< front -> back; first 0 ends the list; all 0 = unknown
};

/// Channel descriptor, 128 bytes.
struct ChannelDesc {
    char     name[channels::kNameLen];
    char     unit[16];
    float    rangeMin;
    float    rangeMax;
    uint32_t kind;
    uint32_t flags;
    uint16_t arrayIndex;
    uint16_t arrayCapacity;
    uint8_t  provenance;     ///< channels::Provenance
    uint8_t  layout;         ///< channels::ArrayLayout
    uint8_t  reserved[26];
};

/// Typed views of the descriptor fields the section stores as plain integers.
constexpr channels::Kind kindOf(const ChannelDesc& d) noexcept { return static_cast<channels::Kind>(d.kind); }
constexpr channels::Provenance provenanceOf(const ChannelDesc& d) noexcept
{
    return static_cast<channels::Provenance>(d.provenance);
}
constexpr channels::ArrayLayout layoutOf(const ChannelDesc& d) noexcept
{
    return static_cast<channels::ArrayLayout>(d.layout);
}

/// Return header, 128 bytes.
struct ReturnHeader {
    uint32_t magic;
    uint16_t versionMajor;
    uint16_t versionMinor;
    uint32_t headerBytes;
    uint32_t totalBytes;
    uint32_t capacity;
    uint32_t acceptedOffset;
    uint32_t demandedOffset;
    uint32_t reserved0;
    uint64_t retSeq;
    uint64_t retSeqEnd;
    uint32_t forLayoutSeq;
    uint32_t consumerState;
    uint64_t lastConsumeQpc;
    uint32_t consumerPid;
    uint8_t  reserved1[60];
};

#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderBytes, "inbound header is 256 bytes");
static_assert(sizeof(ChannelDesc) == kDescBytes, "descriptor is 128 bytes");
static_assert(sizeof(ReturnHeader) == kReturnHeaderBytes, "return header is 128 bytes");
static_assert(offsetof(Header, frameSeq) == 80 && offsetof(Header, frameQpc) == 112, "concurrent range");
static_assert(offsetof(Header, simName) == 120 && offsetof(Header, wheelsPerAxle) == 248, "identity block");
static_assert(offsetof(ChannelDesc, provenance) == 100 && offsetof(ChannelDesc, layout) == 101, "descriptor tail");
static_assert(offsetof(ReturnHeader, retSeq) == 32 && offsetof(ReturnHeader, consumerPid) == 64, "return layout");

/// Offsets for a given capacity. `valid` first keeps every array 8-byte aligned.
struct Layout {
    uint32_t descOffset{0};
    uint32_t validOffset{0};
    uint32_t eventCountOffset{0};
    uint32_t valuesOffset{0};
    uint32_t totalBytes{0};
};

constexpr bool isValidCapacity(uint32_t capacity) noexcept
{
    return capacity >= kMinCapacity && capacity <= kMaxCapacity && capacity % 64 == 0;
}

/// Layout of a section holding `capacity` slots (must satisfy isValidCapacity).
constexpr Layout layoutFor(uint32_t capacity) noexcept
{
    Layout l;
    l.descOffset       = kHeaderBytes;
    l.validOffset      = l.descOffset + capacity * kDescBytes;
    l.eventCountOffset = l.validOffset + (capacity / 64) * 8;
    l.valuesOffset     = l.eventCountOffset + capacity * 4;
    l.totalBytes       = l.valuesOffset + capacity * 4;
    return l;
}

/// Offsets inside the return region for a given capacity.
constexpr uint32_t returnAcceptedOffset() noexcept { return kReturnHeaderBytes; }
constexpr uint32_t returnDemandedOffset(uint32_t capacity) noexcept
{
    return kReturnHeaderBytes + ((capacity / 8 + 7) & ~7u);
}

/// Events since the previous read: the modular difference of a free-running counter.
/// 0 = none. The consumer emits ONE event for any non-zero difference.
constexpr uint32_t eventsSince(uint32_t previous, uint32_t current) noexcept { return current - previous; }

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

inline constexpr std::string_view kInboundPrefix = "irTactile_Telemetry_";
inline constexpr std::string_view kReturnPrefix  = "irTactile_TelemetryReturn_";

inline std::string inboundName(std::string_view simId)
{
    return std::string(kInboundPrefix) + std::string(simId);
}

/// Return-region name derived from the inbound name alone (spec: Names).
inline std::string returnNameFor(std::string_view inbound)
{
    if (inbound.size() > kInboundPrefix.size() && inbound.substr(0, kInboundPrefix.size()) == kInboundPrefix) {
        return std::string(kReturnPrefix) + std::string(inbound.substr(kInboundPrefix.size()));
    }
    return std::string(inbound) + "_Return";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

enum class AttachResult : uint32_t {
    Ok,
    NotReady,    ///< magic == 0: header not yet written. Retry.
    BadVersion,  ///< versionMajor mismatch
    Malformed,   ///< anything else the spec rejects
};

inline const char* toCString(AttachResult r) noexcept
{
    switch (r) {
        case AttachResult::Ok:         return "Ok";
        case AttachResult::NotReady:   return "NotReady";
        case AttachResult::BadVersion: return "BadVersion";
        case AttachResult::Malformed:  return "Malformed";
    }
    return "?";
}

/// A NUL-terminated string within a field of `n` bytes (no NUL = malformed).
inline bool fieldString(const char* field, size_t n, std::string_view& out) noexcept
{
    const void* nul = std::memchr(field, 0, n);
    if (nul == nullptr) return false;
    out = std::string_view(field, static_cast<size_t>(static_cast<const char*>(nul) - field));
    return true;
}

inline bool isReservedSimId(std::string_view id) noexcept
{
    return id == "std" || id == "sim" || id == "plugin" || id == "irtactile";
}

namespace detail {

constexpr bool arrayFits(uint32_t offset, uint64_t bytes, uint32_t headerBytes, uint32_t totalBytes) noexcept
{
    return offset % 8 == 0 && offset >= headerBytes && static_cast<uint64_t>(offset) + bytes <= totalBytes;
}

}  // namespace detail

/// The wheels-per-axle list: entries <= kMaxWheelsPerAxle, no non-zero entry after a zero.
inline bool validWheelsPerAxle(const uint8_t (&w)[kMaxAxles]) noexcept
{
    bool ended = false;
    for (uint8_t n : w) {
        if (n == 0) { ended = true; continue; }
        if (ended || n > kMaxWheelsPerAxle) return false;
    }
    return true;
}

/// Validates a header copy against the size the OS reports for the mapping.
/// Pass the snapshot that will be used for addressing, never the live header.
inline AttachResult validateHeader(const Header& h, size_t mappedBytes) noexcept
{
    if (h.magic == 0) return AttachResult::NotReady;
    if (h.magic != kMagic) return AttachResult::Malformed;
    if (h.versionMajor != kVersionMajor) return AttachResult::BadVersion;
    if (h.headerBytes < kHeaderBytes || h.headerBytes > mappedBytes) return AttachResult::Malformed;
    if (h.totalBytes < h.headerBytes || h.totalBytes > mappedBytes) return AttachResult::Malformed;
    if (!isValidCapacity(h.capacity) || h.channelCount > h.capacity) return AttachResult::Malformed;
    const uint64_t cap = h.capacity;
    if (!detail::arrayFits(h.descOffset, cap * kDescBytes, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
    if (!detail::arrayFits(h.validOffset, (cap / 64) * 8, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
    if (!detail::arrayFits(h.eventCountOffset, cap * 4, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
    if (!detail::arrayFits(h.valuesOffset, cap * 4, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
    if (h.producerState > static_cast<uint32_t>(ProducerState::Closed)) return AttachResult::Malformed;

    std::string_view simId;
    if (!fieldString(h.simId, sizeof(h.simId), simId)) return AttachResult::Malformed;
    if (!simId.empty() && (!channels::isValidOwnerId(simId) || isReservedSimId(simId))) {
        return AttachResult::Malformed;
    }
    std::string_view s;
    if (!fieldString(h.simName, sizeof(h.simName), s)) return AttachResult::Malformed;
    if (!fieldString(h.carId, sizeof(h.carId), s)) return AttachResult::Malformed;
    if (!fieldString(h.carName, sizeof(h.carName), s)) return AttachResult::Malformed;
    if (!validWheelsPerAxle(h.wheelsPerAxle)) return AttachResult::Malformed;
    return AttachResult::Ok;
}

/// Validates the pinned and frame fields of a return header copy. The concurrent fields (`consumerState`,
/// `lastConsumeQpc`, `consumerPid`) are not checked: a copy may hold them mid-store, so a reader takes them from
/// live loads.
inline AttachResult validateReturnFrame(const ReturnHeader& h, size_t mappedBytes) noexcept
{
    if (h.magic == 0) return AttachResult::NotReady;
    if (h.magic != kReturnMagic) return AttachResult::Malformed;
    if (h.versionMajor != kVersionMajor) return AttachResult::BadVersion;
    if (h.headerBytes < kReturnHeaderBytes || h.headerBytes > mappedBytes) return AttachResult::Malformed;
    if (h.totalBytes < h.headerBytes || h.totalBytes > mappedBytes) return AttachResult::Malformed;
    if (h.capacity != 0) {
        if (!isValidCapacity(h.capacity)) return AttachResult::Malformed;
        const uint64_t words = h.capacity / 64;
        if (!detail::arrayFits(h.acceptedOffset, words * 8, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
        if (!detail::arrayFits(h.demandedOffset, words * 8, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
    }
    return AttachResult::Ok;
}

/// Validates a return header copy at attach: validateReturnFrame plus an unknown `consumerState`.
inline AttachResult validateReturnHeader(const ReturnHeader& h, size_t mappedBytes) noexcept
{
    const AttachResult r = validateReturnFrame(h, mappedBytes);
    if (r != AttachResult::Ok) return r;
    if (h.consumerState > static_cast<uint32_t>(ConsumerState::Closed)) return AttachResult::Malformed;
    return AttachResult::Ok;
}

/// Per-descriptor sanity: provenance and layout in range, layout only on arrays, index below capacity.
/// A consumer drops a descriptor that fails this (not the whole layout).
inline bool validateDescriptor(const ChannelDesc& d) noexcept
{
    if (d.provenance > static_cast<uint8_t>(channels::Provenance::Estimated)) return false;
    if (d.layout > static_cast<uint8_t>(channels::ArrayLayout::Axle)) return false;
    if (d.arrayCapacity == 0) return d.layout == static_cast<uint8_t>(channels::ArrayLayout::Index) && d.arrayIndex == 0;
    return d.arrayIndex < d.arrayCapacity;
}

/// Properties of a descriptor must equal the table entry of a std.* name. A producer may declare an array with
/// a smaller capacity than the table (`1 <= arrayCapacity <= def.arrayCapacity`); provenance is not compared.
inline bool matchesTable(const ChannelDesc& d, const channels::ChannelDef& def, uint32_t arrayElement) noexcept
{
    std::string_view unit;
    if (!fieldString(d.unit, sizeof(d.unit), unit) || unit != def.unit) return false;
    if (d.rangeMin != def.rangeMin || d.rangeMax != def.rangeMax ||
        d.kind != static_cast<uint32_t>(def.kind) || d.flags != def.flags ||
        d.layout != static_cast<uint8_t>(def.layout)) {
        return false;
    }
    if (def.arrayCapacity == 0) return d.arrayCapacity == 0 && d.arrayIndex == 0;
    return d.arrayCapacity >= 1 && d.arrayCapacity <= def.arrayCapacity &&
           d.arrayIndex < d.arrayCapacity && d.arrayIndex == arrayElement;
}

}  // namespace telemetry
}  // namespace shm
}  // namespace irtactile

#endif  // IRTACTILE_SHM_TELEMETRY_LAYOUT_H
