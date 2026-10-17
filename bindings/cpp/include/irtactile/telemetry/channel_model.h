// irtactile/telemetry/channel_model.h - the shared channel model: kinds, flags,
// name rules. Normative text: spec/telemetry-shm/channel_model.md.
//
// SPDX-License-Identifier: MIT
//
// Hand-written (the generated tables are core_channels.h / ext_channels.h).
// Header-only, no dependencies beyond the standard library.

#ifndef IRTACTILE_TELEMETRY_CHANNEL_MODEL_H
#define IRTACTILE_TELEMETRY_CHANNEL_MODEL_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace irt {
namespace telemetry {

/// Signal kind of a channel. Describes the data, not how it is played back.
enum class Kind : uint32_t {
    Continuous = 0,   ///< physical quantity sampled per frame
    Discrete   = 1,   ///< integer-valued (gear, enum, bool)
    Event      = 2,   ///< impulses; valid only in frames that carry an event
};

/// Channel flag bits. Bit 0 is reserved (producers write 0, readers ignore).
inline constexpr uint32_t kFlagNone     = 0;
inline constexpr uint32_t kFlagOptional = 1u << 1;  ///< producer may leave the channel invalid

/// Descriptor name field size, including the terminating NUL.
inline constexpr uint32_t kNameLen = 64;
/// Longest channel name (kNameLen - 1).
inline constexpr size_t   kNameMax = kNameLen - 1;
/// Longest owner id (sourceId / simId / pluginId).
inline constexpr size_t   kOwnerIdMax = 24;
/// Highest array element index representable in a name suffix.
inline constexpr uint32_t kArrayIndexMax = 65535;

/// One entry of a generated channel table. For an array table entry `name` is
/// the base name (without `[i]`) and `arrayCapacity` is the element count;
/// scalars have `arrayCapacity == 0`.
struct ChannelDef {
    const char* name;
    const char* unit;
    float       rangeMin;
    float       rangeMax;
    Kind        kind;
    uint32_t    flags;
    uint16_t    arrayCapacity;
};

namespace detail {

constexpr bool isLowerSegmentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
constexpr bool isLastSegmentChar(char c) {
    return isLowerSegmentChar(c) || (c >= 'A' && c <= 'Z');
}

}  // namespace detail

/// Owner id rule: `[a-z0-9_]{1,24}`. Says nothing about reserved ids.
constexpr bool isValidOwnerId(std::string_view id) {
    if (id.empty() || id.size() > kOwnerIdMax) return false;
    for (char c : id) {
        if (!detail::isLowerSegmentChar(c)) return false;
    }
    return true;
}

/// True for ids no external source or plugin may claim: the four namespaces,
/// `irtactile`, and the built-in simulator ids. (An external source may still
/// declare a built-in simulator id as its simId; see channel_model.md.)
constexpr bool isReservedOwnerId(std::string_view id) {
    constexpr std::string_view kReserved[] = {
        "core", "ext", "sim", "plugin", "irtactile",
        "iracing", "ams2", "lmu", "rf2", "ac", "acc", "ac_evo", "ac_rally", "rbr",
    };
    for (std::string_view r : kReserved) {
        if (id == r) return true;
    }
    return false;
}

/// Full channel-name validation (namespace, segments, characters, length,
/// optional `[index]` suffix). Owner-id reservation is a registry policy and
/// is not checked here.
constexpr bool isValidChannelName(std::string_view name) {
    if (name.empty() || name.size() > kNameMax) return false;

    // Optional trailing "[index]".
    std::string_view body = name;
    if (body.back() == ']') {
        const size_t open = body.rfind('[');
        if (open == std::string_view::npos) return false;
        const std::string_view digits = body.substr(open + 1, body.size() - open - 2);
        if (digits.empty() || digits.size() > 5) return false;
        if (digits.size() > 1 && digits.front() == '0') return false;
        uint32_t index = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return false;
            index = index * 10 + static_cast<uint32_t>(c - '0');
        }
        if (index > kArrayIndexMax) return false;
        body = body.substr(0, open);
        if (body.empty()) return false;
    }

    // Split on '.', validate every segment.
    size_t segments = 0;
    std::string_view first;
    size_t start = 0;
    while (true) {
        const size_t dot = body.find('.', start);
        const bool last = (dot == std::string_view::npos);
        const std::string_view seg = body.substr(start, last ? std::string_view::npos : dot - start);
        if (seg.empty()) return false;
        for (char c : seg) {
            if (last ? !detail::isLastSegmentChar(c) : !detail::isLowerSegmentChar(c)) return false;
        }
        if (segments == 0) first = seg;
        ++segments;
        if (last) break;
        start = dot + 1;
    }

    if (first == "core" || first == "ext") return segments >= 2;
    if (first == "sim" || first == "plugin") return segments >= 3;
    return false;
}

}  // namespace telemetry
}  // namespace irt

#endif  // IRTACTILE_TELEMETRY_CHANNEL_MODEL_H
