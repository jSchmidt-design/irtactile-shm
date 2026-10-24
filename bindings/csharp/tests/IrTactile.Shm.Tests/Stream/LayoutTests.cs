// Size arithmetic, ABI pinning, rate ladder, size classes, int16 conversion.
//
// SPDX-License-Identifier: MIT

using System.Runtime.InteropServices;
using IrTactile.Shm.Stream;
using static IrTactile.Shm.Stream.StreamLayout;

namespace IrTactile.Shm.Tests.Stream;

public unsafe class LayoutTests
{
    // =========================================================================
    // Size arithmetic
    // =========================================================================

    [Theory]
    [InlineData(1u, 1u, 2u, SampleFormat.Float32)]
    [InlineData(8u, 48u, 2u, SampleFormat.Float32)]
    [InlineData(32u, 1u, 256u, SampleFormat.Int16)]
    [InlineData(8u, 768u, 2u, SampleFormat.Float32)]
    [InlineData(20u, 128u, 64u, SampleFormat.Int16)]
    public void ComputeTotalSize_MatchesManualLayout(uint ch, uint spb, uint ring, SampleFormat fmt)
    {
        var p = new LayoutParams(ch, spb, ring, fmt);
        uint bps = BytesPerSampleOf(fmt);
        uint cpb = spb * bps;
        uint slotB = Align64(BlockDescBytes + ch * cpb);
        ulong want = Align64(HeaderBytes) + (ulong)ring * slotB;

        Assert.Equal(want, ComputeTotalSize(p));
        Assert.Equal(cpb, ChannelPlaneBytesOf(p));
        Assert.Equal(slotB, SlotBytesOf(p));
        // slotBytes must have room for the descriptor and every plane.
        Assert.True(SlotPayloadOffsetOf() + ch * cpb <= slotB);
    }

    [Fact]
    public void ChannelPlaneBytes_EqualsSamplesPerBlockTimesBytesPerSample()
    {
        var p = new LayoutParams(8, 48, 2, SampleFormat.Int16);
        Assert.Equal(48u * 2u, ChannelPlaneBytesOf(p));
    }

    // =========================================================================
    // ABI stability - the offset-pinning test: spec/stream-shm/v1/fields.md
    // transcribed against
    // Marshal.OffsetOf / Marshal.SizeOf. A wrong offset in this binding fails
    // here, loudly, instead of producing plausible-looking float garbage.
    // =========================================================================

    [Fact]
    public void Abi_StructSizesArePinned()
    {
        Assert.Equal(256, Marshal.SizeOf<ShmStreamHeader>());
        Assert.Equal(64, Marshal.SizeOf<ShmBlockDesc>());
        Assert.Equal(256, HeaderBytes);
        Assert.Equal(64, BlockDescBytes);

        // Marshal.SizeOf describes the MARSHALLED layout. The reader never
        // marshals: it snapshots with Buffer.MemoryCopy into a stack
        // ShmStreamHeader and addresses the segment through raw pointers, so
        // the managed layout is what has to be 256/64. They coincide for a
        // blittable explicit-layout struct - pinned here so they cannot quietly
        // stop coinciding.
        Assert.Equal(256, sizeof(ShmStreamHeader));
        Assert.Equal(64, sizeof(ShmBlockDesc));
    }

