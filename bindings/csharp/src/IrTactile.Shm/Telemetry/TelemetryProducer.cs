// IrTactile.Shm.Telemetry - the producer side of telemetry-shm v1.
//
// SPDX-License-Identifier: MIT
//
// A managed implementation of spec/telemetry-shm/v1, written against the spec
// and checked against the same conformance vectors as the C++ binding
// (the encode cases compare section bytes).
//
//     using var s = TelemetrySection.Create(TelemetrySection.NameFromEnvironment("demo"), 256,
//                                           new ProducerIdentity { SimName = "My Sim" });
//     TelemetryProducer p = s.Producer;
//     int rpm = p.Declare(Std.All[(int)StdChannel.EngineRpm]);
//     p.CommitLayout();
//     p.SetState(ProducerState.Active, TelemetryClock.Now);
//     p.BeginFrame(); p.Set(rpm, 5000f); p.EndFrame(TelemetryClock.Now, TelemetryClock.Now);

using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Runtime.Versioning;
using System.Text;
using IrTactile.Shm.Channels;

namespace IrTactile.Shm.Telemetry;

/// <summary>Producer-side state published in the header.</summary>
public enum ProducerState : uint
{
    /// <summary>The producer is up, the sim is not running / sits in a menu.</summary>
    Inactive = 0,

    /// <summary>Frames are being published.</summary>
    Active = 1,

    /// <summary>Clean shutdown.</summary>
    Closed = 2,
}

/// <summary>Consumer-side state published in the return region.</summary>
public enum ConsumerState : uint
{
    /// <summary>irTactile is up, this source does not drive the bus.</summary>
    Idle = 0,

    /// <summary>This source drives the bus.</summary>
    Active = 1,

    /// <summary>irTactile shut down.</summary>
    Closed = 2,
}

/// <summary>Result of <see cref="TelemetryProducer.Open"/>.</summary>
public enum TelemetryOpenResult
{
    /// <summary>Bound.</summary>
    Ok,

    /// <summary>The buffer (or the pinned totalBytes of an existing section) cannot hold the layout.</summary>
    TooSmall,

    /// <summary>simId is not a valid owner id or is reserved.</summary>
    BadIdentity,
}

/// <summary>Identity the producer publishes in the header.</summary>
public sealed class ProducerIdentity
{
    /// <summary>Namespace of local names (<c>sim.&lt;simId&gt;.*</c>); empty = the consumer's sourceId.</summary>
    public string SimId { get; set; } = "";

    /// <summary>Display name of the simulator / source.</summary>
    public string SimName { get; set; } = "";

    /// <summary>QPC frequency; 0 = <see cref="Stopwatch.Frequency"/>.</summary>
    public ulong QpcFrequency { get; set; }

    /// <summary>Process id; 0 = the current process.</summary>
    public uint Pid { get; set; }
}

/// <summary>QPC readings (Stopwatch ticks are QPC ticks on Windows).</summary>
public static class TelemetryClock
{
    /// <summary>QueryPerformanceCounter().</summary>
    public static ulong Now => (ulong)Stopwatch.GetTimestamp();

    /// <summary>QueryPerformanceFrequency().</summary>
    public static ulong Frequency => (ulong)Stopwatch.Frequency;
}

/// <summary>Constants and name rules of telemetry-shm v1.</summary>
public static class TelemetryLayout
{
    /// <summary>'I','T','T','E' in memory.</summary>
    public const uint Magic = 0x45545449u;

    /// <summary>'I','T','T','R' in memory.</summary>
    public const uint ReturnMagic = 0x52545449u;

    public const int HeaderBytes = 256;
    public const int ReturnHeaderBytes = 128;
    public const int ReturnBytes = 4096;
    public const int DescBytes = 128;
    public const int MinCapacity = 64;
    public const int MaxCapacity = 8192;

