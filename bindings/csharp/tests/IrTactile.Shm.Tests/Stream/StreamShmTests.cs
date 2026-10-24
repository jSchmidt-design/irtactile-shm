// Format tests for IrTactile.Shm.Stream (stream-shm v1): attach validation,
// forward compatibility, ring overrun, slot recycling, the seqlock, liveness.
//
// SPDX-License-Identifier: MIT
//
// A case-by-case port of bindings/cpp/tests/stream/StreamShm_test.cpp. Both
// suites run over an owned buffer - the publish protocol and the reader's
// forward-compatibility rules are pure data transformations.

using IrTactile.Shm.Stream;
using static IrTactile.Shm.Stream.StreamLayout;

namespace IrTactile.Shm.Tests.Stream;

public unsafe class StreamShmTests
{
    private static LayoutParams DefaultParams() =>
        new(channelCount: 4, samplesPerBlock: 8, ringBlocks: 2, SampleFormat.Float32);

    // =========================================================================
    // Attach
    // =========================================================================

    [Fact]
    public void Attach_ZeroFilledSegment_IsNotReadyAndRetryable()
    {
        using var zeros = new PinnedSegment(HeaderBytes + 4096);
        var r = new Reader();
        AttachResult ar = r.Attach(zeros.Ptr, zeros.Length);
        Assert.Equal(AttachResult.NotReady, ar);
        Assert.True(ar.ShouldRetry());
    }

    [Fact]
    public void Attach_WrongMagic_IsMalformedAndFatal()
    {
        using var s = new PinnedSegment(HeaderBytes + 4096);
        SegmentMemory.SetU32(s.Raw, HeaderOffset.Magic, 0xDEADBEEFu);
        var r = new Reader();
        AttachResult ar = r.Attach(s.Ptr, s.Length);
        Assert.Equal(AttachResult.Malformed, ar);
        Assert.False(ar.ShouldRetry());
    }

    [Fact]
    public void Attach_HigherVersionMajor_IsBadVersionAndNotRetryable()
    {
        using var prod = new TestProducer(DefaultParams());
        SegmentMemory.SetU16(prod.Raw, HeaderOffset.VersionMajor, (ushort)(VersionMajor + 1));
        var r = new Reader();
        AttachResult ar = r.Attach(prod.BasePtr, prod.Size);
        Assert.Equal(AttachResult.BadVersion, ar);
        Assert.False(ar.ShouldRetry());
    }

    [Fact]
    public void Attach_HigherVersionMinor_IsTolerated()
    {
        using var prod = new TestProducer(DefaultParams());
        SegmentMemory.SetU16(prod.Raw, HeaderOffset.VersionMinor, (ushort)(VersionMinor + 5));
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
    }