    [Fact]
    public void Abi_HeaderOffsetsMatchTheSpecTable()
    {
        static void Pin(int specOffset, string field)
        {
            Assert.Equal(specOffset, (int)Marshal.OffsetOf<ShmStreamHeader>(field));
        }

        Pin(HeaderOffset.Magic, nameof(ShmStreamHeader.Magic));
        Pin(HeaderOffset.VersionMajor, nameof(ShmStreamHeader.VersionMajor));
        Pin(HeaderOffset.VersionMinor, nameof(ShmStreamHeader.VersionMinor));
        Pin(HeaderOffset.HeaderBytes, nameof(ShmStreamHeader.HeaderBytes));
        Pin(HeaderOffset.TotalBytes, nameof(ShmStreamHeader.TotalBytes));
        Pin(HeaderOffset.SampleFormat, nameof(ShmStreamHeader.SampleFormat));
        Pin(HeaderOffset.ChannelLayout, nameof(ShmStreamHeader.ChannelLayout));
        Pin(HeaderOffset.ChannelCount, nameof(ShmStreamHeader.ChannelCount));
        Pin(HeaderOffset.SampleRateMilliHz, nameof(ShmStreamHeader.SampleRateMilliHz));
        Pin(HeaderOffset.SourceSampleRate, nameof(ShmStreamHeader.SourceSampleRate));
        Pin(HeaderOffset.RateDivisor, nameof(ShmStreamHeader.RateDivisor));
        Pin(HeaderOffset.SamplesPerBlock, nameof(ShmStreamHeader.SamplesPerBlock));
        Pin(HeaderOffset.RingBlocks, nameof(ShmStreamHeader.RingBlocks));
        Pin(HeaderOffset.BytesPerSample, nameof(ShmStreamHeader.BytesPerSample));
        Pin(HeaderOffset.SlotPayloadOffset, nameof(ShmStreamHeader.SlotPayloadOffset));
        Pin(HeaderOffset.ChannelArrayOffset, nameof(ShmStreamHeader.ChannelArrayOffset));
        Pin(HeaderOffset.SlotArrayOffset, nameof(ShmStreamHeader.SlotArrayOffset));
        Pin(HeaderOffset.SlotBytes, nameof(ShmStreamHeader.SlotBytes));
        Pin(HeaderOffset.ChannelPlaneBytes, nameof(ShmStreamHeader.ChannelPlaneBytes));
        Pin(HeaderOffset.DiagEngineBlockSize, nameof(ShmStreamHeader.DiagEngineBlockSize));
        Pin(HeaderOffset.DiagFramesPerPublish, nameof(ShmStreamHeader.DiagFramesPerPublish));
        Pin(HeaderOffset.WriteSeq, nameof(ShmStreamHeader.WriteSeq));
        Pin(HeaderOffset.LastPublishQpc, nameof(ShmStreamHeader.LastPublishQpc));
        Pin(HeaderOffset.DeviceState, nameof(ShmStreamHeader.DeviceState));
        Pin(HeaderOffset.LayoutSeq, nameof(ShmStreamHeader.LayoutSeq));
        Pin(HeaderOffset.WiredChannelMask, nameof(ShmStreamHeader.WiredChannelMask));
        Pin(HeaderOffset.Reserved1, nameof(ShmStreamHeader.Reserved1));
        Pin(HeaderOffset.QpcFrequency, nameof(ShmStreamHeader.QpcFrequency));
        Pin(HeaderOffset.DeviceGuid, nameof(ShmStreamHeader.DeviceGuid));
        Pin(HeaderOffset.DeviceName, nameof(ShmStreamHeader.DeviceName));
        Pin(HeaderOffset.ProducerPid, nameof(ShmStreamHeader.ProducerPid));
        Pin(HeaderOffset.Reserved, nameof(ShmStreamHeader.Reserved));
    }

    [Fact]
    public void Abi_DescriptorOffsetsMatchTheSpecTable()
    {
        static void Pin(int specOffset, string field)
        {
            Assert.Equal(specOffset, (int)Marshal.OffsetOf<ShmBlockDesc>(field));
        }

        Pin(DescOffset.Seq, nameof(ShmBlockDesc.Seq));
        Pin(DescOffset.FirstSampleIndex, nameof(ShmBlockDesc.FirstSampleIndex));
        Pin(DescOffset.TimestampQpc, nameof(ShmBlockDesc.TimestampQpc));
        Pin(DescOffset.Flags, nameof(ShmBlockDesc.Flags));
        Pin(DescOffset.SilentChannelMask, nameof(ShmBlockDesc.SilentChannelMask));
        Pin(DescOffset.LayoutEpoch, nameof(ShmBlockDesc.LayoutEpoch));
    }

    [Fact]
    public void Abi_HotRangeOffsetsArePinnedLiterally()
    {
        // The spec table's values, written out literally: this is what a
        // binding's hardcoded numbers are checked against.
        Assert.Equal(80, HeaderOffset.WriteSeq);
        Assert.Equal(88, HeaderOffset.LastPublishQpc);
        Assert.Equal(96, HeaderOffset.DeviceState);
        Assert.Equal(100, HeaderOffset.LayoutSeq);
        Assert.Equal(104, HeaderOffset.WiredChannelMask);
        Assert.Equal(112, HeaderOffset.QpcFrequency);
        Assert.Equal(52, HeaderOffset.SlotPayloadOffset);
        Assert.Equal(224, HeaderOffset.ProducerPid);
        Assert.Equal(228, HeaderOffset.Reserved);

        // 64-bit fields naturally aligned - the precondition for a tear-free read.
        Assert.Equal(0, HeaderOffset.WriteSeq % 8);
        Assert.Equal(0, HeaderOffset.LastPublishQpc % 8);
        Assert.Equal(0, HeaderOffset.QpcFrequency % 8);
    }

