// Unit tests for the shared channel model (spec/channels/README.md):
// the name rules and the invariants of the generated std table.

#include <doctest/doctest.h>

#include <irtactile/shm/channels/model.h>
#include <irtactile/shm/channels/std.h>

#include <set>
#include <string>

using namespace irtactile::shm::channels;

TEST_CASE("isValidChannelName accepts the documented forms") {
    CHECK(isValidChannelName("std.engine.rpm"));
    CHECK(isValidChannelName("std.vehicle.velocity.lon"));
    CHECK(isValidChannelName("std.suspension.deflection[4]"));
    CHECK(isValidChannelName("std.wheel.load[0]"));
    CHECK(isValidChannelName("sim.iracing.brake_bias"));
    CHECK(isValidChannelName("sim.myrig.pump_pressure"));
    CHECK(isValidChannelName("plugin.kerbs.hit.FL"));
    CHECK(isValidChannelName("plugin.kerbs.hit[65535]"));
}

TEST_CASE("isValidChannelName rejects malformed names") {
    CHECK_FALSE(isValidChannelName(""));
    CHECK_FALSE(isValidChannelName("std"));
    CHECK_FALSE(isValidChannelName("std."));
    CHECK_FALSE(isValidChannelName(".std.engine"));
    CHECK_FALSE(isValidChannelName("std..engine"));
    CHECK_FALSE(isValidChannelName("core.engine.rpm"));       // retired namespaces
    CHECK_FALSE(isValidChannelName("ext.engine.rpm"));
    CHECK_FALSE(isValidChannelName("sim.iracing"));          // sim needs ns.owner.path
    CHECK_FALSE(isValidChannelName("plugin.kerbs"));
    CHECK_FALSE(isValidChannelName("other.engine.rpm"));     // unknown namespace
    CHECK_FALSE(isValidChannelName("std.Engine.rpm"));      // upper case outside the last segment
    CHECK_FALSE(isValidChannelName("std.engine.rp-m"));
    CHECK_FALSE(isValidChannelName("std.engine rpm"));
    CHECK_FALSE(isValidChannelName("std.wheel.load[]"));
    CHECK_FALSE(isValidChannelName("std.wheel.load[01]"));   // leading zero
    CHECK_FALSE(isValidChannelName("std.wheel.load[65536]"));
    CHECK_FALSE(isValidChannelName("std.wheel.load[123456]"));
    CHECK_FALSE(isValidChannelName("std.wheel.load[a]"));
    CHECK_FALSE(isValidChannelName("std.wheel.load[1]x"));
    CHECK_FALSE(isValidChannelName("std.wheel.load]"));
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

    for (const char* r : {"std", "sim", "plugin", "irtactile", "iracing", "ams2", "lmu", "rf2",
                          "ac", "acc", "ac_evo", "ac_rally", "rbr"}) {
        CHECK_MESSAGE(isReservedOwnerId(r), r);
    }
    CHECK_FALSE(isReservedOwnerId("myrig"));
    CHECK_FALSE(isReservedOwnerId("core"));   // no longer a namespace
    CHECK_FALSE(isReservedOwnerId("ext"));
}

namespace {

// Names and order copied from drafts/sim_channel_coverage_proposal.md segment 2.2 (the ids follow the order).
const char* const kScalarNames[] = {
    "std.session.status",
    "std.input.throttle", "std.input.brake", "std.input.clutch", "std.input.steering", "std.input.handbrake",
    "std.vehicle.speed",
    "std.vehicle.velocity.lon", "std.vehicle.velocity.lat", "std.vehicle.velocity.vert",
    "std.vehicle.accel.lon", "std.vehicle.accel.lat", "std.vehicle.accel.vert",
    "std.vehicle.yaw_rate", "std.vehicle.pitch_rate", "std.vehicle.roll_rate",
    "std.vehicle.orientation.pitch", "std.vehicle.orientation.roll",
    "std.vehicle.pit_limiter", "std.vehicle.boost_active",
    "std.assist.abs_level", "std.assist.tc_active",
    "std.engine.rpm", "std.engine.max_rpm", "std.engine.throttle", "std.engine.torque",
    "std.engine.manifold_pressure", "std.engine.turbo_boost", "std.engine.ignition", "std.engine.starter",
    "std.engine.rev_limiter",
    "std.drivetrain.gear", "std.steering.torque", "std.aero.drs", "std.hybrid.deploy",
};
const char* const kWheelArrayNames[] = {
    "std.suspension.deflection", "std.suspension.velocity", "std.suspension.acceleration", "std.suspension.ride_height",
    "std.wheel.speed", "std.wheel.load", "std.wheel.slip_ratio", "std.wheel.slip_angle", "std.wheel.surface",
    "std.wheel.force.lat", "std.wheel.force.lon",
    "std.tyre.temp", "std.tyre.temp.inner", "std.tyre.temp.middle", "std.tyre.temp.outer",
    "std.tyre.pressure", "std.brake.temp",
};

}  // namespace

