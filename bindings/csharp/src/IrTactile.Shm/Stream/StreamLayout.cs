// IrTactile.Shm.Stream - constants, offsets, size arithmetic, addressing.
//
// SPDX-License-Identifier: MIT
//
// The managed twin of bindings/cpp/include/irtactile/shm/stream/layout.h,
// written against spec/stream-shm/v1/fields.md. Every offset written out
// literally, so a layout change fails the offset-pinning tests here.

namespace IrTactile.Shm.Stream;

/// <summary>
/// The wire-format constants and the normative arithmetic. Consumers read the
/// header fields and never re-derive them; the size arithmetic is for
/// producers and tests.
/// </summary>
public static class StreamLayout
{
    // =========================================================================
    // Constants
    // =========================================================================

    /// <summary>'ITSA', the segment magic.</summary>
    public const uint Magic = 0x41535449u;

    /// <summary>Layout break.</summary>
    public const ushort VersionMajor = 1;

    /// <summary>Additive only.</summary>
    public const ushort VersionMinor = 0;

    /// <summary>
    /// Stored in the descriptor's seq while the producer is filling the slot.
    /// A reader that sees any value other than the requested seq must discard
    /// the slot and resynchronise.
    /// </summary>
    public const ulong SlotBusy = ulong.MaxValue;

    /// <summary>sizeof(ShmStreamHeader), 64-byte aligned.</summary>
    public const int HeaderBytes = 256;

    /// <summary>sizeof(ShmBlockDesc), 64-byte aligned.</summary>
    public const int BlockDescBytes = 64;

    // =========================================================================
    // Field offsets - pinned by the ABI tests against spec/stream-shm/v1/fields.md
    // =========================================================================

    /// <summary>Byte offsets into <see cref="ShmStreamHeader"/>.</summary>
    public static class HeaderOffset
    {
#pragma warning disable CS1591
        public const int Magic = 0;
        public const int VersionMajor = 4;
        public const int VersionMinor = 6;
        public const int HeaderBytes = 8;
        public const int TotalBytes = 12;
        public const int SampleFormat = 16;
        public const int ChannelLayout = 20;
        public const int ChannelCount = 24;
        public const int SampleRateMilliHz = 28;
        public const int SourceSampleRate = 32;
        public const int RateDivisor = 36;
        public const int SamplesPerBlock = 40;
        public const int RingBlocks = 44;
        public const int BytesPerSample = 48;
        public const int SlotPayloadOffset = 52;
        public const int ChannelArrayOffset = 56;
        public const int SlotArrayOffset = 60;
        public const int SlotBytes = 64;
        public const int ChannelPlaneBytes = 68;
        public const int DiagEngineBlockSize = 72;
        public const int DiagFramesPerPublish = 76;
        public const int WriteSeq = 80;
        public const int LastPublishQpc = 88;
        public const int DeviceState = 96;
        public const int LayoutSeq = 100;
        public const int WiredChannelMask = 104;
        public const int Reserved1 = 108;
        public const int QpcFrequency = 112;
        public const int DeviceGuid = 120;
        public const int DeviceName = 160;
        public const int ProducerPid = 224;
        public const int Reserved = 228;
#pragma warning restore CS1591
    }

    /// <summary>Byte offsets into <see cref="ShmBlockDesc"/>.</summary>
    public static class DescOffset
    {
#pragma warning disable CS1591
        public const int Seq = 0;
        public const int FirstSampleIndex = 8;
        public const int TimestampQpc = 16;
        public const int Flags = 24;
        public const int SilentChannelMask = 28;
        public const int LayoutEpoch = 32;
        public const int Reserved = 36;
#pragma warning restore CS1591
    }

    // =========================================================================
    // Size arithmetic - for the producer and tests. Consumers read the header
    // fields and never re-derive them.
    // =========================================================================

    /// <summary>Round up to a multiple of 64.</summary>
    public static uint Align64(uint v) => (v + 63u) & ~63u;

    /// <summary>Bytes per sample for a format.</summary>
    public static uint BytesPerSampleOf(SampleFormat f) => f == SampleFormat.Int16 ? 2u : 4u;

    /// <summary>One channel plane inside a slot, in bytes.</summary>
    public static uint ChannelPlaneBytesOf(in LayoutParams p) =>
        p.SamplesPerBlock * BytesPerSampleOf(p.Format);

    /// <summary>Slot base → plane 0: the descriptor's own size.</summary>
    public static uint SlotPayloadOffsetOf() => BlockDescBytes;