    [Fact]
    public void NoChannelTableInV1()
    {
        using var prod = new TestProducer(DefaultParams());
        Assert.Equal(0u, SegmentMemory.U32(prod.Raw, HeaderOffset.ChannelArrayOffset));
        Assert.Equal(Align64(HeaderBytes), SegmentMemory.U32(prod.Raw, HeaderOffset.SlotArrayOffset));
    }

    internal static LayoutParams DefaultParams() =>
        new(channelCount: 4, samplesPerBlock: 8, ringBlocks: 2, SampleFormat.Float32);

    // =========================================================================
    // Size classes
    // =========================================================================

    [Fact]
    public void SmallestSizeClassFor_PicksSmallestFittingEnvelope()
    {
        Assert.Equal(SizeClass.S256K,
            SmallestSizeClassFor(new LayoutParams(2, 128, 2, SampleFormat.Float32)));
        Assert.Equal(SizeClass.S512K,
            SmallestSizeClassFor(new LayoutParams(16, 128, 32, SampleFormat.Float32)));
        Assert.Equal(SizeClass.S1M,
            SmallestSizeClassFor(new LayoutParams(32, 512, 8, SampleFormat.Float32)));
    }

    [Fact]
    public void SizeClass_IsSizedAsFloat32EvenForInt16()
    {
        var i16 = new LayoutParams(16, 128, 32, SampleFormat.Int16);
        var f32 = new LayoutParams(16, 128, 32, SampleFormat.Float32);
        Assert.True(ComputeTotalSize(i16) < ComputeTotalSize(f32));
        Assert.Equal(SmallestSizeClassFor(f32), SmallestSizeClassFor(i16));
    }

    [Fact]
    public void SizeClass_EverySuffixParsesToItsClass()
    {
        Assert.True(SizeClassFromString("256k", out var c)); Assert.Equal(SizeClass.S256K, c);
        Assert.True(SizeClassFromString("512k", out c)); Assert.Equal(SizeClass.S512K, c);
        Assert.True(SizeClassFromString("1m", out c)); Assert.Equal(SizeClass.S1M, c);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("256K")]   // case-sensitive
    [InlineData("256k ")]
    [InlineData("2m")]
    [InlineData("1")]
    public void SizeClassFromString_RejectsAnythingElse(string? s)
    {
        Assert.False(SizeClassFromString(s, out _));
    }

    [Fact]
    public void SizeClass_SuffixesAreDistinct()
    {
        Assert.NotEqual(SuffixOf(SizeClass.S256K), SuffixOf(SizeClass.S512K));
        Assert.NotEqual(SuffixOf(SizeClass.S512K), SuffixOf(SizeClass.S1M));
        Assert.NotEqual(SuffixOf(SizeClass.S256K), SuffixOf(SizeClass.S1M));
    }

    [Fact]
    public void ReferenceProfile_ResolvesToTheDocumentedClass()
    {
        // The "ship it" device profile and the toy producers: 8 ch @ 3000 Hz
        // (divisor 16), framesPerPublish 6 → 48 samples/block, ring 2, float32.
        Assert.True(RateDivisorFromHz(3000.0, out uint divisor));
        Assert.Equal(16u, divisor);

        uint samplesPerBlock = 6u * 128u / divisor;
        Assert.Equal(48u, samplesPerBlock);

        var p = new LayoutParams(8, samplesPerBlock, 2, SampleFormat.Float32);
        Assert.Equal(192u, ChannelPlaneBytesOf(p));
        Assert.Equal(1600u, SlotBytesOf(p));
        Assert.Equal(3456ul, ComputeTotalSize(p));   // the figure quoted in the spec

        Assert.Equal(SizeClass.S256K, SmallestSizeClassFor(p));
        Assert.Equal("_256k", SuffixOf(SmallestSizeClassFor(p)));
    }

