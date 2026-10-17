// Unit tests for the shared channel model (spec/telemetry-shm/channel_model.md):
// the name rules and the invariants of the generated core/ext tables.

#include <doctest/doctest.h>

#include <irtactile/telemetry/channel_model.h>
#include <irtactile/telemetry/core_channels.h>
#include <irtactile/telemetry/ext_channels.h>

#include <set>
#include <string>

using namespace irt::telemetry;

TEST_CASE("isValidChannelName accepts the documented forms") {
    CHECK(isValidChannelName("core.engine.rpm"));
    CHECK(isValidChannelName("core.suspension.deflection.FL"));
    CHECK(isValidChannelName("ext.suspension.deflection[4]"));
    CHECK(isValidChannelName("ext.wheel.load[0]"));
    CHECK(isValidChannelName("sim.iracing.brake_bias"));
    CHECK(isValidChannelName("sim.myrig.pump_pressure"));
    CHECK(isValidChannelName("plugin.kerbs.hit.FL"));
    CHECK(isValidChannelName("plugin.kerbs.hit[65535]"));
}

TEST_CASE("isValidChannelName rejects malformed names") {
    CHECK_FALSE(isValidChannelName(""));
    CHECK_FALSE(isValidChannelName("core"));
    CHECK_FALSE(isValidChannelName("core."));
    CHECK_FALSE(isValidChannelName(".core.engine"));
    CHECK_FALSE(isValidChannelName("core..engine"));
    CHECK_FALSE(isValidChannelName("sim.iracing"));          // sim needs ns.owner.path
    CHECK_FALSE(isValidChannelName("plugin.kerbs"));
    CHECK_FALSE(isValidChannelName("other.engine.rpm"));     // unknown namespace
    CHECK_FALSE(isValidChannelName("core.Engine.rpm"));      // upper case outside the last segment
    CHECK_FALSE(isValidChannelName("core.engine.rp-m"));
    CHECK_FALSE(isValidChannelName("core.engine rpm"));
    CHECK_FALSE(isValidChannelName("ext.wheel.load[]"));
    CHECK_FALSE(isValidChannelName("ext.wheel.load[01]"));   // leading zero
    CHECK_FALSE(isValidChannelName("ext.wheel.load[65536]"));
    CHECK_FALSE(isValidChannelName("ext.wheel.load[123456]"));
    CHECK_FALSE(isValidChannelName("ext.wheel.load[a]"));
    CHECK_FALSE(isValidChannelName("ext.wheel.load[1]x"));
    CHECK_FALSE(isValidChannelName("ext.wheel.load]"));
    CHECK_FALSE(isValidChannelName("[1]"));
}

TEST_CASE("isValidChannelName enforces 63 characters") {
    const std::string prefix = "sim.x.";
    CHECK(isValidChannelName(prefix + std::string(63 - prefix.size(), 'a')));
    CHECK_FALSE(isValidChannelName(prefix + std::string(64 - prefix.size(), 'a')));
    // The [index] suffix counts toward the limit.
    const std::string base = "sim.x." + std::string(63 - 6 - 3, 'a');
    CHECK(isValidChannelName(base + "[1]"));
    CHECK_FALSE(isValidChannelName(base + "[12]"));
}

TEST_CASE("owner id rule and reserved ids") {
    CHECK(isValidOwnerId("myrig"));
    CHECK(isValidOwnerId("a_b_9"));
    CHECK(isValidOwnerId(std::string(24, 'a')));
    CHECK_FALSE(isValidOwnerId(std::string(25, 'a')));
    CHECK_FALSE(isValidOwnerId(""));
    CHECK_FALSE(isValidOwnerId("My"));
    CHECK_FALSE(isValidOwnerId("a.b"));

    for (const char* r : {"core", "ext", "sim", "plugin", "irtactile", "iracing", "ams2", "lmu", "rf2",
                          "ac", "acc", "ac_evo", "ac_rally", "rbr"}) {
        CHECK_MESSAGE(isReservedOwnerId(r), r);
    }
    CHECK_FALSE(isReservedOwnerId("myrig"));
}