    // Header offsets.
    internal const int OffMagic = 0, OffVersionMajor = 4, OffVersionMinor = 6, OffHeaderBytes = 8, OffTotalBytes = 12;
    internal const int OffCapacity = 16, OffChannelCount = 20, OffDescOffset = 24, OffValidOffset = 28;
    internal const int OffEventCountOffset = 32, OffValuesOffset = 36, OffQpcFrequency = 40, OffProducerPid = 48;
    internal const int OffSimId = 56, OffFrameSeq = 80, OffLastPublishQpc = 88, OffProducerState = 96;
    internal const int OffLayoutSeq = 100, OffFrameSeqEnd = 104, OffFrameQpc = 112, OffSimName = 120;
    internal const int OffCarId = 152, OffCarName = 216, OffWheelsPerAxle = 248;

    /// <summary>Entries of the header's wheels-per-axle list.</summary>
    public const int MaxAxles = 8;

    /// <summary>Most wheels one axle entry may hold.</summary>
    public const int MaxWheelsPerAxle = 8;

    // Return header offsets.
    internal const int ROffCapacity = 16, ROffAccepted = 20, ROffDemanded = 24, ROffRetSeq = 32, ROffRetSeqEnd = 40;
    internal const int ROffForLayoutSeq = 48, ROffConsumerState = 52, ROffLastConsume = 56, ROffConsumerPid = 64;

    /// <summary>True for a capacity the format accepts.</summary>
    public static bool IsValidCapacity(int capacity) =>
        capacity >= MinCapacity && capacity <= MaxCapacity && capacity % 64 == 0;

    /// <summary>Bytes a section needs for <paramref name="capacity"/> slots.</summary>
    public static int RequiredBytes(int capacity) =>
        HeaderBytes + capacity * DescBytes + (capacity / 64) * 8 + capacity * 4 + capacity * 4;

    /// <summary><c>irTactile_Telemetry_&lt;sourceId&gt;</c>.</summary>
    public static string InboundName(string sourceId) => "irTactile_Telemetry_" + sourceId;

    /// <summary>Return-region name derived from the inbound name alone.</summary>
    public static string ReturnNameFor(string inbound)
    {
        const string prefix = "irTactile_Telemetry_";
        if (inbound.Length > prefix.Length && inbound.StartsWith(prefix, StringComparison.Ordinal))
        {
            return "irTactile_TelemetryReturn_" + inbound.Substring(prefix.Length);
        }
        return inbound + "_Return";
    }

    internal static bool IsReservedSimId(string id) =>
        id == "std" || id == "sim" || id == "plugin" || id == "irtactile";

    /// <summary>The wheels-per-axle list: entries at most 8, no non-zero entry after a zero.</summary>
    public static bool IsValidWheelsPerAxle(ReadOnlySpan<byte> wheels)
    {
        bool ended = false;
        foreach (byte n in wheels)
        {
            if (n == 0) { ended = true; continue; }
            if (ended || n > MaxWheelsPerAxle) return false;
        }
        return true;
    }
}

/// <summary>
/// Declares channels and publishes frames into a caller-provided buffer (a mapped section, or a pinned
/// array in a test). Single-threaded.
/// </summary>
public sealed unsafe class TelemetryProducer
{
    /// <summary>Returned by the Declare methods when a channel cannot be declared.</summary>
    public const int InvalidSlot = -1;

    private byte* _base;
    private int _total;
    private int _capacity;
    private bool _committed;
    private ProducerIdentity _identity = new();
    private string _carId = "";
    private string _carName = "";
    private readonly byte[] _wheelsPerAxle = new byte[TelemetryLayout.MaxAxles];
    private readonly List<byte[]> _stage = new();

    /// <summary>Slots the bound layout holds.</summary>
    public int Capacity => _capacity;

    /// <summary>Slots declared so far.</summary>
    public int ChannelCount => _stage.Count;