TEST_CASE("generated table: names are valid, prefixed, unique and match the proposal") {
    std::set<std::string> names;
    for (const ChannelDef& c : standard::kChannels) {
        INFO(c.name);
        CHECK(isValidChannelName(c.name));
        CHECK(std::string(c.name).rfind("std.", 0) == 0);
        CHECK(c.rangeMin < c.rangeMax);
        CHECK(c.flags == kFlagOptional);
        CHECK(c.arrayCapacity == 0);
        CHECK(c.layout == ArrayLayout::Index);
        CHECK(names.insert(c.name).second);
    }
    for (const ChannelDef& a : standard::kArrays) {
        INFO(a.name);
        CHECK(isValidChannelName(a.name));
        CHECK(std::string(a.name).rfind("std.", 0) == 0);
        CHECK(a.rangeMin < a.rangeMax);
        CHECK(a.flags == kFlagOptional);
        CHECK(a.layout != ArrayLayout::Index);
        CHECK(a.arrayCapacity == (a.layout == ArrayLayout::Wheel ? 8 : 4));
        CHECK(names.insert(a.name).second);
        for (unsigned i = 0; i < a.arrayCapacity; ++i) {
            CHECK(isValidChannelName(std::string(a.name) + "[" + std::to_string(i) + "]"));
        }
    }

    REQUIRE(standard::kCount == sizeof(kScalarNames) / sizeof(kScalarNames[0]));
    for (uint32_t i = 0; i < standard::kCount; ++i) CHECK(std::string(standard::kChannels[i].name) == kScalarNames[i]);
    constexpr size_t wheelArrays = sizeof(kWheelArrayNames) / sizeof(kWheelArrayNames[0]);
    REQUIRE(standard::kArrayCount == wheelArrays + 1);
    for (size_t i = 0; i < wheelArrays; ++i) {
        CHECK(std::string(standard::kArrays[i].name) == kWheelArrayNames[i]);
        CHECK(standard::kArrays[i].layout == ArrayLayout::Wheel);
    }
    CHECK(std::string(standard::kArrays[wheelArrays].name) == "std.suspension.third");
    CHECK(standard::kArrays[wheelArrays].layout == ArrayLayout::Axle);

    // 35 scalars + 17 wheel arrays x 8 + 1 axle array x 4.
    uint32_t handles = standard::kCount;
    for (const ChannelDef& a : standard::kArrays) handles += a.arrayCapacity;
    CHECK(handles == 175);
}

TEST_CASE("generated table: counts and enums agree") {
    CHECK(standard::kCount == sizeof(standard::kChannels) / sizeof(standard::kChannels[0]));
    CHECK(standard::kArrayCount == sizeof(standard::kArrays) / sizeof(standard::kArrays[0]));
    CHECK(std::string(standard::kChannels[static_cast<uint32_t>(standard::Id::EngineRpm)].name) == "std.engine.rpm");
    CHECK(std::string(standard::kChannels[static_cast<uint32_t>(standard::Id::SessionStatus)].name) == "std.session.status");
    CHECK(std::string(standard::kChannels[static_cast<uint32_t>(standard::Id::VehicleOrientationRoll)].name) ==
          "std.vehicle.orientation.roll");
    CHECK(std::string(standard::kArrays[static_cast<uint32_t>(standard::ArrayId::TyreTemp)].name) == "std.tyre.temp");
    CHECK(std::string(standard::kArrays[static_cast<uint32_t>(standard::ArrayId::TyreTempInner)].name) == "std.tyre.temp.inner");
    CHECK(std::string(standard::kArrays[static_cast<uint32_t>(standard::ArrayId::SuspensionThird)].name) == "std.suspension.third");
}

TEST_CASE("generated table: discrete channels and enums") {
    CHECK(standard::kChannels[static_cast<uint32_t>(standard::Id::DrivetrainGear)].kind == Kind::Discrete);
    CHECK(standard::kChannels[static_cast<uint32_t>(standard::Id::SessionStatus)].kind == Kind::Discrete);
    CHECK(standard::kChannels[static_cast<uint32_t>(standard::Id::EngineRpm)].kind == Kind::Continuous);
    CHECK(static_cast<uint32_t>(standard::ConnectionStatus::DrivingDataLoss) == 4);
    CHECK(static_cast<uint32_t>(standard::Surface::Water) == 11);
    CHECK(standard::kSurfaceReservedFirst == 12);
    CHECK(standard::kSurfaceReservedLast == 15);
    CHECK(std::string(standard::kArrays[static_cast<uint32_t>(standard::ArrayId::WheelSurface)].unit) == "std::Surface");
}

TEST_CASE("kind, flag, layout and provenance values are the wire values") {
    CHECK(static_cast<uint32_t>(Kind::Continuous) == 0);
    CHECK(static_cast<uint32_t>(Kind::Discrete) == 1);
    CHECK(static_cast<uint32_t>(Kind::Event) == 2);
    CHECK(kFlagOptional == 2u);
    CHECK(kNameLen == 64u);
    CHECK(static_cast<uint32_t>(ArrayLayout::Index) == 0);
    CHECK(static_cast<uint32_t>(ArrayLayout::Wheel) == 1);
    CHECK(static_cast<uint32_t>(ArrayLayout::Axle) == 2);
    CHECK(static_cast<uint32_t>(Provenance::Unspecified) == 0);
    CHECK(static_cast<uint32_t>(Provenance::Native) == 1);
    CHECK(static_cast<uint32_t>(Provenance::Derived) == 2);
    CHECK(static_cast<uint32_t>(Provenance::Estimated) == 3);
}
