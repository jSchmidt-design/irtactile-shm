// Channel model: name rules and invariants of the generated std table.
//
// SPDX-License-Identifier: MIT

using IrTactile.Shm.Channels;

namespace IrTactile.Shm.Tests.Channels;

public class ChannelModelTests
{
    [Theory]
    [InlineData("std.engine.rpm")]
    [InlineData("std.vehicle.velocity.lon")]
    [InlineData("std.suspension.deflection[4]")]
    [InlineData("sim.iracing.brake_bias")]
    [InlineData("plugin.kerbs.hit.FL")]
    [InlineData("plugin.kerbs.hit[65535]")]
    public void ValidNames(string name) => Assert.True(ChannelModel.IsValidChannelName(name));

    [Theory]
    [InlineData("")]
    [InlineData("std")]
    [InlineData("std..engine")]
    [InlineData("core.engine.rpm")]
    [InlineData("ext.engine.rpm")]
    [InlineData("sim.iracing")]
    [InlineData("other.engine.rpm")]
    [InlineData("std.Engine.rpm")]
    [InlineData("std.wheel.load[]")]
    [InlineData("std.wheel.load[01]")]
    [InlineData("std.wheel.load[65536]")]
    [InlineData("std.wheel.load[1]x")]
    [InlineData("[1]")]
    public void InvalidNames(string name) => Assert.False(ChannelModel.IsValidChannelName(name));

    [Fact]
    public void NameLengthLimit()
    {
        string prefix = "sim.x.";
        Assert.True(ChannelModel.IsValidChannelName(prefix + new string('a', 63 - prefix.Length)));
        Assert.False(ChannelModel.IsValidChannelName(prefix + new string('a', 64 - prefix.Length)));
    }

    [Fact]
    public void OwnerIds()
    {
        Assert.True(ChannelModel.IsValidOwnerId("myrig"));
        Assert.False(ChannelModel.IsValidOwnerId("My"));
        Assert.False(ChannelModel.IsValidOwnerId(new string('a', 25)));
        Assert.True(ChannelModel.IsReservedOwnerId("lmu"));
        Assert.True(ChannelModel.IsReservedOwnerId("plugin"));
        Assert.True(ChannelModel.IsReservedOwnerId("std"));
        Assert.False(ChannelModel.IsReservedOwnerId("core"));
        Assert.False(ChannelModel.IsReservedOwnerId("myrig"));
    }

    // Names and order copied from drafts/sim_channel_coverage_proposal.md section 2.2 (the ids follow the order).
    private static readonly string[] ScalarNames =
    {
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

    private static readonly string[] WheelArrayNames =
    {
        "std.suspension.deflection", "std.suspension.velocity", "std.suspension.acceleration", "std.suspension.ride_height",
        "std.wheel.speed", "std.wheel.load", "std.wheel.slip_ratio", "std.wheel.slip_angle", "std.wheel.surface",
        "std.wheel.force.lat", "std.wheel.force.lon",
        "std.tyre.temp", "std.tyre.temp.inner", "std.tyre.temp.middle", "std.tyre.temp.outer",
        "std.tyre.pressure", "std.brake.temp",
    };

    [Fact]
    public void GeneratedTableIsConsistent()
    {
        var names = new HashSet<string>();
        foreach (var c in Std.All)
        {
            Assert.True(ChannelModel.IsValidChannelName(c.Name), c.Name);
            Assert.StartsWith("std.", c.Name);
            Assert.True(c.RangeMin < c.RangeMax, c.Name);
            Assert.Equal(ChannelFlags.Optional, c.Flags);
            Assert.Equal(0, c.ArrayCapacity);
            Assert.Equal(ArrayLayout.Index, c.Layout);
            Assert.True(names.Add(c.Name), c.Name);
        }
        int handles = Std.All.Length;
        foreach (var a in Std.Arrays)
        {
            Assert.True(ChannelModel.IsValidChannelName(a.Name), a.Name);
            Assert.StartsWith("std.", a.Name);
            Assert.True(a.RangeMin < a.RangeMax, a.Name);
            Assert.Equal(ChannelFlags.Optional, a.Flags);
            Assert.NotEqual(ArrayLayout.Index, a.Layout);
            Assert.Equal(a.Layout == ArrayLayout.Wheel ? 8 : 4, a.ArrayCapacity);
            Assert.True(names.Add(a.Name), a.Name);
            handles += a.ArrayCapacity;
        }
        Assert.Equal(175, handles);

        Assert.Equal(ScalarNames, Std.All.Select(c => c.Name));
        Assert.Equal(WheelArrayNames.Length + 1, Std.Arrays.Length);
        for (int i = 0; i < WheelArrayNames.Length; i++)
        {
            Assert.Equal(WheelArrayNames[i], Std.Arrays[i].Name);
            Assert.Equal(ArrayLayout.Wheel, Std.Arrays[i].Layout);
        }
        Assert.Equal("std.suspension.third", Std.Arrays[^1].Name);
        Assert.Equal(ArrayLayout.Axle, Std.Arrays[^1].Layout);

        Assert.Equal("std.engine.rpm", Std.All[(int)StdChannel.EngineRpm].Name);
        Assert.Equal("std.tyre.temp.inner", Std.Arrays[(int)StdArray.TyreTempInner].Name);
        Assert.Equal(4u, (uint)ConnectionStatus.DrivingDataLoss);
        Assert.Equal(11u, (uint)Surface.Water);
        Assert.Equal("std::Surface", Std.Arrays[(int)StdArray.WheelSurface].Unit);
    }

    [Fact]
    public void LayoutAndProvenanceValuesAreTheWireValues()
    {
        Assert.Equal(0, (int)ArrayLayout.Index);
        Assert.Equal(1, (int)ArrayLayout.Wheel);
        Assert.Equal(2, (int)ArrayLayout.Axle);
        Assert.Equal(0, (int)Provenance.Unspecified);
        Assert.Equal(1, (int)Provenance.Native);
        Assert.Equal(2, (int)Provenance.Derived);
        Assert.Equal(3, (int)Provenance.Estimated);
    }
}