    /// <summary>
    /// Binds to the buffer (8-byte aligned). If it already holds a valid header (a restart into an existing section)
    /// its pinned fields are kept and <paramref name="capacity"/> is clamped to what fits.
    /// </summary>
    public TelemetryOpenResult Open(byte* baseAddress, long bytes, int capacity, ProducerIdentity identity)
    {
        string simId = identity.SimId ?? "";
        if (simId.Length != 0 && (!ChannelModel.IsValidOwnerId(simId) || TelemetryLayout.IsReservedSimId(simId)))
        {
            return TelemetryOpenResult.BadIdentity;
        }
        _base = baseAddress;
        _identity = identity;
        _total = (int)bytes;
        uint existingMagic = *(uint*)(_base + TelemetryLayout.OffMagic);
        uint existingTotal = *(uint*)(_base + TelemetryLayout.OffTotalBytes);
        if (existingMagic == TelemetryLayout.Magic && *(uint*)(_base + TelemetryLayout.OffHeaderBytes) == TelemetryLayout.HeaderBytes
            && existingTotal <= bytes && existingTotal >= TelemetryLayout.HeaderBytes)
        {
            _total = (int)existingTotal;
        }
        while (capacity >= TelemetryLayout.MinCapacity && TelemetryLayout.RequiredBytes(capacity) > _total) capacity -= 64;
        if (!TelemetryLayout.IsValidCapacity(capacity)) return TelemetryOpenResult.TooSmall;
        _capacity = capacity;
        _stage.Clear();
        _committed = false;
        return TelemetryOpenResult.Ok;
    }

    /// <summary>Declares a scalar from a generated table (properties copied from the table).</summary>
    public int Declare(ChannelDef def, Provenance provenance = Provenance.Unspecified)
    {
        if (def.ArrayCapacity != 0) return DeclareArray(def, 0, provenance);
        return AddDesc(def.Name, def.Unit, def.RangeMin, def.RangeMax, def.Kind, def.Flags, 0, 0, def.Layout, provenance);
    }

    /// <summary>
    /// Declares the first <paramref name="count"/> elements of an array table entry (0 = the table capacity); the
    /// descriptors carry <paramref name="count"/> as their arrayCapacity. Returns the slot of element 0.
    /// </summary>
    public int DeclareArray(ChannelDef def, int count = 0, Provenance provenance = Provenance.Unspecified)
    {
        if (def.ArrayCapacity == 0) return Declare(def, provenance);
        if (count == 0) count = def.ArrayCapacity;
        if (count < 0 || count > def.ArrayCapacity) return InvalidSlot;
        if (_stage.Count + count > _capacity) return InvalidSlot;
        int first = InvalidSlot;
        for (int i = 0; i < count; i++)
        {
            int s = AddDesc(def.Name + "[" + i + "]", def.Unit, def.RangeMin, def.RangeMax, def.Kind, def.Flags, i, count, def.Layout, provenance);
            if (s == InvalidSlot) return InvalidSlot;
            if (first == InvalidSlot) first = s;
        }
        return first;
    }

    /// <summary>A producer-specific channel; the consumer publishes it as <c>sim.&lt;simId&gt;.&lt;name&gt;</c>.</summary>
    public int DeclareLocal(string name, string unit, float rangeMin, float rangeMax, SignalKind kind,
                            ChannelFlags flags = ChannelFlags.None, Provenance provenance = Provenance.Unspecified)
    {
        if (string.IsNullOrEmpty(name) || name.Length > ChannelModel.NameMax) return InvalidSlot;
        if (!ChannelModel.IsValidChannelName("sim.x." + name)) return InvalidSlot;
        return AddDesc(name, unit, rangeMin, rangeMax, kind, flags, 0, 0, ArrayLayout.Index, provenance);
    }