    // =========================================================================
    // The rate ladder
    // =========================================================================

    [Theory]
    [InlineData(48000.0, 1u)]
    [InlineData(24000.0, 2u)]
    [InlineData(12000.0, 4u)]
    [InlineData(6000.0, 8u)]
    [InlineData(3000.0, 16u)]
    [InlineData(1500.0, 32u)]
    [InlineData(750.0, 64u)]
    [InlineData(375.0, 128u)]
    [InlineData(187.5, 256u)]
    [InlineData(93.75, 512u)]
    [InlineData(46.875, 1024u)]
    public void RateDivisorFromHz_CoversExactlyThe11Rungs(double hz, uint divisor)
    {
        Assert.True(RateDivisorFromHz(hz, out uint d));
        Assert.Equal(divisor, d);
        // 48000 / divisor is exact on every rung - the whole reason
        // sampleRateMilliHz carries no floating point downstream.
        Assert.Equal(0u, 48_000_000u % d);
    }

    [Theory]
    [InlineData(44100.0)]
    [InlineData(0.0)]
    [InlineData(96000.0)]
    [InlineData(-48000.0)]
    public void RateDivisorFromHz_RejectsOffLadderRates(double hz)
    {
        Assert.False(RateDivisorFromHz(hz, out _));
    }

    // =========================================================================
    // Derived quantities
    // =========================================================================

    [Theory]
    [InlineData(1u, 128u, 375.0)]
    [InlineData(2u, 256u, 187.5)]
    [InlineData(4u, 512u, 93.75)]
    [InlineData(8u, 1024u, 46.875)]
    public void DerivedQuantities_AreExactOnHandBuiltRungs(uint fpp, uint spb, double publishHz)
    {
        // The engine publishes once per 128-sample frame at 375 Hz. Batching
        // framesPerPublish frames gives samplesPerBlock = 128*fpp and
        // publishHz = 375 / fpp exactly.
        Assert.Equal(128u * fpp, spb);
        var p = new LayoutParams(4, spb, 2, SampleFormat.Float32);
        var h = new ShmStreamHeader
        {
            SampleRateMilliHz = 48_000_000u,
            SamplesPerBlock = spb,
            RingBlocks = 2,
        };

        double periodSec = spb / 48000.0;
        double periodMs = periodSec * 1000.0;

        Assert.Equal(periodSec, PeriodSecondsOf(h), 12);
        Assert.Equal(periodMs, PeriodMsOf(h), 9);
        Assert.Equal(publishHz, PublishHzOf(h), 12);
        Assert.Equal(periodSec * p.RingBlocks, RingSecondsOf(h), 12);

        double wantWait = 4.0 * periodMs > 50.0 ? 4.0 * periodMs : 50.0;
        double wantStale = 5.0 * periodMs > 100.0 ? 5.0 * periodMs : 100.0;
        Assert.Equal(wantWait, WaitTimeoutMsOf(h), 9);
        Assert.Equal(wantStale, StaleAfterMsOf(h), 9);
    }

    [Fact]
    public void PeriodDerivation_DependsOnlyOnSamplesPerBlockAndSampleRate()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        // 8 samples / 3000 Hz = 2.6666... ms
        Assert.Equal(8.0 / 3.0, r.PeriodMs, 9);
        Assert.Equal(50.0, r.WaitTimeoutMs, 9);    // 4x period < 50 ms floor
        Assert.Equal(100.0, r.StaleAfterMs, 9);    // 5x period < 100 ms floor
    }

    // =========================================================================
    // int16 conversion
    // =========================================================================

    [Fact]
    public void Int16_RoundTripIsWithin1Lsb_AndClampsRatherThanWraps()
    {
        Assert.Equal(0.5f, Int16ToFloat(FloatToInt16(0.5f)), 3);
        Assert.Equal(-0.5f, Int16ToFloat(FloatToInt16(-0.5f)), 3);

        // Beyond range: clamp, not wrap. A wrap would turn a loud transient
        // into full-scale noise of the opposite sign.
        Assert.Equal(32767, FloatToInt16(2.0f));
        Assert.Equal(-32768, FloatToInt16(-2.0f));
        Assert.True(FloatToInt16(1.0f) >= 32000);
    }
}
