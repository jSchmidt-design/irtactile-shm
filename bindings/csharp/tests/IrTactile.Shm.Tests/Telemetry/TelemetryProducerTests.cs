// telemetry-shm v1 producer: the encode cases of the conformance suite
// (conformance/telemetry-shm/v1) must reproduce the golden segment bytes. The golden bytes come
// from the C++ binding, so a pass proves two independent implementations write the same layout.
//
// SPDX-License-Identifier: MIT

using System.Runtime.InteropServices;
using System.Text.Json;
using IrTactile.Shm.Channels;
using IrTactile.Shm.Telemetry;

namespace IrTactile.Shm.Tests.Telemetry;

public unsafe class TelemetryProducerTests
{
    private static string FindDir()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            string candidate = Path.Combine(dir.FullName, "conformance", "telemetry-shm", "v1");
            if (File.Exists(Path.Combine(candidate, "manifest.json"))) return candidate;
            dir = dir.Parent;
        }
        throw new FileNotFoundException("conformance/telemetry-shm/v1 not found");
    }

    private static readonly string Dir = FindDir();

    [Fact]
    public void WheelsPerAxleValidation()
    {
        Assert.True(TelemetryLayout.IsValidWheelsPerAxle(new byte[] { }));
        Assert.True(TelemetryLayout.IsValidWheelsPerAxle(new byte[] { 2, 2 }));
        Assert.True(TelemetryLayout.IsValidWheelsPerAxle(new byte[] { 2, 4, 4 }));
        Assert.True(TelemetryLayout.IsValidWheelsPerAxle(new byte[] { 1, 1, 1, 1, 1, 1, 1, 8 }));
        Assert.False(TelemetryLayout.IsValidWheelsPerAxle(new byte[] { 0, 2 }));
        Assert.False(TelemetryLayout.IsValidWheelsPerAxle(new byte[] { 2, 2, 0, 2 }));
        Assert.False(TelemetryLayout.IsValidWheelsPerAxle(new byte[] { 9 }));
    }

    [Fact]
    public void StringsAreTruncatedWithoutSplittingUtf8()
    {
        int bytes = TelemetryLayout.RequiredBytes(64);
        var buffer = new byte[bytes + 8];
        var pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try
        {
            byte* aligned = (byte*)(((long)pin.AddrOfPinnedObject() + 7) & ~7L);
            var p = new TelemetryProducer();
            Assert.Equal(TelemetryOpenResult.Ok, p.Open(aligned, bytes, 64, new ProducerIdentity()));
            string x30 = new string('x', 30);
            p.SetCar("car", x30 + "é");   // 32 bytes; the field holds 31 + NUL, byte 31 would split the 'é'
            Assert.True(p.CommitLayout());
            var carName = new Span<byte>(aligned + TelemetryLayout.OffCarName, 32);
            Assert.Equal(x30, System.Text.Encoding.UTF8.GetString(carName.Slice(0, carName.IndexOf((byte)0)).ToArray()));
        }
        finally
        {
            pin.Free();
        }
    }

    [Fact]
    public void PollReturn_ConsumerDiedMidPublication_AgesIntoGone()
    {
        if (!OperatingSystem.IsWindows()) return;
        string name = "irTactile_Telemetry_test_cs_oddret_" + Environment.ProcessId;
        using var segment = TelemetrySegment.Create(name, 64, new ProducerIdentity());
        var p = segment.Producer;
        Assert.NotEqual(TelemetryProducer.InvalidSlot, p.Declare(FindDef("std.engine.rpm")));
        Assert.True(p.CommitLayout());

        // A consumer's return region: slot 0 accepted, not demanded, Active and fresh.
        using var ret = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateNew(
            TelemetryLayout.ReturnNameFor(name), TelemetryLayout.ReturnBytes);
        using var view = ret.CreateViewAccessor();
        byte* r = null;
        view.SafeMemoryMappedViewHandle.AcquirePointer(ref r);
        try
        {
            *(uint*)r = TelemetryLayout.ReturnMagic;
            *(ushort*)(r + 4) = TelemetryLayout.VersionMajor;
            *(uint*)(r + TelemetryLayout.ROffCapacity) = 64;
            *(uint*)(r + TelemetryLayout.ROffAccepted) = TelemetryLayout.ReturnHeaderBytes;
            *(uint*)(r + TelemetryLayout.ROffDemanded) = TelemetryLayout.ReturnHeaderBytes + 8;
            *(uint*)(r + TelemetryLayout.ROffForLayoutSeq) = p.LayoutSeq;
            *(uint*)(r + TelemetryLayout.ROffConsumerState) = (uint)ConsumerState.Active;
            *(ulong*)(r + TelemetryLayout.ROffLastConsume) = TelemetryClock.Now;
            *(uint*)(r + TelemetryLayout.ROffConsumerPid) = (uint)Environment.ProcessId;
            *(ulong*)(r + TelemetryLayout.ReturnHeaderBytes) = 0b1;
            *(ulong*)(r + TelemetryLayout.ROffRetSeq) = 2;
            *(ulong*)(r + TelemetryLayout.ROffRetSeqEnd) = 2;

            var rv = new TelemetryReturnView();
            Assert.True(segment.PollReturn(rv));
            Assert.False(rv.IsDemanded(0));

            *(ulong*)(r + TelemetryLayout.ROffRetSeq) = 3;    // the consumer crashes mid-publication
            Assert.False(segment.PollReturn(rv));
            Assert.False(rv.ConsumerGone);                      // not yet stale
            Assert.False(rv.IsDemanded(0));

            rv.LastConsumeQpc = TelemetryClock.Now - 2 * TelemetryClock.Frequency;   // time passes without a good read
            Assert.False(segment.PollReturn(rv));
            Assert.True(rv.ConsumerGone);
            Assert.True(rv.IsDemanded(0));                      // every accepted slot, not the stale mask

            Assert.NotEqual(TelemetryProducer.InvalidSlot, p.Declare(FindDef("std.engine.throttle")));
            Assert.True(p.CommitLayout());                      // a new layout while the region is stuck
            Assert.False(segment.PollReturn(rv));
            Assert.False(rv.IsAccepted(0));                     // the masks of the old layout are dropped
        }
        finally
        {
            view.SafeMemoryMappedViewHandle.ReleasePointer();
        }
    }

    public static IEnumerable<object[]> EncodeCases()
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(Dir, "manifest.json")));
        foreach (JsonElement c in doc.RootElement.GetProperty("cases").EnumerateArray())
        {
            if (c.GetProperty("op").GetString() == "encode") yield return new object[] { c.GetProperty("name").GetString()! };
        }
    }

    private static ChannelDef FindDef(string name)
    {
        foreach (var d in Std.All) if (d.Name == name) return d;
        foreach (var d in Std.Arrays) if (d.Name == name) return d;
        throw new InvalidOperationException("unknown channel " + name);
    }

    private static SignalKind KindOf(string s) => s switch
    {
        "continuous" => SignalKind.Continuous,
        "discrete" => SignalKind.Discrete,
        "event" => SignalKind.Event,
        _ => throw new InvalidOperationException(s),
    };

    [Theory]
    [MemberData(nameof(EncodeCases))]
    public void Encode_ReproducesGoldenBytes(string name)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(Dir, "manifest.json")));
        JsonElement c = doc.RootElement.GetProperty("cases").EnumerateArray()
            .Single(e => e.GetProperty("name").GetString() == name);

        int bytes = c.GetProperty("bytes").GetInt32();
        var buffer = new byte[bytes + 8];                 // +8: room to align
        var pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try
        {
            byte* raw = (byte*)pin.AddrOfPinnedObject();
            byte* aligned = (byte*)(((long)raw + 7) & ~7L);
            var segment = new Span<byte>(aligned, bytes);
            if (c.TryGetProperty("initial", out JsonElement init))
            {
                File.ReadAllBytes(Path.Combine(Dir, init.GetString()!)).AsSpan(0, bytes).CopyTo(segment);
            }

            var p = new TelemetryProducer();
            foreach (JsonElement op in c.GetProperty("script").EnumerateArray())
            {
                switch (op.GetProperty("op").GetString())
                {
                    case "open":
                        Assert.Equal(TelemetryOpenResult.Ok, p.Open(aligned, bytes, op.GetProperty("capacity").GetInt32(),
                            new ProducerIdentity
                            {
                                SimId = op.TryGetProperty("simId", out var si) ? si.GetString()! : "",
                                SimName = op.TryGetProperty("simName", out var sn) ? sn.GetString()! : "",
                                QpcFrequency = op.TryGetProperty("qpcFrequency", out var qf) ? qf.GetUInt64() : 10_000_000UL,
                                Pid = op.TryGetProperty("pid", out var pid) ? pid.GetUInt32() : 4242u,
                            }));
                        break;
                    case "declare":
                        var def = FindDef(op.GetProperty("name").GetString()!);
                        var prov = (Provenance)(op.TryGetProperty("provenance", out var pv) ? pv.GetInt32() : 0);
                        int count = op.TryGetProperty("count", out var cnt) ? cnt.GetInt32() : 0;
                        Assert.NotEqual(TelemetryProducer.InvalidSlot,
                            def.ArrayCapacity != 0 ? p.DeclareArray(def, count, prov) : p.Declare(def, prov));
                        break;
                    case "wheels":
                        p.SetWheelLayout(op.GetProperty("list").EnumerateArray().Select(e => e.GetByte()).ToArray());
                        break;
                    case "declareLocal":
                        Assert.NotEqual(TelemetryProducer.InvalidSlot, p.DeclareLocal(
                            op.GetProperty("name").GetString()!,
                            op.TryGetProperty("unit", out var u) ? u.GetString()! : "",
                            op.TryGetProperty("min", out var mn) ? mn.GetSingle() : 0f,
                            op.TryGetProperty("max", out var mx) ? mx.GetSingle() : 1f,
                            KindOf(op.TryGetProperty("kind", out var k) ? k.GetString()! : "continuous"),
                            op.TryGetProperty("flags", out var fl) ? (ChannelFlags)fl.GetUInt32() : ChannelFlags.None));
                        break;
                    case "car":
                        p.SetCar(op.GetProperty("id").GetString()!, op.TryGetProperty("name", out var cn) ? cn.GetString()! : "");
                        break;
                    case "commit":
                        Assert.True(p.CommitLayout());
                        break;
                    case "state":
                        p.SetState(op.GetProperty("state").GetString() switch
                        {
                            "inactive" => ProducerState.Inactive,
                            "active" => ProducerState.Active,
                            _ => ProducerState.Closed,
                        }, op.TryGetProperty("now", out var now) ? now.GetUInt64() : 0UL);
                        break;
                    case "frame":
                        p.BeginFrame();
                        if (op.TryGetProperty("set", out var set))
                            foreach (var kv in set.EnumerateObject()) p.Set(int.Parse(kv.Name), kv.Value.GetSingle());
                        if (op.TryGetProperty("event", out var ev))
                            foreach (var kv in ev.EnumerateObject()) p.Event(int.Parse(kv.Name), kv.Value.GetSingle());
                        p.EndFrame(op.TryGetProperty("frameQpc", out var fq) ? fq.GetUInt64() : 0UL,
                                   op.TryGetProperty("now", out var nw) ? nw.GetUInt64() : 0UL);
                        break;
                    default:
                        throw new InvalidOperationException("unknown op " + op.GetProperty("op").GetString());
                }
            }

            byte[] golden = File.ReadAllBytes(Path.Combine(Dir, c.GetProperty("file").GetString()!));
            Assert.Equal(bytes, golden.Length);
            for (int i = 0; i < bytes; i++)
            {
                if (segment[i] != golden[i]) Assert.Fail($"byte {i}: got {segment[i]}, want {golden[i]}");
            }
        }
        finally
        {
            pin.Free();
        }
    }
}