    [Fact]
    public void Attach_UnknownSampleFormat_IsRejectedNotDefaulted()
    {
        using var prod = new TestProducer(DefaultParams());
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.SampleFormat, 99u);
        var r = new Reader();
        Assert.Equal(AttachResult.BadVersion, r.Attach(prod.BasePtr, prod.Size));
    }

    [Fact]
    public void Attach_UnknownChannelLayout_IsRejectedNotDefaulted()
    {
        using var prod = new TestProducer(DefaultParams());
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.ChannelLayout, 99u);
        var r = new Reader();
        Assert.Equal(AttachResult.BadVersion, r.Attach(prod.BasePtr, prod.Size));
    }

    [Fact]
    public void Attach_OffsetPastMappedBytes_IsMalformed()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        // Map fewer bytes than the segment claims.
        Assert.Equal(AttachResult.Malformed, r.Attach(prod.BasePtr, prod.Size - 1));
    }

    [Theory]
    [InlineData("channelPlaneBytes")]
    [InlineData("slotPayloadOffset")]
    [InlineData("slotBytes")]
    [InlineData("slotArrayOffset")]
    public void Attach_PlaneMisalignedLayout_IsMalformed(string term)
    {
        // The plane-alignment guarantee ("planes are bytesPerSample-aligned")
        // is validated, not assumed: every term of the plane address must be a
        // multiple of bytesPerSample (4 here). Each subcase breaks exactly one
        // term while keeping every size relation satisfiable, which needs a
        // padded buffer - TestProducer's is the exact live footprint.
        using var prod = new TestProducer(DefaultParams());   // CPB 32, slotBytes 192, bps 4
        byte[] copy = new byte[prod.Size + 4096];
        System.Runtime.InteropServices.Marshal.Copy(prod.BasePtr, copy, 0, (int)prod.Size);
        using var s = new PinnedSegment(copy);
        SegmentMemory.SetU32(s.Raw, HeaderOffset.TotalBytes, (uint)s.Length);

        switch (term)
        {
            case "channelPlaneBytes":
                SegmentMemory.SetU32(s.Raw, HeaderOffset.ChannelPlaneBytes, 34u);
                SegmentMemory.SetU32(s.Raw, HeaderOffset.SlotBytes, 256u);
                break;
            case "slotPayloadOffset":
                SegmentMemory.SetU32(s.Raw, HeaderOffset.SlotPayloadOffset, 66u);
                SegmentMemory.SetU32(s.Raw, HeaderOffset.SlotBytes, 256u);
                break;
            case "slotBytes":
                SegmentMemory.SetU32(s.Raw, HeaderOffset.SlotBytes, 194u);
                break;
            case "slotArrayOffset":
                SegmentMemory.SetU32(s.Raw, HeaderOffset.SlotArrayOffset, 258u);
                break;
        }

        var r = new Reader();
        Assert.Equal(AttachResult.Malformed, r.Attach(s.Ptr, s.Length));
    }

    [Fact]
    public void Attach_NeverCopiesMoreThanHeaderBytes()
    {
        // A hypothetical v1.1 producer whose header grew by 64 bytes:
        // headerBytes and slotArrayOffset both move out together, leaving a
        // gap a v1.0 Reader knows nothing about.
        var p = new LayoutParams(2, 4, 2, SampleFormat.Float32);
        uint grownHeaderBytes = HeaderBytes + 64u;
        uint grownSlotArrayOff = Align64(grownHeaderBytes);
        uint cpb = ChannelPlaneBytesOf(p);
        uint slotB = SlotBytesOf(p);
        ulong total = grownSlotArrayOff + (ulong)p.RingBlocks * slotB;

        using var s = new PinnedSegment((long)total);
        byte* b = s.Raw;
        SegmentMemory.SetU16(b, HeaderOffset.VersionMajor, VersionMajor);
        SegmentMemory.SetU16(b, HeaderOffset.VersionMinor, VersionMinor);
        SegmentMemory.SetU32(b, HeaderOffset.HeaderBytes, grownHeaderBytes);   // > 256
        SegmentMemory.SetU32(b, HeaderOffset.TotalBytes, (uint)total);
        SegmentMemory.SetU32(b, HeaderOffset.SampleFormat, (uint)p.Format);
        SegmentMemory.SetU32(b, HeaderOffset.ChannelLayout, (uint)ChannelLayout.Planar);
        SegmentMemory.SetU32(b, HeaderOffset.ChannelCount, p.ChannelCount);
        SegmentMemory.SetU32(b, HeaderOffset.SampleRateMilliHz, 3_000_000u);
        SegmentMemory.SetU32(b, HeaderOffset.SourceSampleRate, 48000u);
        SegmentMemory.SetU32(b, HeaderOffset.RateDivisor, 16u);
        SegmentMemory.SetU32(b, HeaderOffset.SamplesPerBlock, p.SamplesPerBlock);
        SegmentMemory.SetU32(b, HeaderOffset.RingBlocks, p.RingBlocks);
        SegmentMemory.SetU32(b, HeaderOffset.BytesPerSample, BytesPerSampleOf(p.Format));
        SegmentMemory.SetU32(b, HeaderOffset.SlotPayloadOffset, SlotPayloadOffsetOf());
        SegmentMemory.SetU32(b, HeaderOffset.ChannelArrayOffset, 0u);
        SegmentMemory.SetU32(b, HeaderOffset.SlotArrayOffset, grownSlotArrayOff);
        SegmentMemory.SetU32(b, HeaderOffset.SlotBytes, slotB);
        SegmentMemory.SetU32(b, HeaderOffset.ChannelPlaneBytes, cpb);
        SegmentMemory.SetU64(b, HeaderOffset.QpcFrequency, 10_000_000ul);
        SegmentMemory.SetU32(b, HeaderOffset.DeviceState, (uint)DeviceState.Active);
        SegmentMemory.Release32(b, HeaderOffset.Magic, Magic);

        // Attach must clamp its snapshot copy to min(headerBytes, 256) - never
        // headerBytes, which would read past the reader's own snapshot buffer.
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(s.Ptr, s.Length));
    }

    [Fact]
    public void Attach_NonZeroReservedBytes_AreTolerated()
    {
        using var prod = new TestProducer(DefaultParams());
        for (int i = 0; i < 28; ++i)
            prod.Raw[HeaderOffset.Reserved + i] = 0xAB;
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
    }

    [Fact]
    public void Attach_CursorStartsAtWriteSeq_NotZero()
    {
        using var prod = new TestProducer(DefaultParams());
        for (int i = 0; i < 10; ++i) prod.Publish();
        // "Producer has been running for hours": jump writeSeq far ahead
        // without publishing that many blocks - what matters is that attach
        // reads it rather than assuming 0.
        SegmentMemory.Release64(prod.Raw, HeaderOffset.WriteSeq, 10_000_000ul);

        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        var b = new BlockView();
        Assert.False(r.Next(b));            // no gap reported on the first read
        Assert.Equal(0ul, r.DroppedBlocks);
    }

    // =========================================================================
    // StreamRequest / Mismatch
    // =========================================================================

    [Fact]
    public void StreamRequest_MatchingFieldsAttachOk_AZeroFieldIsNotChecked()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size,
            new StreamRequest { ChannelCount = 4, SampleRateMilliHz = 3_000_000,
                                SamplesPerBlock = 8, SampleFormat = SampleFormat.Float32 }));

        var r2 = new Reader();
        Assert.Equal(AttachResult.Ok, r2.Attach(prod.BasePtr, prod.Size,
            new StreamRequest { ChannelCount = 4 }));
    }

    [Fact]
    public void StreamRequest_ChannelCountMismatch_NamesTheFieldAndDoesNotRetry()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        AttachResult ar = r.Attach(prod.BasePtr, prod.Size,
            new StreamRequest { ChannelCount = 6 });
        Assert.Equal(AttachResult.Mismatch, ar);
        Assert.False(ar.ShouldRetry());
        Assert.Contains("channelCount", r.MismatchDetail);
        Assert.Contains('4', r.MismatchDetail);
        Assert.Contains('6', r.MismatchDetail);
    }

    [Fact]
    public void StreamRequest_EachFieldIsCheckedIndependently()
    {
        using var prod = new TestProducer(DefaultParams());

        var r1 = new Reader();
        Assert.Equal(AttachResult.Mismatch, r1.Attach(prod.BasePtr, prod.Size,
            new StreamRequest { SampleRateMilliHz = 375_000 }));
        Assert.Contains("sampleRateMilliHz", r1.MismatchDetail);

        var r2 = new Reader();
        Assert.Equal(AttachResult.Mismatch, r2.Attach(prod.BasePtr, prod.Size,
            new StreamRequest { SamplesPerBlock = 128 }));
        Assert.Contains("samplesPerBlock", r2.MismatchDetail);

        var r3 = new Reader();
        Assert.Equal(AttachResult.Mismatch, r3.Attach(prod.BasePtr, prod.Size,
            new StreamRequest { SampleFormat = SampleFormat.Int16 }));
        Assert.Contains("sampleFormat", r3.MismatchDetail);
    }

    // =========================================================================
    // Publish / consume round-trip
    // =========================================================================

    [Fact]
    public void Attach_SamplesPerBlockOverrunningThePlane_IsRejected()
    {
        // The copy bound is header.samplesPerBlock, so THIS is the check that
        // stands between a corrupt segment and a read past the mapping.
        using var prod = new TestProducer(new LayoutParams(2, 4, 2, SampleFormat.Float32));
        // channelPlaneBytes still says 4 samples; the header now claims 4096.
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.SamplesPerBlock, 4096u);

        var r = new Reader();
        Assert.Equal(AttachResult.Malformed, r.Attach(prod.BasePtr, prod.Size));
        Assert.False(r.Attached);
    }

    [Fact]
    public void RefreshSnapshot_RewriteOverrunningThePlane_DetachesTheReader()
    {
        // Same bound, reached the other way: the segment validated at attach
        // and was then rewritten with a hostile samplesPerBlock under a
        // layoutSeq bump. The reader must fail validation and detach.
        using var prod = new TestProducer(new LayoutParams(2, 4, 2, SampleFormat.Float32));
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();

        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq,
            SegmentMemory.U32(prod.Raw, HeaderOffset.LayoutSeq) + 2u);
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.SamplesPerBlock, 4096u);

        var b = new BlockView();
        Assert.False(r.Next(b));
        Assert.False(r.Attached);
    }

    [Theory]
    [InlineData(HeaderOffset.SampleFormat, 3u)]                        // a v1.x format this build does not know
    [InlineData(HeaderOffset.ChannelLayout, (uint)ChannelLayout.Interleaved)]
    public void RefreshSnapshot_RewriteToAnUnknownEnum_DetachesTheReader(int offset, uint value)
    {
        // "Reject unknown enum values" is a per-snapshot rule, not an
        // attach-time one: a producer restart may rewrite either enum. Falling
        // through to a default is the silent-misread this rule exists to stop -
        // BytesPerSampleOf() answers 4 for any unknown format, so an unchecked
        // sampleFormat=3 would keep decoding as Float32, and Interleaved would
        // keep decoding as Planar. Both must detach instead.
        using var prod = new TestProducer(new LayoutParams(2, 4, 2, SampleFormat.Float32));
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();

        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq,
            SegmentMemory.U32(prod.Raw, HeaderOffset.LayoutSeq) + 2u);
        SegmentMemory.SetU32(prod.Raw, offset, value);

        var b = new BlockView();
        Assert.False(r.Next(b));
        Assert.False(r.Attached);
    }

    [Fact]
    public void RoundTrip_NBlocksInNOut_SampleExact_SeqContiguous()
    {
        using var prod = new TestProducer(new LayoutParams(4, 8, 8, SampleFormat.Float32));
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));

        for (int i = 0; i < 5; ++i) prod.Publish();

        var b = new BlockView();
        ulong expectedSeq = 0;
        int count = 0;
        while (r.Next(b))
        {
            Assert.Equal(expectedSeq, b.Seq);
            Assert.Equal(8, b.SampleCount);
            for (int ch = 0; ch < 4; ++ch)
            {
                ReadOnlySpan<float> s = b.Channel(ch);
                for (int i = 0; i < b.SampleCount; ++i)
                {
                    float want = b.FirstSampleIndex + (ulong)i + (ulong)ch * 1000.0f;
                    Assert.Equal(want, s[i], 5);
                }
            }
            ++expectedSeq;
            ++count;
        }
        Assert.Equal(5, count);
        Assert.Equal(0ul, r.DroppedBlocks);
    }

    [Fact]
    public void Overrun_RingPlus10WithoutDraining_IsDetectedAndResynchronised()
    {
        using var prod = new TestProducer(new LayoutParams(2, 4, 4, SampleFormat.Float32)); // ring 4
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));

        for (int i = 0; i < 14; ++i) prod.Publish();   // ringBlocks(4) + 10

        var b = new BlockView();
        int count = 0;
        while (r.Next(b)) ++count;
        // Readable window is ringBlocks - 1.
        Assert.Equal(3, count);
        Assert.True(r.DroppedBlocks > 0);
    }

    [Fact]
    public void OverrunBoundaryRing2_OneBlockPastCursor_Delivers()
    {
        using var prod = new TestProducer(DefaultParams());   // ring 2
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();
        var b = new BlockView();
        Assert.True(r.Next(b));
        Assert.Equal(0ul, b.Seq);
        Assert.False(r.Next(b));
    }

    [Fact]
    public void OverrunBoundaryRing2_TwoBlocksPastCursor_ReportsAGap()
    {
        using var prod = new TestProducer(DefaultParams());   // ring 2
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();
        prod.Publish();
        var b = new BlockView();
        // Readable window is 1 (ringBlocks-1): the writer's in-progress slot
        // is never handed over, so exactly the newest complete block survives.
        int count = 0;
        while (r.Next(b)) ++count;
        Assert.Equal(1, count);
        Assert.Equal(1ul, r.DroppedBlocks);
    }

    [Fact]
    public void SlotRecyclingMidConstruction_BusySlotIsRejectedOutright()
    {
        using var prod = new TestProducer(DefaultParams());   // ring 2
        prod.Publish();               // seq 0 in slot 0
        prod.Publish();               // seq 1 in slot 1

        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        // Attach cursor = writeSeq = 2, so nothing to read yet.
        var b = new BlockView();
        Assert.False(r.Next(b));

        // Producer claims slot 0 again (seq 2) but never finishes it.
        prod.BeginPartialPublish();
        // Force writeSeq forward without publishing seq 2's descriptor, to put
        // seq=2 inside the reader's requested range while the slot is kSlotBusy.
        SegmentMemory.Release64(prod.Raw, HeaderOffset.WriteSeq, 3ul);

        // The reader must reject the busy slot rather than return torn/garbage
        // data, and must not crash.
        Assert.False(r.Next(b));
        Assert.True(r.DroppedBlocks >= 1);
    }

    [Fact]
    public void AbandonedBlock_FirstSampleIndexAdvancesBySamplesDiscarded()
    {
        // Ring 4 (rather than the default 2) so both published blocks stay
        // readable at once - this test is about firstSampleIndex bookkeeping,
        // not the ring-2 overrun boundary covered elsewhere.
        using var prod = new TestProducer(new LayoutParams(4, 8, 4, SampleFormat.Float32));
        // Attach before anything is published - attach intentionally seeds the
        // cursor at the current writeSeq, so a reader that attached after the
        // fact would see neither block as "new".
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));

        prod.Publish();               // seq 0: firstSampleIndex 0..7

        // A partial block (4 of 8 samples) is abandoned mid-frame - the
        // samples already emitted into it still count as emitted.
        prod.AbandonPartialBlock(4);
        prod.Publish();               // seq 1: the next successful block

        var b0 = new BlockView();
        var b1 = new BlockView();
        Assert.True(r.Next(b0));
        Assert.Equal(0ul, b0.Seq);
        Assert.True(r.Next(b1));
        Assert.Equal(1ul, b1.Seq);

        // gap = firstSampleIndex - (prev.firstSampleIndex + prev.sampleCount)
        ulong gap = b1.FirstSampleIndex - (b0.FirstSampleIndex + (ulong)b0.SampleCount);
        Assert.Equal(4ul, gap);   // exactly the samples discarded, not zero
    }

    // =========================================================================
    // layoutSeq seqlock / reconfiguration
    // =========================================================================

    [Fact]
    public void LayoutSeqSeqlock_RewriteIsPickedUpOnTheNextRead()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();
        var b = new BlockView();
        Assert.True(r.Next(b));
        Assert.Equal(4u, r.ChannelCount);

        // A producer restart into a reused, differently-configured segment
        // bumps layoutSeq odd → rewrite → even.
        uint seq = SegmentMemory.U32(prod.Raw, HeaderOffset.LayoutSeq);
        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq, ++seq);   // odd
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.ChannelCount, 2u);
        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq, ++seq);   // even

        // The re-snapshot happens on the next read cycle, not merely by
        // polling an accessor - Next/Latest are what call the refresh.
        Assert.False(r.Next(b));            // no new block, but the header is re-read
        Assert.Equal(2u, r.ChannelCount);   // ...and the new layout is now visible
    }

    [Fact]
    public void LayoutSeqMoveDuringCopy_BracketsTheBlockAndCountsADrop()
    {
        // If the producer bumps layoutSeq after the descriptor is written but
        // before the reader finishes copying the slot, the post-copy layoutSeq
        // re-check must discard the block. A literal thread race cannot be
        // reproduced single-threaded, so we simulate the outcome: the snapshot
        // layoutSeq that entered CopyBlock is stale while the live header has
        // already moved on. The block must be discarded and counted as a drop.
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();

        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq,
            SegmentMemory.U32(prod.Raw, HeaderOffset.LayoutSeq) + 2u);

        // The block's own layoutEpoch is 0 and so is the injected snapshot
        // value, so the epoch check cannot be what rejects this: only the
        // post-copy layoutSeq bracket can.
        var b = new BlockView();
        Assert.False(r.TestCopyStepWithStaleSnapLayoutSeq(0, b));
        Assert.Equal(1ul, r.DroppedBlocks);
    }

    [Fact]
    public void ReconfigureKind_NoLayoutChange_ReportsNone()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        Assert.True(r.RefreshSnapshot());
        Assert.Equal(Reader.ReconfigureKind.None, r.LastReconfigureKind);
    }

    [Fact]
    public void ReconfigureKind_AddressingFieldChange_IsGeometry()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));

        uint seq = SegmentMemory.U32(prod.Raw, HeaderOffset.LayoutSeq);
        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq, ++seq);   // odd
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.ChannelCount, 2u);
        uint cpb = SegmentMemory.U32(prod.Raw, HeaderOffset.ChannelPlaneBytes);
        uint newSlotBytes = Align64(SlotPayloadOffsetOf() + 2u * cpb);
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.SlotBytes, newSlotBytes);
        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq, ++seq);   // even

        Assert.True(r.RefreshSnapshot());
        Assert.Equal(Reader.ReconfigureKind.Geometry, r.LastReconfigureKind);
        Assert.Equal(2u, r.ChannelCount);
        Assert.Equal(newSlotBytes, r.Header.SlotBytes);
    }

    [Fact]
    public void ReconfigureKind_DiagnosticOnlyChange_IsCosmetic()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));

        uint seq = SegmentMemory.U32(prod.Raw, HeaderOffset.LayoutSeq);
        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq, ++seq);   // odd
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.SourceSampleRate, 96000u);
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.RateDivisor, 8u);
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.DiagEngineBlockSize, 128u);
        SegmentMemory.SetU32(prod.Raw, HeaderOffset.DiagFramesPerPublish, 4u);
        SegmentMemory.Release32(prod.Raw, HeaderOffset.LayoutSeq, ++seq);   // even

        Assert.True(r.RefreshSnapshot());
        Assert.Equal(Reader.ReconfigureKind.Cosmetic, r.LastReconfigureKind);
        Assert.Equal(4u, r.ChannelCount);
        Assert.Equal(8u, r.SamplesPerBlock);
    }

    [Fact]
    public void LayoutEpochMismatch_RejectsABlock()
    {
        // A block's layoutEpoch must equal the layoutSeq that was current when
        // the payload was encoded. If the descriptor claims a different epoch,
        // the reader must discard the block instead of decoding potentially
        // stale-geometry payload.
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();

        uint slotArrayOffset = SegmentMemory.U32(prod.Raw, HeaderOffset.SlotArrayOffset);
        byte* slot = prod.Raw + slotArrayOffset;   // seq 0 → slot 0
        // Corrupt the epoch without changing the snapshot's layoutSeq or the
        // descriptor seq. Only the layoutEpoch check can catch this.
        SegmentMemory.Release32(slot, DescOffset.LayoutEpoch, 0xDEADBEEFu);

        var b = new BlockView();
        Assert.False(r.Next(b));
        Assert.Equal(1ul, r.DroppedBlocks);
    }

    [Fact]
    public void WiredChannelMask_IsObservableWithoutAResnapshot()
    {
        using var prod = new TestProducer(DefaultParams(), channelsWired: 0x3u);
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        Assert.Equal(0x3u, r.WiredChannelMask);

        // Changed live, with layoutSeq untouched - this field is deliberately
        // outside the seqlock, so no re-snapshot should be required.
        uint layoutBefore = r.LayoutSeq;
        SegmentMemory.Release32(prod.Raw, HeaderOffset.WiredChannelMask, 0x5u);
        Assert.Equal(0x5u, r.WiredChannelMask);
        Assert.Equal(layoutBefore, r.LayoutSeq);
    }

    // =========================================================================
    // Masks
    // =========================================================================

    [Fact]
    public void SilentChannelMask_FullSilenceSetsBlockSilence()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));   // before publishing
        prod.Publish(forceSilent: true);
        var b = new BlockView();
        Assert.True(r.Next(b));
        Assert.Equal(0xFu, b.SilentChannelMask);   // all 4 channels silent
        Assert.True((b.Flags & (uint)BlockFlags.Silence) != 0);
    }

    // =========================================================================
    // Liveness / thresholds
    // =========================================================================

    [Fact]
    public void Staleness_IsDecidedOverASyntheticClock()
    {
        using var prod = new TestProducer(DefaultParams());
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();
        var b = new BlockView();
        Assert.True(r.Next(b));

        ulong qpcFreq = r.Header.QpcFrequency;
        ulong lastQpc = r.LastPublishQpc;

        // Active + fresh → Live.
        Assert.Equal(StreamState.Live, r.State(lastQpc + 1));

        // Active + far in the future → Stalled.
        ulong staleTicks = (ulong)(r.StaleAfterMs / 1000.0 * qpcFreq) + qpcFreq;
        Assert.Equal(StreamState.Stalled, r.State(lastQpc + staleTicks));

        // Inactive, however stale → Quiet, not Stalled.
        SegmentMemory.Release32(prod.Raw, HeaderOffset.DeviceState, (uint)DeviceState.Inactive);
        Assert.Equal(StreamState.Quiet, r.State(lastQpc + staleTicks));

        // Closed → Closed regardless.
        SegmentMemory.Release32(prod.Raw, HeaderOffset.DeviceState, (uint)DeviceState.Closed);
        Assert.Equal(StreamState.Closed, r.State(lastQpc + staleTicks));
    }

    // =========================================================================
    // int16 decode
    // =========================================================================

    [Fact]
    public void Int16Segment_DecodesViaTheSameBlockViewApi()
    {
        using var prod = new TestProducer(new LayoutParams(2, 4, 2, SampleFormat.Int16));
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));
        prod.Publish();
        var b = new BlockView();
        Assert.True(r.Next(b));
        // Values were scaled down by 100000 before quantizing in TestProducer,
        // so just check the decode path runs and produces finite, ordered values.
        ReadOnlySpan<float> s = b.Channel(0);
        Assert.True(s[0] <= s[1]);
        Assert.True(s[1] <= s[2]);
    }

    // =========================================================================
    // Consumer never recomputes
    // =========================================================================

    [Fact]
    public void InflatedSlotPayloadOffsetAndSlotBytes_AreHonored()
    {
        // A hypothetical future producer whose descriptor grew and which pads
        // planes more generously than the v1 formula would. Built by hand so
        // the header's own fields - not any recomputation - are what the
        // reader must follow.
        var p = new LayoutParams(2, 4, 2, SampleFormat.Float32);

        uint inflatedPayloadOffset = BlockDescBytes + 64u;
        uint cpb = ChannelPlaneBytesOf(p);
        uint inflatedSlotBytes = Align64(inflatedPayloadOffset + p.ChannelCount * cpb);
        uint slotArrayOffset = SlotArrayOffsetOf();
        ulong totalBytes = slotArrayOffset + (ulong)p.RingBlocks * inflatedSlotBytes;

        using var s = new PinnedSegment((long)totalBytes);
        byte* b = s.Raw;
        SegmentMemory.SetU16(b, HeaderOffset.VersionMajor, VersionMajor);
        SegmentMemory.SetU16(b, HeaderOffset.VersionMinor, VersionMinor);
        SegmentMemory.SetU32(b, HeaderOffset.HeaderBytes, HeaderBytes);
        SegmentMemory.SetU32(b, HeaderOffset.TotalBytes, (uint)totalBytes);
        SegmentMemory.SetU32(b, HeaderOffset.SampleFormat, (uint)p.Format);
        SegmentMemory.SetU32(b, HeaderOffset.ChannelLayout, (uint)ChannelLayout.Planar);
        SegmentMemory.SetU32(b, HeaderOffset.ChannelCount, p.ChannelCount);
        SegmentMemory.SetU32(b, HeaderOffset.SampleRateMilliHz, 3_000_000u);
        SegmentMemory.SetU32(b, HeaderOffset.SourceSampleRate, 48000u);
        SegmentMemory.SetU32(b, HeaderOffset.RateDivisor, 16u);
        SegmentMemory.SetU32(b, HeaderOffset.SamplesPerBlock, p.SamplesPerBlock);
        SegmentMemory.SetU32(b, HeaderOffset.RingBlocks, p.RingBlocks);
        SegmentMemory.SetU32(b, HeaderOffset.BytesPerSample, BytesPerSampleOf(p.Format));
        SegmentMemory.SetU32(b, HeaderOffset.SlotPayloadOffset, inflatedPayloadOffset);
        SegmentMemory.SetU32(b, HeaderOffset.ChannelArrayOffset, 0u);
        SegmentMemory.SetU32(b, HeaderOffset.SlotArrayOffset, slotArrayOffset);
        SegmentMemory.SetU32(b, HeaderOffset.SlotBytes, inflatedSlotBytes);
        SegmentMemory.SetU32(b, HeaderOffset.ChannelPlaneBytes, cpb);
        SegmentMemory.SetU64(b, HeaderOffset.QpcFrequency, 10_000_000ul);
        SegmentMemory.SetU32(b, HeaderOffset.DeviceState, (uint)DeviceState.Active);

        // Write one block using exactly the arithmetic a real producer uses:
        // slot/plane off the header's own (inflated) fields, nothing hardcoded.
        byte* slot = b + slotArrayOffset;   // seq 0 → slot 0
        for (uint ch = 0; ch < p.ChannelCount; ++ch)
        {
            byte* plane = slot + inflatedPayloadOffset + (long)ch * cpb;
            for (uint i = 0; i < p.SamplesPerBlock; ++i)
                *(float*)(plane + i * 4) = i + ch * 1000.0f;
        }
        SegmentMemory.SetU64(slot, DescOffset.FirstSampleIndex, 0ul);
        SegmentMemory.SetU64(slot, DescOffset.TimestampQpc, 1000ul);
        SegmentMemory.SetU32(slot, DescOffset.Flags, 0u);
        SegmentMemory.SetU32(slot, DescOffset.SilentChannelMask, 0u);
        SegmentMemory.Release64(slot, DescOffset.Seq, 0ul);
        SegmentMemory.Release64(b, HeaderOffset.LastPublishQpc, 1000ul);
        SegmentMemory.Release32(b, HeaderOffset.Magic, Magic);   // writeSeq still 0: attach first

        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(s.Ptr, s.Length));

        // Publish is now "observed": bump writeSeq after attach, as a real
        // producer would - attach seeds the cursor at whatever writeSeq
        // already is, so this ordering is what makes the block "new".
        SegmentMemory.Release64(b, HeaderOffset.WriteSeq, 1ul);

        var view = new BlockView();
        Assert.True(r.Next(view));
        Assert.Equal(0.0f, view.Channel(0)[0], 5);
        Assert.Equal(1000.0f, view.Channel(1)[0], 5);
        Assert.Equal(1002.0f, view.Channel(1)[2], 5);
    }

    [Fact]
    public void AddressingRoundTrip_NormativeArithmeticAgreesWithReader()
    {
        using var prod = new TestProducer(new LayoutParams(3, 6, 2, SampleFormat.Float32));
        var r = new Reader();
        Assert.Equal(AttachResult.Ok, r.Attach(prod.BasePtr, prod.Size));   // before publishing

        prod.Publish();

        uint slotArrayOffset = SegmentMemory.U32(prod.Raw, HeaderOffset.SlotArrayOffset);
        uint slotPayloadOffset = SegmentMemory.U32(prod.Raw, HeaderOffset.SlotPayloadOffset);
        uint channelPlaneBytes = SegmentMemory.U32(prod.Raw, HeaderOffset.ChannelPlaneBytes);
        byte* slot = prod.Raw + slotArrayOffset;            // seq 0 → slot 0
        byte* plane1 = slot + slotPayloadOffset + channelPlaneBytes;
        float manualSample0 = *(float*)plane1;

        var b = new BlockView();
        Assert.True(r.Next(b));
        Assert.Equal(manualSample0, b.Channel(1)[0], 5);
    }
}
