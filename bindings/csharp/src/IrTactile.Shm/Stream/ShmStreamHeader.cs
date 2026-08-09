// IrTactile.Shm.Stream - ShmStreamHeader / ShmBlockDesc, the wire structs.
//
// SPDX-License-Identifier: MIT
//
// The byte layout is normative (spec/stream-shm/v1/fields.md); every
// FieldOffset below is pinned by tests against both the spec table and
// Marshal.OffsetOf. Little-endian, IEEE-754 float32, two's complement int16,
// natural alignment, no packing.
//
// Concurrency classes, as contiguous byte ranges of the header:
//
//   Bytes [0, 80) and [112, 256)
//                   - pinned + layout fields, protected by the layoutSeq
//                     seqlock. Read them under the seqlock and discard any
//                     snapshot taken before the most recent even layoutSeq.
//   Bytes [80, 112) - the only concurrently-written fields. The seqlock does
//                     not cover them: read each with its own acquire load
//                     (Volatile.Read), at any time - even mid-rewrite, while
//                     layoutSeq is odd. Never take their values from a header
//                     snapshot.

using System.Runtime.InteropServices;
using System.Text;

namespace IrTactile.Shm.Stream;

/// <summary>
/// The 256-byte section header, as a blittable snapshot. <see cref="Reader"/>
/// copies this out of the section under the layoutSeq seqlock; the concurrent
/// range (bytes [80, 112)) inside a snapshot is stale by definition and is
/// never read from it.
/// </summary>
[StructLayout(LayoutKind.Explicit, Size = StreamLayout.HeaderBytes)]
public unsafe struct ShmStreamHeader
{
    // --- pinned + layout fields; seqlock-protected (* = pinned) ---
    /// <summary>*pinned; kMagic; 0 ⇒ header not yet written.</summary>
    [FieldOffset(0)] public uint Magic;
    /// <summary>*pinned; reader must reject a mismatch.</summary>
    [FieldOffset(4)] public ushort VersionMajor;
    /// <summary>Reader must tolerate a higher value.</summary>
    [FieldOffset(6)] public ushort VersionMinor;
    /// <summary>*pinned; size of THIS header; never copy more.</summary>
    [FieldOffset(8)] public uint HeaderBytes;
    /// <summary>*pinned; size of the whole section.</summary>
    [FieldOffset(12)] public uint TotalBytes;

    /// <summary><see cref="Stream.SampleFormat"/>; reject an unknown value.</summary>
    [FieldOffset(16)] public uint SampleFormat;
    /// <summary><see cref="Stream.ChannelLayout"/>; reject an unknown value.</summary>
    [FieldOffset(20)] public uint ChannelLayout;
    /// <summary>1..32.</summary>
    [FieldOffset(24)] public uint ChannelCount;
    /// <summary>Operative rate ×1000; exact at every rate.</summary>
    [FieldOffset(28)] public uint SampleRateMilliHz;
    /// <summary>Provenance: rate before decimation (Hz).</summary>
    [FieldOffset(32)] public uint SourceSampleRate;
    /// <summary>Provenance: decimation ratio, 1..1024.</summary>
    [FieldOffset(36)] public uint RateDivisor;
    /// <summary>Samples per channel in one block.</summary>
    [FieldOffset(40)] public uint SamplesPerBlock;
    /// <summary>Ring depth, in slots, 2..256.</summary>
    [FieldOffset(44)] public uint RingBlocks;
    /// <summary>4 (Float32) or 2 (Int16).</summary>
    [FieldOffset(48)] public uint BytesPerSample;

    // --- layout fields; seqlock-protected (addressing). Read, never recompute ---
    /// <summary>Slot base → plane 0.</summary>
    [FieldOffset(52)] public uint SlotPayloadOffset;
    /// <summary>0 in v1: no channel table.</summary>
    [FieldOffset(56)] public uint ChannelArrayOffset;
    /// <summary>*pinned; section base → slot[0].</summary>
    [FieldOffset(60)] public uint SlotArrayOffset;
    /// <summary>Slot to slot; multiple of 64.</summary>
    [FieldOffset(64)] public uint SlotBytes;
    /// <summary>Plane to plane inside a slot.</summary>
    [FieldOffset(68)] public uint ChannelPlaneBytes;