namespace {

template <size_t N>
void checkTable(const ChannelDef (&table)[N], const char* ns, uint32_t expectedFlags, std::set<std::string>& names) {
    for (const ChannelDef& c : table) {
        INFO(c.name);
        CHECK(isValidChannelName(c.name));
        CHECK(std::string(c.name).rfind(std::string(ns) + ".", 0) == 0);
        CHECK(c.rangeMin <= c.rangeMax);
        CHECK(c.flags == expectedFlags);
        CHECK(names.insert(c.name).second);  // unique across every table
    }
}

}  // namespace

TEST_CASE("generated tables: names are valid, prefixed and unique") {
    std::set<std::string> names;
    checkTable(core::kChannels, "core", kFlagNone, names);
    checkTable(ext::kChannels, "ext", kFlagOptional, names);
    checkTable(ext::kArrays, "ext", kFlagOptional, names);
}

TEST_CASE("generated tables: counts and enums agree") {
    CHECK(core::kCount == sizeof(core::kChannels) / sizeof(core::kChannels[0]));
    CHECK(ext::kCount == sizeof(ext::kChannels) / sizeof(ext::kChannels[0]));
    CHECK(ext::kArrayCount == sizeof(ext::kArrays) / sizeof(ext::kArrays[0]));

    CHECK(std::string(core::kChannels[static_cast<uint32_t>(core::Id::EngineRpm)].name) == "core.engine.rpm");
    CHECK(std::string(core::kChannels[static_cast<uint32_t>(core::Id::SessionStatus)].name) == "core.session.status");
    CHECK(std::string(core::kChannels[static_cast<uint32_t>(core::Id::SuspensionDeflectionFL)].name) ==
          "core.suspension.deflection.FL");
    CHECK(std::string(ext::kChannels[static_cast<uint32_t>(ext::Id::AeroDrs)].name) == "ext.aero.drs");
    CHECK(std::string(ext::kArrays[static_cast<uint32_t>(ext::ArrayId::WheelLoad)].name) == "ext.wheel.load");
}

TEST_CASE("generated tables: arrays are 1..8 elements and their elements are valid names") {
    for (const ChannelDef& a : ext::kArrays) {
        INFO(a.name);
        CHECK(a.arrayCapacity >= 1);
        CHECK(a.arrayCapacity <= 8);
        for (unsigned i = 0; i < a.arrayCapacity; ++i) {
            CHECK(isValidChannelName(std::string(a.name) + "[" + std::to_string(i) + "]"));
        }
    }
    for (const ChannelDef& c : core::kChannels) CHECK(c.arrayCapacity == 0);
    for (const ChannelDef& c : ext::kChannels) CHECK(c.arrayCapacity == 0);
}

TEST_CASE("generated tables: discrete channels and enums") {
    CHECK(core::kChannels[static_cast<uint32_t>(core::Id::DrivetrainGear)].kind == Kind::Discrete);
    CHECK(core::kChannels[static_cast<uint32_t>(core::Id::SessionStatus)].kind == Kind::Discrete);
    CHECK(core::kChannels[static_cast<uint32_t>(core::Id::EngineRpm)].kind == Kind::Continuous);
    CHECK(static_cast<uint32_t>(core::ConnectionStatus::DrivingDataLoss) == 4);
    CHECK(static_cast<uint32_t>(ext::Surface::Water) == 11);
    CHECK(ext::kSurfaceReservedFirst == 12);
    CHECK(ext::kSurfaceReservedLast == 15);
    // 4 corners per per-wheel family.
    CHECK(std::string(ext::kChannels[static_cast<uint32_t>(ext::Id::WheelSurfaceRR)].unit) == "ext::Surface");
}

TEST_CASE("kind and flag values are the wire values") {
    CHECK(static_cast<uint32_t>(Kind::Continuous) == 0);
    CHECK(static_cast<uint32_t>(Kind::Discrete) == 1);
    CHECK(static_cast<uint32_t>(Kind::Event) == 2);
    CHECK(kFlagOptional == 2u);
    CHECK(kNameLen == 64u);
}
