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

#include <irtactile/telemetry/channel_model.h>

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
inline constexpr uint32_t kMaxFrameRetries = 4;

enum class ProducerState : uint32_t { Inactive = 0, Active = 1, Closed = 2 };
enum class ConsumerState : uint32_t { Idle = 0, Active = 1, Closed = 2 };

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
    uint8_t  reserved1[8];
};

/// Channel descriptor, 128 bytes.
struct ChannelDesc {
    char     name[irt::telemetry::kNameLen];
    char     unit[16];
    float    rangeMin;
    float    rangeMax;
    uint32_t kind;
    uint32_t flags;
    uint16_t arrayIndex;
    uint16_t arrayCapacity;
    uint8_t  reserved[28];
};

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
static_assert(offsetof(Header, simName) == 120 && offsetof(Header, reserved1) == 248, "identity block");
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

inline std::string inboundName(std::string_view sourceId)
{
    return std::string(kInboundPrefix) + std::string(sourceId);
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
    return id == "core" || id == "ext" || id == "sim" || id == "plugin" || id == "irtactile";
}

namespace detail {

constexpr bool arrayFits(uint32_t offset, uint64_t bytes, uint32_t headerBytes, uint32_t totalBytes) noexcept
{
    return offset % 8 == 0 && offset >= headerBytes && static_cast<uint64_t>(offset) + bytes <= totalBytes;
}

}  // namespace detail

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
    if (!simId.empty() && (!irt::telemetry::isValidOwnerId(simId) || isReservedSimId(simId))) {
        return AttachResult::Malformed;
    }
    std::string_view s;
    if (!fieldString(h.simName, sizeof(h.simName), s)) return AttachResult::Malformed;
    if (!fieldString(h.carId, sizeof(h.carId), s)) return AttachResult::Malformed;
    if (!fieldString(h.carName, sizeof(h.carName), s)) return AttachResult::Malformed;
    return AttachResult::Ok;
}

/// Validates a return header copy.
inline AttachResult validateReturnHeader(const ReturnHeader& h, size_t mappedBytes) noexcept
{
    if (h.magic == 0) return AttachResult::NotReady;
    if (h.magic != kReturnMagic) return AttachResult::Malformed;
    if (h.versionMajor != kVersionMajor) return AttachResult::BadVersion;
    if (h.headerBytes < kReturnHeaderBytes || h.headerBytes > mappedBytes) return AttachResult::Malformed;
    if (h.totalBytes < h.headerBytes || h.totalBytes > mappedBytes) return AttachResult::Malformed;
    if (h.consumerState > static_cast<uint32_t>(ConsumerState::Closed)) return AttachResult::Malformed;
    if (h.capacity != 0) {
        if (!isValidCapacity(h.capacity)) return AttachResult::Malformed;
        const uint64_t words = h.capacity / 64;
        if (!detail::arrayFits(h.acceptedOffset, words * 8, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
        if (!detail::arrayFits(h.demandedOffset, words * 8, h.headerBytes, h.totalBytes)) return AttachResult::Malformed;
    }
    return AttachResult::Ok;
}

/// Properties of a descriptor must equal the table entry of a core.* / ext.* name.
inline bool matchesTable(const ChannelDesc& d, const irt::telemetry::ChannelDef& def, uint32_t arrayElement) noexcept
{
    std::string_view unit;
    if (!fieldString(d.unit, sizeof(d.unit), unit) || unit != def.unit) return false;
    return d.rangeMin == def.rangeMin && d.rangeMax == def.rangeMax &&
           d.kind == static_cast<uint32_t>(def.kind) && d.flags == def.flags &&
           d.arrayCapacity == def.arrayCapacity && d.arrayIndex == (def.arrayCapacity != 0 ? arrayElement : 0u);
}

}  // namespace telemetry
}  // namespace shm
}  // namespace irtactile

#endif  // IRTACTILE_SHM_TELEMETRY_LAYOUT_H