    /// <summary>Slot stride, padded to a multiple of 64 so slots never share a cache line.</summary>
    public static uint SlotBytesOf(in LayoutParams p) =>
        Align64(SlotPayloadOffsetOf() + p.ChannelCount * ChannelPlaneBytesOf(p));

    /// <summary>Segment base → slot[0].</summary>
    public static uint SlotArrayOffsetOf() => Align64(HeaderBytes);

    /// <summary>
    /// Total segment footprint. 64-bit so an out-of-range config is detectable
    /// rather than wrapped.
    /// </summary>
    public static ulong ComputeTotalSize(in LayoutParams p) =>
        SlotArrayOffsetOf() + (ulong)p.RingBlocks * SlotBytesOf(p);

    // =========================================================================
    // Derived quantities - for consumers as well as producers
    // =========================================================================

    /// <summary>Publish periods a wait covers.</summary>
    public const double WaitTimeoutPeriods = 4.0;

    /// <summary>Publish periods that count as stalled.</summary>
    public const double StaleAfterPeriods = 5.0;

    /// <summary>Publish period, from samplesPerBlock and sampleRateMilliHz alone.</summary>
    public static double PeriodSecondsOf(in ShmStreamHeader h) =>
        h.SampleRateMilliHz == 0
            ? 0.0
            : (double)h.SamplesPerBlock * 1000.0 / h.SampleRateMilliHz;

    /// <summary>The publish period in milliseconds.</summary>
    public static double PeriodMsOf(in ShmStreamHeader h) =>
        h.SampleRateMilliHz == 0
            ? 0.0
            : (double)h.SamplesPerBlock * 1000.0 * 1000.0 / h.SampleRateMilliHz;

    /// <summary>Blocks published per second.</summary>
    public static double PublishHzOf(in ShmStreamHeader h) =>
        h.SamplesPerBlock == 0
            ? 0.0
            : h.SampleRateMilliHz / 1000.0 / (double)h.SamplesPerBlock;

    /// <summary>How much history the ring holds.</summary>
    public static double RingSecondsOf(in ShmStreamHeader h) =>
        h.RingBlocks * PeriodSecondsOf(h);

    /// <summary>The library's wait timeout: max(50 ms, 4 × period).</summary>
    public static double WaitTimeoutMsOf(in ShmStreamHeader h)
    {
        double p = WaitTimeoutPeriods * PeriodMsOf(h);
        return p > 50.0 ? p : 50.0;
    }

    /// <summary>The staleness threshold: max(100 ms, 5 × period).</summary>
    public static double StaleAfterMsOf(in ShmStreamHeader h)
    {
        double p = StaleAfterPeriods * PeriodMsOf(h);
        return p > 100.0 ? p : 100.0;
    }

    // =========================================================================
    // Size classes
    // =========================================================================

    private static readonly long[] SizeClassBytes = { 256L * 1024, 512L * 1024, 1024L * 1024 };
    private static readonly string[] SizeClassSuffix = { "_256k", "_512k", "_1m" };

    /// <summary>The byte envelope of a size class.</summary>
    public static long BytesOf(SizeClass c) => SizeClassBytes[(int)c];

    /// <summary>The name suffix of a size class ("_256k", "_512k", "_1m").</summary>
    public static string SuffixOf(SizeClass c) => SizeClassSuffix[(int)c];

    /// <summary>
    /// The config spelling of a size class ("256k", "512k", "1m"). False on
    /// anything else, including null.
    /// </summary>
    public static bool SizeClassFromString(string? s, out SizeClass c)
    {
        switch (s)
        {
            case "256k": c = SizeClass.S256K; return true;
            case "512k": c = SizeClass.S512K; return true;
            case "1m": c = SizeClass.S1M; return true;
            default: c = default; return false;
        }
    }

    /// <summary>
    /// The smallest class whose envelope holds <paramref name="p"/>, sized
    /// against Float32 so an int16→float32 switch keeps the same segment name.
    /// Returns S1M when nothing fits; the caller must still range-check
    /// <see cref="ComputeTotalSize"/> against <see cref="MaxTotalBytes"/>.
    /// </summary>
    public static SizeClass SmallestSizeClassFor(in LayoutParams p)
    {
        var envelope = new LayoutParams(p.ChannelCount, p.SamplesPerBlock, p.RingBlocks,
                                        SampleFormat.Float32);
        ulong need = ComputeTotalSize(envelope);
        if (need <= (ulong)SizeClassBytes[0]) return SizeClass.S256K;
        if (need <= (ulong)SizeClassBytes[1]) return SizeClass.S512K;
        return SizeClass.S1M;
    }