    /// <summary>
    /// Wheels per axle, front to back (car <c>{2,2}</c>, motorcycle <c>{1,1}</c>); empty = unknown. Entries are 1..8,
    /// at most 8 axles; an invalid list is stored as unknown. After <see cref="CommitLayout"/> this is a layout rewrite.
    /// </summary>
    public void SetWheelLayout(params byte[] wheelsPerAxle)
    {
        Array.Clear(_wheelsPerAxle, 0, _wheelsPerAxle.Length);
        if (wheelsPerAxle.Length <= TelemetryLayout.MaxAxles && TelemetryLayout.IsValidWheelsPerAxle(wheelsPerAxle))
        {
            wheelsPerAxle.CopyTo(_wheelsPerAxle, 0);
        }
        if (_committed) CommitLayout();
    }

    /// <summary>Session identity; after <see cref="CommitLayout"/> this is a layout rewrite published immediately.</summary>
    public void SetCar(string carId, string carName)
    {
        _carId = Truncate(carId, 63);
        _carName = Truncate(carName, 31);
        if (_committed) CommitLayout();
    }

    /// <summary>Publishes the layout fields and descriptor table under the layout seqlock; clears frame data.</summary>
    public bool CommitLayout()
    {
        if (_base == null) return false;
        int cap = _capacity;
        int descOffset = TelemetryLayout.HeaderBytes;
        int validOffset = descOffset + cap * TelemetryLayout.DescBytes;
        int eventOffset = validOffset + (cap / 64) * 8;
        int valuesOffset = eventOffset + cap * 4;

        uint s = Volatile.Read(ref U32(TelemetryLayout.OffLayoutSeq));
        uint begin = s + 1 + (s & 1u);                       // odd, and past a leftover odd value
        Volatile.Write(ref U32(TelemetryLayout.OffLayoutSeq), begin);

        ulong fs = Volatile.Read(ref U64(TelemetryLayout.OffFrameSeq));
        if ((fs & 1u) != 0) fs++;

        bool fresh = U32(TelemetryLayout.OffMagic) != TelemetryLayout.Magic;
        if (fresh)
        {
            new Span<byte>(_base, _total).Clear();
            U16(TelemetryLayout.OffVersionMajor) = 1;
            U16(TelemetryLayout.OffVersionMinor) = 0;
            U32(TelemetryLayout.OffHeaderBytes) = TelemetryLayout.HeaderBytes;
            U32(TelemetryLayout.OffTotalBytes) = (uint)_total;
            U32(TelemetryLayout.OffLayoutSeq) = begin;
        }
        U32(TelemetryLayout.OffCapacity) = (uint)cap;
        U32(TelemetryLayout.OffChannelCount) = (uint)_stage.Count;
        U32(TelemetryLayout.OffDescOffset) = (uint)descOffset;
        U32(TelemetryLayout.OffValidOffset) = (uint)validOffset;
        U32(TelemetryLayout.OffEventCountOffset) = (uint)eventOffset;
        U32(TelemetryLayout.OffValuesOffset) = (uint)valuesOffset;
        U64(TelemetryLayout.OffQpcFrequency) = _identity.QpcFrequency != 0 ? _identity.QpcFrequency : TelemetryClock.Frequency;
        U32(TelemetryLayout.OffProducerPid) = _identity.Pid != 0 ? _identity.Pid : (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
        SetString(TelemetryLayout.OffSimId, 24, _identity.SimId ?? "");
        SetString(TelemetryLayout.OffSimName, 32, _identity.SimName ?? "");
        SetString(TelemetryLayout.OffCarId, 64, _carId);
        SetString(TelemetryLayout.OffCarName, 32, _carName);
        _wheelsPerAxle.CopyTo(new Span<byte>(_base + TelemetryLayout.OffWheelsPerAxle, TelemetryLayout.MaxAxles));

        new Span<byte>(_base + descOffset, cap * TelemetryLayout.DescBytes).Clear();
        for (int i = 0; i < _stage.Count; i++)
        {
            _stage[i].AsSpan().CopyTo(new Span<byte>(_base + descOffset + i * TelemetryLayout.DescBytes, TelemetryLayout.DescBytes));
        }
        new Span<byte>(_base + validOffset, (cap / 64) * 8).Clear();
        new Span<byte>(_base + eventOffset, cap * 4).Clear();
        new Span<byte>(_base + valuesOffset, cap * 4).Clear();

        Volatile.Write(ref U64(TelemetryLayout.OffFrameSeq), fs);
        Volatile.Write(ref U64(TelemetryLayout.OffFrameSeqEnd), fs);
        if (fresh) Volatile.Write(ref U32(TelemetryLayout.OffMagic), TelemetryLayout.Magic);
        Volatile.Write(ref U32(TelemetryLayout.OffLayoutSeq), begin + 1);
        _committed = true;
        return true;
    }

    /// <summary>Starts a frame (frameSeq odd) and clears the valid mask.</summary>
    public void BeginFrame()
    {
        if (!_committed) return;
        Volatile.Write(ref U64(TelemetryLayout.OffFrameSeq), Volatile.Read(ref U64(TelemetryLayout.OffFrameSeq)) + 1);
        new Span<byte>(_base + U32(TelemetryLayout.OffValidOffset), (_capacity / 64) * 8).Clear();
    }

    /// <summary>Writes a value and sets its valid bit.</summary>
    public void Set(int slot, float value)
    {
        if (!_committed || slot < 0 || slot >= _stage.Count) return;
        *(float*)(_base + U32(TelemetryLayout.OffValuesOffset) + slot * 4) = value;
        *(ulong*)(_base + U32(TelemetryLayout.OffValidOffset) + (slot >> 6) * 8) |= 1UL << (slot & 63);
    }

    /// <summary>One event with <paramref name="amplitude"/>.</summary>
    public void Event(int slot, float amplitude)
    {
        if (!_committed || slot < 0 || slot >= _stage.Count) return;
        (*(uint*)(_base + U32(TelemetryLayout.OffEventCountOffset) + slot * 4))++;
        Set(slot, amplitude);
    }

    /// <summary>Ends the frame. <paramref name="frameQpc"/>: sample time; <paramref name="nowQpc"/>: publish time.</summary>
    public void EndFrame(ulong frameQpc, ulong nowQpc)
    {
        if (!_committed) return;
        U64(TelemetryLayout.OffFrameQpc) = frameQpc;
        ulong odd = Volatile.Read(ref U64(TelemetryLayout.OffFrameSeq));
        Volatile.Write(ref U64(TelemetryLayout.OffFrameSeqEnd), odd + 1);
        Volatile.Write(ref U64(TelemetryLayout.OffFrameSeq), odd + 1);
        Volatile.Write(ref U64(TelemetryLayout.OffLastPublishQpc), nowQpc);
    }

    /// <summary>Sets the producer state and refreshes the liveness timestamp.</summary>
    public void SetState(ProducerState state, ulong nowQpc)
    {
        Volatile.Write(ref U64(TelemetryLayout.OffLastPublishQpc), nowQpc);
        Volatile.Write(ref U32(TelemetryLayout.OffProducerState), (uint)state);
    }

    private ref uint U32(int offset) => ref *(uint*)(_base + offset);

    private ref ulong U64(int offset) => ref *(ulong*)(_base + offset);

    private ref ushort U16(int offset) => ref *(ushort*)(_base + offset);

    private void SetString(int offset, int length, string value)
    {
        var dst = new Span<byte>(_base + offset, length);
        dst.Clear();
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        int n = Math.Min(bytes.Length, length - 1);
        bytes.AsSpan(0, n).CopyTo(dst);
    }

    private static string Truncate(string s, int maxBytes)
    {
        byte[] b = Encoding.UTF8.GetBytes(s ?? "");
        return b.Length <= maxBytes ? s ?? "" : Encoding.UTF8.GetString(b, 0, maxBytes);
    }

    private int AddDesc(string name, string unit, float rangeMin, float rangeMax, SignalKind kind, ChannelFlags flags,
                        int arrayIndex, int arrayCapacity, ArrayLayout layout = ArrayLayout.Index,
                        Provenance provenance = Provenance.Unspecified)
    {
        byte[] nameBytes = Encoding.UTF8.GetBytes(name);
        byte[] unitBytes = Encoding.UTF8.GetBytes(unit ?? "");
        if (_stage.Count >= _capacity || nameBytes.Length > ChannelModel.NameMax || unitBytes.Length >= 16) return InvalidSlot;
        var d = new byte[TelemetryLayout.DescBytes];
        nameBytes.CopyTo(d, 0);
        unitBytes.CopyTo(d, 64);
        BitConverter.GetBytes(rangeMin).CopyTo(d, 80);
        BitConverter.GetBytes(rangeMax).CopyTo(d, 84);
        BitConverter.GetBytes((uint)kind).CopyTo(d, 88);
        BitConverter.GetBytes((uint)flags).CopyTo(d, 92);
        BitConverter.GetBytes((ushort)arrayIndex).CopyTo(d, 96);
        BitConverter.GetBytes((ushort)arrayCapacity).CopyTo(d, 98);
        d[100] = (byte)provenance;
        d[101] = (byte)layout;
        _stage.Add(d);
        return _stage.Count - 1;
    }
}

/// <summary>The return region as a producer sees it.</summary>
public sealed class TelemetryReturnView
{
    public uint Capacity { get; internal set; }
    public uint ForLayoutSeq { get; internal set; }
    public ConsumerState ConsumerState { get; internal set; }
    public ulong LastConsumeQpc { get; internal set; }
    internal ulong[] Accepted { get; set; } = Array.Empty<ulong>();
    internal ulong[] Demanded { get; set; } = Array.Empty<ulong>();

