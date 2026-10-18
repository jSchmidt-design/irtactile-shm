// irtactile/shm/telemetry/producer.h - the producer side of telemetry-shm v1:
// declare channels, publish frames, publish the layout. Normative text:
// spec/telemetry-shm/v1/README.md ("Publish protocol").
//
// SPDX-License-Identifier: MIT
//
// Portable: it works on a caller-provided, 8-byte-aligned buffer (a mapped
// section, or a vector in a test) and takes QPC values as parameters. The
// Windows section handling is win32.h.
//
//     Producer p;
//     p.open(base, bytes, Producer::requiredBytes(256) ...);
//     const int rpm = p.declare(standard::Id::EngineRpm);
//     const int wave = p.declareLocal("demo_wave", "", -1.f, 1.f, Kind::Continuous);
//     p.commitLayout();
//     p.setState(ProducerState::Active, qpcNow);
//     for (;;) { p.beginFrame(); p.set(rpm, v); p.endFrame(qpcSample, qpcNow); }

#ifndef IRTACTILE_SHM_TELEMETRY_PRODUCER_H
#define IRTACTILE_SHM_TELEMETRY_PRODUCER_H

#include <cstdint>
#include <cstring>
#include <string>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include <irtactile/shm/common/atomic.h>
#include <irtactile/telemetry/std_channels.h>

#include "layout.h"

namespace irtactile {
namespace shm {
namespace telemetry {

/// Identity the producer publishes in the header.
struct Identity {
    std::string simId;          ///< namespace of local names; empty = the consumer's sourceId
    std::string simName;
    uint64_t    qpcFrequency{0};
    uint32_t    pid{0};
};

enum class OpenResult : uint32_t {
    Ok,
    TooSmall,   ///< the buffer (or the pinned totalBytes of an existing section) cannot hold the layout
    BadIdentity ///< simId is not a valid owner id or is reserved
};

class Producer {
public:
    static constexpr int kInvalidSlot = -1;

    /// Bytes a section needs for `capacity` slots.
    static constexpr uint32_t requiredBytes(uint32_t capacity) noexcept { return layoutFor(capacity).totalBytes; }

    /// Binds to the buffer. `base` must be 8-byte aligned. If the buffer already holds a valid header (a restart
    /// into an existing section) its pinned fields are kept; `capacity` is clamped to what fits.
    OpenResult open(void* base, size_t bytes, uint32_t capacity, Identity identity)
    {
        if (!irt::telemetry::isValidOwnerId(identity.simId) && !identity.simId.empty()) return OpenResult::BadIdentity;
        if (isReservedSimId(identity.simId)) return OpenResult::BadIdentity;
        m_base = static_cast<uint8_t*>(base);
        m_bytes = bytes;
        m_identity = std::move(identity);

        Header* h = header();
        m_total = static_cast<uint32_t>(bytes);
        if (h->magic == kMagic && h->headerBytes == kHeaderBytes && h->totalBytes <= bytes && h->totalBytes >= kHeaderBytes) {
            m_total = h->totalBytes;   // restart into an existing section: totalBytes is pinned
        }
        while (capacity >= kMinCapacity && layoutFor(capacity).totalBytes > m_total) capacity -= 64;
        if (!isValidCapacity(capacity)) return OpenResult::TooSmall;
        m_capacity = capacity;
        m_stage.clear();
        m_committed = false;
        return OpenResult::Ok;
    }

    [[nodiscard]] uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] uint32_t channelCount() const noexcept { return static_cast<uint32_t>(m_stage.size()); }

    // --- declaration (staged until commitLayout) ----------------------------

    /// A scalar from a generated table. Properties are copied from the table, so they always match.
    int declare(const irt::telemetry::ChannelDef& def, irt::telemetry::Provenance provenance = irt::telemetry::Provenance::Unspecified)
    {
        if (def.arrayCapacity != 0) return declareArray(def, 0, provenance);
        return addDesc(def.name, def.unit, def.rangeMin, def.rangeMax, def.kind, def.flags, 0, 0, def.layout, provenance);
    }
    int declare(irt::telemetry::standard::Id id, irt::telemetry::Provenance provenance = irt::telemetry::Provenance::Unspecified)
    {
        return declare(irt::telemetry::standard::kChannels[static_cast<uint32_t>(id)], provenance);
    }