    // =========================================================================
    // The rate ladder - normative
    // =========================================================================

    /// <summary>
    /// The 11 valid rates. 48000 / divisor is exact at every rate, which is why
    /// sampleRateMilliHz carries no floating point.
    /// </summary>
    public static readonly uint[] RateDivisors = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 };

    /// <summary>
    /// The rate matching <paramref name="hz"/> within 0.001 Hz - well under
    /// the smallest gap on the ladder (~47 Hz, between divisors 512 and 1024).
    /// False if off-ladder.
    /// </summary>
    public static bool RateDivisorFromHz(double hz, out uint divisor)
    {
        foreach (uint d in RateDivisors)
        {
            double ladderHz = 48000.0 / d;
            double diff = hz > ladderHz ? hz - ladderHz : ladderHz - hz;
            if (diff < 0.001) { divisor = d; return true; }
        }
        divisor = 0;
        return false;
    }

    // =========================================================================
    // Limits - normative
    // =========================================================================

#pragma warning disable CS1591
    public const uint MinChannels = 1;
    public const uint MaxChannels = 32;      // a uint32 mask covers this exactly
    public const uint MinRingBlocks = 2;     // 1 offers no protection at all
    public const uint MaxRingBlocks = 256;
    public const uint MaxRateDivisor = 1024;
    public const long MaxTotalBytes = 1024L * 1024;   // largest size class
    public const double WarnRingSeconds = 1.0;        // warning, not an error
#pragma warning restore CS1591

    // =========================================================================
    // Addressing - the normative arithmetic
    //
    //     slot_index   = seq % ringBlocks
    //     slot_base    = base + slotArrayOffset + slot_index × slotBytes
    //     descriptor   = slot_base
    //     plane(ch)    = slot_base + slotPayloadOffset + ch × channelPlaneBytes
    //     sample(ch,i) = plane(ch) + i × bytesPerSample     // i < samplesPerBlock
    //
    // All values are byte offsets from base.
    // =========================================================================

    /// <summary>Segment-relative byte offset of the slot holding <paramref name="seq"/>.</summary>
    public static long SlotOffsetOf(in ShmStreamHeader h, ulong seq) =>
        h.SlotArrayOffset + (long)(seq % h.RingBlocks) * h.SlotBytes;

    /// <summary>Slot-relative byte offset of channel <paramref name="ch"/>'s plane.</summary>
    public static long PlaneOffsetInSlot(in ShmStreamHeader h, uint ch) =>
        h.SlotPayloadOffset + (long)ch * h.ChannelPlaneBytes;

    // =========================================================================
    // int16 conversion - clamping, not wrapping
    //
    // The 32767 convention: ±1.0 maps to ±32767, so the round trip is exact for
    // every value the producer emits. The asymmetric consequence is that a raw
    // -32768 decodes to -1.00003, marginally outside [-1, 1]; a consumer that
    // needs a hard bound clamps after conversion.
    // =========================================================================

    /// <summary>Float → int16, clamped to [-32768, 32767], never wrapped.</summary>
    public static short FloatToInt16(float v)
    {
        float s = v * 32767.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        return (short)(s < 0.0f ? s - 0.5f : s + 0.5f);
    }

    /// <summary>int16 → float via the ±32767 convention.</summary>
    public static float Int16ToFloat(short v) => v * (1.0f / 32767.0f);
}

/// <summary>Everything the layout is a function of.</summary>
public readonly struct LayoutParams
{
    /// <summary>1..32.</summary>
    public uint ChannelCount { get; }

    /// <summary>Samples per channel in one block.</summary>
    public uint SamplesPerBlock { get; }

    /// <summary>Ring depth, in slots, 2..256.</summary>
    public uint RingBlocks { get; }

    /// <summary>Payload encoding.</summary>
    public SampleFormat Format { get; }

    /// <summary>All four parameters.</summary>
    public LayoutParams(uint channelCount, uint samplesPerBlock, uint ringBlocks,
                        SampleFormat format = SampleFormat.Float32)
    {
        ChannelCount = channelCount;
        SamplesPerBlock = samplesPerBlock;
        RingBlocks = ringBlocks;
        Format = format;
    }
}