    /// <summary>True when the consumer bound this slot.</summary>
    public bool IsAccepted(int slot) =>
        slot >= 0 && (slot >> 6) < Accepted.Length && ((Accepted[slot >> 6] >> (slot & 63)) & 1UL) != 0;

    /// <summary>True when something consumes this slot; while the consumer is not Active every accepted slot counts.</summary>
    public bool IsDemanded(int slot)
    {
        if (ConsumerState != ConsumerState.Active) return IsAccepted(slot);
        return slot >= 0 && (slot >> 6) < Demanded.Length && ((Demanded[slot >> 6] >> (slot & 63)) & 1UL) != 0;
    }
}

/// <summary>Owns the named inbound section (Windows) and the <see cref="TelemetryProducer"/> bound to it.</summary>
#if NET5_0_OR_GREATER
[SupportedOSPlatform("windows")]
#endif
public sealed unsafe class TelemetrySection : IDisposable
{
    private readonly MemoryMappedFile _inbound;
    private readonly MemoryMappedViewAccessor _view;
    private readonly string _returnName;
    private MemoryMappedFile? _return;
    private MemoryMappedViewAccessor? _returnView;
    private byte* _ptr;

    private TelemetrySection(MemoryMappedFile file, MemoryMappedViewAccessor view, byte* ptr, string returnName, TelemetryProducer producer)
    {
        _inbound = file;
        _view = view;
        _ptr = ptr;
        _returnName = returnName;
        Producer = producer;
    }