    // --- layout fields; seqlock-protected (diagnostics). Never derive behaviour ---
    /// <summary>irTactile DSP block: 32/64/128.</summary>
    [FieldOffset(72)] public uint DiagEngineBlockSize;
    /// <summary>irTactile frames batched per publish.</summary>
    [FieldOffset(76)] public uint DiagFramesPerPublish;

    // --- concurrent fields; read directly from the section with Volatile.Read ---
    /// <summary>Blocks published [acq/rel].</summary>
    [FieldOffset(80)] public ulong WriteSeq;
    /// <summary>QPC at the last publish [acq/rel].</summary>
    [FieldOffset(88)] public ulong LastPublishQpc;
    /// <summary><see cref="Stream.DeviceState"/> [acq/rel].</summary>
    [FieldOffset(96)] public uint DeviceState;
    /// <summary>Odd ⇒ header being rewritten [acq/rel]; a change invalidates every cached snapshot of a layout field.</summary>
    [FieldOffset(100)] public uint LayoutSeq;
    /// <summary>Bit c ⇒ channel c has a source [acq/rel].</summary>
    [FieldOffset(104)] public uint WiredChannelMask;
    /// <summary>Producer zeroes; reader ignores.</summary>
    [FieldOffset(108)] public uint Reserved1;

    // --- layout fields again; seqlock-protected ---
    /// <summary>Ticks/s, for both QPC fields.</summary>
    [FieldOffset(112)] public ulong QpcFrequency;
    /// <summary>UTF-8, NUL-terminated.</summary>
    [FieldOffset(120)] public fixed byte DeviceGuid[40];
    /// <summary>UTF-8, NUL-terminated.</summary>
    [FieldOffset(160)] public fixed byte DeviceName[64];
    /// <summary>
    /// Producer's OS process id; 0 ⇒ unknown. Diagnostics and zombie
    /// adjudication only - PIDs are reused, so never act on it without a
    /// staleness check.
    /// </summary>
    [FieldOffset(224)] public uint ProducerPid;
    /// <summary>→ 256 B. Producer zeroes; reader IGNORES.</summary>
    [FieldOffset(228)] public fixed byte Reserved[28];

    /// <summary>The NUL-terminated UTF-8 device GUID, from the snapshot.</summary>
    public string GetDeviceGuid()
    {
        fixed (byte* p = DeviceGuid) return ReadCString(p, 40);
    }

    /// <summary>The NUL-terminated UTF-8 device name, from the snapshot.</summary>
    public string GetDeviceName()
    {
        fixed (byte* p = DeviceName) return ReadCString(p, 64);
    }

    private static string ReadCString(byte* p, int max)
    {
        int len = 0;
        while (len < max && p[len] != 0) ++len;
        return Encoding.UTF8.GetString(p, len);
    }
}

/// <summary>
/// The 64-byte block descriptor at the base of every slot. Never snapshot it:
/// <c>seq</c> and <c>layoutEpoch</c> are acquire/release fields read live, and
/// the remaining fields are read only between the two seq brackets.
/// </summary>
[StructLayout(LayoutKind.Explicit, Size = StreamLayout.BlockDescBytes)]
public struct ShmBlockDesc
{
    /// <summary>Block index, or kSlotBusy while being filled; slot = seq % ringBlocks [acq/rel].</summary>
    [FieldOffset(0)] public ulong Seq;
    /// <summary>Stream-rate samples emitted before this block, INCLUDING any later discarded.</summary>
    [FieldOffset(8)] public ulong FirstSampleIndex;
    /// <summary>QPC at publish.</summary>
    [FieldOffset(16)] public ulong TimestampQpc;
    /// <summary><see cref="Stream.BlockFlags"/>.</summary>
    [FieldOffset(24)] public uint Flags;
    /// <summary>Bit c set ⇒ channel c is all-zero here.</summary>
    [FieldOffset(28)] public uint SilentChannelMask;
    /// <summary>LayoutSeq snapshot at publish; reader must reject a mismatch.</summary>
    [FieldOffset(32)] public uint LayoutEpoch;
    // [36, 64): reserved. Producer zeroes; reader IGNORES.
}