    /// The first `count` elements of an array table entry (0 = the table capacity); the descriptors carry `count` as
    /// their arrayCapacity. Returns the slot of element 0, elements are consecutive.
    int declareArray(const irt::telemetry::ChannelDef& def, uint16_t count = 0,
                     irt::telemetry::Provenance provenance = irt::telemetry::Provenance::Unspecified)
    {
        if (def.arrayCapacity == 0) return declare(def, provenance);
        if (count == 0) count = def.arrayCapacity;
        if (count > def.arrayCapacity) return kInvalidSlot;
        if (m_stage.size() + count > m_capacity) return kInvalidSlot;
        int first = kInvalidSlot;
        for (uint16_t i = 0; i < count; ++i) {
            const std::string name = std::string(def.name) + "[" + std::to_string(i) + "]";
            const int s = addDesc(name, def.unit, def.rangeMin, def.rangeMax, def.kind, def.flags, i, count, def.layout, provenance);
            if (s == kInvalidSlot) return kInvalidSlot;
            if (first == kInvalidSlot) first = s;
        }
        return first;
    }
    int declareArray(irt::telemetry::standard::ArrayId id, uint16_t count = 0,
                     irt::telemetry::Provenance provenance = irt::telemetry::Provenance::Unspecified)
    {
        return declareArray(irt::telemetry::standard::kArrays[static_cast<uint32_t>(id)], count, provenance);
    }

    /// A producer-specific channel; the consumer publishes it as `sim.<simId>.<name>`.
    int declareLocal(std::string_view name, std::string_view unit, float rangeMin, float rangeMax,
                     irt::telemetry::Kind kind, uint32_t flags = irt::telemetry::kFlagNone,
                     irt::telemetry::Provenance provenance = irt::telemetry::Provenance::Unspecified)
    {
        if (name.empty() || name.size() > irt::telemetry::kNameMax) return kInvalidSlot;
        if (!irt::telemetry::isValidChannelName(std::string("sim.x.") + std::string(name))) return kInvalidSlot;
        return addDesc(name, unit, rangeMin, rangeMax, kind, flags, 0, 0, irt::telemetry::ArrayLayout::Index, provenance);
    }

    /// Wheels per axle, front to back (car `{2,2}`, motorcycle `{1,1}`). Empty = unknown. Entries are 1..8, at most 8
    /// axles; an invalid list is stored as unknown. A change after commitLayout() is published immediately.
    void setWheelLayout(const uint8_t* wheelsPerAxle, size_t axles)
    {
        uint8_t w[kMaxAxles] = {};
        if (axles <= kMaxAxles) {
            for (size_t i = 0; i < axles; ++i) w[i] = wheelsPerAxle[i];
        }
        if (!validWheelsPerAxle(w)) std::memset(w, 0, sizeof(w));
        std::memcpy(m_wheelsPerAxle, w, sizeof(w));
        if (m_committed) commitLayout();
    }

    void setWheelLayout(std::initializer_list<uint8_t> wheelsPerAxle)
    {
        setWheelLayout(wheelsPerAxle.begin(), wheelsPerAxle.size());
    }

    /// Session identity; a change after commitLayout() is a layout rewrite and is published immediately.
    void setCar(std::string_view carId, std::string_view carName)
    {
        m_carId.assign(carId.substr(0, sizeof(Header::carId) - 1));
        m_carName.assign(carName.substr(0, sizeof(Header::carName) - 1));
        if (m_committed) commitLayout();
    }

    /// Publishes header layout fields and the descriptor table under the layout seqlock. Also the first call
    /// that makes the section visible (magic is stored last). Clears the frame data.
    bool commitLayout()
    {
        if (m_base == nullptr) return false;
        Header* h = header();
        const Layout l = layoutFor(m_capacity);

        const uint32_t s = shm::detail::acquireLoad(h->layoutSeq);
        const uint32_t begin = s + 1 + (s & 1u);              // odd, and past a leftover odd value
        shm::detail::releaseStore(h->layoutSeq, begin);

        // Frame seqlock: step past a leftover odd value so readers never wedge on it.
        uint64_t fs = shm::detail::acquireLoad(h->frameSeq);
        if (fs & 1u) ++fs;

        if (h->magic != kMagic) {
            std::memset(m_base, 0, m_total);
            h->versionMajor = kVersionMajor;
            h->versionMinor = kVersionMinor;
            h->headerBytes = kHeaderBytes;
            h->totalBytes = m_total;
            h->layoutSeq = begin;
        }
        h->capacity = m_capacity;
        h->channelCount = static_cast<uint32_t>(m_stage.size());
        h->descOffset = l.descOffset;
        h->validOffset = l.validOffset;
        h->eventCountOffset = l.eventCountOffset;
        h->valuesOffset = l.valuesOffset;
        h->qpcFrequency = m_identity.qpcFrequency;
        h->producerPid = m_identity.pid;
        setString(h->simId, sizeof(h->simId), m_identity.simId);
        setString(h->simName, sizeof(h->simName), m_identity.simName);
        setString(h->carId, sizeof(h->carId), m_carId);
        setString(h->carName, sizeof(h->carName), m_carName);
        std::memcpy(h->wheelsPerAxle, m_wheelsPerAxle, sizeof(m_wheelsPerAxle));

        ChannelDesc* descs = reinterpret_cast<ChannelDesc*>(m_base + l.descOffset);
        std::memset(descs, 0, static_cast<size_t>(m_capacity) * kDescBytes);
        for (size_t i = 0; i < m_stage.size(); ++i) descs[i] = m_stage[i];
        std::memset(m_base + l.validOffset, 0, (m_capacity / 64) * 8);
        std::memset(m_base + l.eventCountOffset, 0, static_cast<size_t>(m_capacity) * 4);
        std::memset(m_base + l.valuesOffset, 0, static_cast<size_t>(m_capacity) * 4);

        shm::detail::releaseStore(h->frameSeq, fs);
        shm::detail::releaseStore(h->frameSeqEnd, fs);
        if (h->magic != kMagic) shm::detail::releaseStore(h->magic, kMagic);
        shm::detail::releaseStore(h->layoutSeq, begin + 1);
        m_committed = true;
        return true;
    }