    /// <summary>The producer bound to the section.</summary>
    public TelemetryProducer Producer { get; }

    /// <summary>Inbound section name: <c>IRTACTILE_TELEMETRY_SECTION</c>, else from <c>IRTACTILE_TELEMETRY_SOURCE</c>, else <paramref name="defaultSourceId"/>.</summary>
    public static string NameFromEnvironment(string defaultSourceId)
    {
        string? section = Environment.GetEnvironmentVariable("IRTACTILE_TELEMETRY_SECTION");
        if (!string.IsNullOrEmpty(section)) return section!;
        string? source = Environment.GetEnvironmentVariable("IRTACTILE_TELEMETRY_SOURCE");
        return TelemetryLayout.InboundName(string.IsNullOrEmpty(source) ? defaultSourceId : source!);
    }

    /// <summary>Creates (or restarts into) the named section. Windows only.</summary>
    public static TelemetrySection Create(string inboundName, int capacity, ProducerIdentity identity)
    {
        long bytes = TelemetryLayout.RequiredBytes(capacity);
        var file = MemoryMappedFile.CreateOrOpen(inboundName, bytes, MemoryMappedFileAccess.ReadWrite);
        var view = file.CreateViewAccessor(0, 0, MemoryMappedFileAccess.ReadWrite);
        byte* ptr = null;
        view.SafeMemoryMappedViewHandle.AcquirePointer(ref ptr);
        var producer = new TelemetryProducer();
        var result = producer.Open(ptr, (long)view.SafeMemoryMappedViewHandle.ByteLength, capacity, identity);
        if (result != TelemetryOpenResult.Ok)
        {
            view.SafeMemoryMappedViewHandle.ReleasePointer();
            view.Dispose();
            file.Dispose();
            throw new InvalidOperationException("telemetry-shm: " + result);
        }
        return new TelemetrySection(file, view, ptr, TelemetryLayout.ReturnNameFor(inboundName), producer);
    }

