// Channel model: name rules and invariants of the generated core/ext tables.
//
// SPDX-License-Identifier: MIT

using IrTactile.Shm.Telemetry;

namespace IrTactile.Shm.Tests.Telemetry;

public class ChannelModelTests
{
    [Theory]
    [InlineData("core.engine.rpm")]
    [InlineData("core.suspension.deflection.FL")]
    [InlineData("ext.suspension.deflection[4]")]
    [InlineData("sim.iracing.brake_bias")]
    [InlineData("plugin.kerbs.hit.FL")]
    [InlineData("plugin.kerbs.hit[65535]")]
    public void ValidNames(string name) => Assert.True(ChannelModel.IsValidChannelName(name));

    [Theory]
    [InlineData("")]
    [InlineData("core")]
    [InlineData("core..engine")]
    [InlineData("sim.iracing")]
    [InlineData("other.engine.rpm")]
    [InlineData("core.Engine.rpm")]
    [InlineData("ext.wheel.load[]")]
    [InlineData("ext.wheel.load[01]")]
    [InlineData("ext.wheel.load[65536]")]
    [InlineData("ext.wheel.load[1]x")]
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
        Assert.False(ChannelModel.IsReservedOwnerId("myrig"));
    }

    [Fact]
    public void GeneratedTablesAreConsistent()
    {
        var names = new HashSet<string>();
        foreach (var c in CoreChannels.All)
        {
            Assert.True(ChannelModel.IsValidChannelName(c.Name), c.Name);
            Assert.StartsWith("core.", c.Name);
            Assert.Equal(ChannelFlags.None, c.Flags);
            Assert.True(names.Add(c.Name), c.Name);
        }
        foreach (var c in ExtChannels.All.Concat(ExtChannels.Arrays))
        {
            Assert.True(ChannelModel.IsValidChannelName(c.Name), c.Name);
            Assert.StartsWith("ext.", c.Name);
            Assert.Equal(ChannelFlags.Optional, c.Flags);
            Assert.True(names.Add(c.Name), c.Name);
        }
        foreach (var a in ExtChannels.Arrays)
        {
            Assert.InRange(a.ArrayCapacity, 1, 8);
        }
        Assert.Equal("core.engine.rpm", CoreChannels.All[(int)CoreChannel.EngineRpm].Name);
        Assert.Equal("ext.wheel.load", ExtChannels.Arrays[(int)ExtArray.WheelLoad].Name);
        Assert.Equal(4u, (uint)ConnectionStatus.DrivingDataLoss);
        Assert.Equal(11u, (uint)Surface.Water);
    }
}