    // --- frames --------------------------------------------------------------

    void beginFrame()
    {
        if (!m_committed) return;
        Header* h = header();
        shm::detail::releaseStore(h->frameSeq, shm::detail::acquireLoad(h->frameSeq) + 1);   // odd
        std::memset(m_base + h->validOffset, 0, (m_capacity / 64) * 8);
    }
    void set(int slot, float value) noexcept
    {
        if (!m_committed || slot < 0 || static_cast<uint32_t>(slot) >= m_stage.size()) return;
        values()[slot] = value;
        valid()[slot >> 6] |= uint64_t{1} << (slot & 63);
    }
    /// One event with `amplitude`; consumers see one event per read however many occur in between.
    void event(int slot, float amplitude) noexcept
    {
        if (!m_committed || slot < 0 || static_cast<uint32_t>(slot) >= m_stage.size()) return;
        ++eventCounts()[slot];
        set(slot, amplitude);
    }
    /// `frameQpc`: QPC of the sim sample. `nowQpc`: QPC of this publish (liveness).
    void endFrame(uint64_t frameQpc, uint64_t nowQpc)
    {
        if (!m_committed) return;
        Header* h = header();
        h->frameQpc = frameQpc;
        const uint64_t odd = shm::detail::acquireLoad(h->frameSeq);
        shm::detail::releaseStore(h->frameSeqEnd, odd + 1);
        shm::detail::releaseStore(h->frameSeq, odd + 1);
        shm::detail::releaseStore(h->lastPublishQpc, nowQpc);
    }
    void setState(ProducerState state, uint64_t nowQpc)
    {
        Header* h = header();
        shm::detail::releaseStore(h->lastPublishQpc, nowQpc);
        shm::detail::releaseStore(h->producerState, static_cast<uint32_t>(state));
    }

    [[nodiscard]] const Header& publishedHeader() const noexcept { return *reinterpret_cast<const Header*>(m_base); }

private:
    Header* header() noexcept { return reinterpret_cast<Header*>(m_base); }
    uint64_t* valid() noexcept { return reinterpret_cast<uint64_t*>(m_base + header()->validOffset); }
    uint32_t* eventCounts() noexcept { return reinterpret_cast<uint32_t*>(m_base + header()->eventCountOffset); }
    float* values() noexcept { return reinterpret_cast<float*>(m_base + header()->valuesOffset); }

    static void setString(char* dst, size_t n, std::string_view src) noexcept
    {
        std::memset(dst, 0, n);
        std::memcpy(dst, src.data(), src.size() < n - 1 ? src.size() : n - 1);
    }

    int addDesc(std::string_view name, std::string_view unit, float rangeMin, float rangeMax,
                irt::telemetry::Kind kind, uint32_t flags, uint16_t arrayIndex, uint16_t arrayCapacity,
                irt::telemetry::ArrayLayout layout = irt::telemetry::ArrayLayout::Index,
                irt::telemetry::Provenance provenance = irt::telemetry::Provenance::Unspecified)
    {
        if (m_stage.size() >= m_capacity || name.size() > irt::telemetry::kNameMax || unit.size() >= sizeof(ChannelDesc::unit)) {
            return kInvalidSlot;
        }
        ChannelDesc d{};
        setString(d.name, sizeof(d.name), name);
        setString(d.unit, sizeof(d.unit), unit);
        d.rangeMin = rangeMin;
        d.rangeMax = rangeMax;
        d.kind = static_cast<uint32_t>(kind);
        d.flags = flags;
        d.arrayIndex = arrayIndex;
        d.arrayCapacity = arrayCapacity;
        d.provenance = static_cast<uint8_t>(provenance);
        d.layout = static_cast<uint8_t>(layout);
        m_stage.push_back(d);
        return static_cast<int>(m_stage.size() - 1);
    }

    uint8_t*                 m_base{nullptr};
    size_t                   m_bytes{0};
    uint32_t                 m_total{0};
    uint32_t                 m_capacity{0};
    bool                     m_committed{false};
    Identity                 m_identity;
    std::string              m_carId;
    std::string              m_carName;
    uint8_t                  m_wheelsPerAxle[kMaxAxles] = {};
    std::vector<ChannelDesc> m_stage;
};

}  // namespace telemetry
}  // namespace shm
}  // namespace irtactile

#endif  // IRTACTILE_SHM_TELEMETRY_PRODUCER_H