    /// <summary>Reads the return region (opened lazily; absence is normal). False = nothing to report.</summary>
    public bool PollReturn(TelemetryReturnView into)
    {
        if (_returnView == null)
        {
            try
            {
                _return = MemoryMappedFile.OpenExisting(_returnName, MemoryMappedFileRights.Read);
                _returnView = _return.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
            }
            catch (FileNotFoundException)
            {
                return false;
            }
        }
        byte* p = null;
        _returnView.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
        try
        {
            if (*(uint*)p != TelemetryLayout.ReturnMagic || *(ushort*)(p + 4) != 1) return false;
            for (int attempt = 0; attempt < 4; attempt++)
            {
                ulong r1 = Volatile.Read(ref *(ulong*)(p + TelemetryLayout.ROffRetSeq));
                if ((r1 & 1) != 0) continue;
                uint cap = *(uint*)(p + TelemetryLayout.ROffCapacity);
                if (!TelemetryLayout.IsValidCapacity((int)cap)) return false;
                int words = (int)cap / 64;
                uint accOff = *(uint*)(p + TelemetryLayout.ROffAccepted);
                uint demOff = *(uint*)(p + TelemetryLayout.ROffDemanded);
                if (accOff + words * 8 > TelemetryLayout.ReturnBytes || demOff + words * 8 > TelemetryLayout.ReturnBytes) return false;
                var acc = new ulong[words];
                var dem = new ulong[words];
                for (int i = 0; i < words; i++)
                {
                    acc[i] = *(ulong*)(p + accOff + i * 8);
                    dem[i] = *(ulong*)(p + demOff + i * 8);
                }
                if (Volatile.Read(ref *(ulong*)(p + TelemetryLayout.ROffRetSeqEnd)) != r1 ||
                    Volatile.Read(ref *(ulong*)(p + TelemetryLayout.ROffRetSeq)) != r1) continue;
                into.Capacity = cap;
                into.ForLayoutSeq = *(uint*)(p + TelemetryLayout.ROffForLayoutSeq);
                uint cs = Volatile.Read(ref *(uint*)(p + TelemetryLayout.ROffConsumerState));
                into.ConsumerState = cs <= 2 ? (ConsumerState)cs : ConsumerState.Closed;
                into.LastConsumeQpc = Volatile.Read(ref *(ulong*)(p + TelemetryLayout.ROffLastConsume));
                into.Accepted = acc;
                into.Demanded = dem;
                return true;
            }
            return false;
        }
        finally
        {
            _returnView.SafeMemoryMappedViewHandle.ReleasePointer();
        }
    }

    /// <inheritdoc />
    public void Dispose()
    {
        if (_ptr != null)
        {
            _view.SafeMemoryMappedViewHandle.ReleasePointer();
            _ptr = null;
        }
        _returnView?.Dispose();
        _return?.Dispose();
        _view.Dispose();
        _inbound.Dispose();
    }
}
