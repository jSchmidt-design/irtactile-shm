// A minimal producer, driving the exact publish protocol steps from the spec
// over an owned buffer. Deliberately independent of irTactile's producer so
// these tests exercise the *format*, not the engine wiring.
//
// SPDX-License-Identifier: MIT

using IrTactile.Shm.Stream;
using static IrTactile.Shm.Stream.StreamLayout;

namespace IrTactile.Shm.Tests.Stream;

internal sealed unsafe class TestProducer : IDisposable
{
    private readonly PinnedSegment? _segment;   // null when driving a caller's buffer
    private readonly LayoutParams _p;
    private ulong _writeSeq;
    private ulong _first;

    public TestProducer(LayoutParams p, uint channelsWired = 0xFFFFFFFFu)
        : this(p, null, 0, channelsWired) { }

    /// <summary>
    /// Drive a buffer the caller owns - a real mapped segment, for the tests
    /// that need a Client rather than a Reader. Pass null to allocate one.
    /// </summary>
    public TestProducer(LayoutParams p, byte* raw, long size, uint channelsWired = 0xFFFFFFFFu)
    {
        _p = p;
        if (raw == null)
        {
            _segment = new PinnedSegment((long)ComputeTotalSize(p));
            Raw = _segment.Raw;
            Size = _segment.Length;
        }
        else
        {
            Raw = raw;
            Size = size;
        }
        byte* b = Raw;

        // magic stays 0 until every other field is in place (not-ready window).
        SegmentMemory.SetU16(b, HeaderOffset.VersionMajor, VersionMajor);
        SegmentMemory.SetU16(b, HeaderOffset.VersionMinor, VersionMinor);
        SegmentMemory.SetU32(b, HeaderOffset.HeaderBytes, StreamLayout.HeaderBytes);
        SegmentMemory.SetU32(b, HeaderOffset.TotalBytes, (uint)ComputeTotalSize(p));
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
        SegmentMemory.SetU32(b, HeaderOffset.SlotArrayOffset, SlotArrayOffsetOf());
        SegmentMemory.SetU32(b, HeaderOffset.SlotBytes, SlotBytesOf(p));
        SegmentMemory.SetU32(b, HeaderOffset.ChannelPlaneBytes, ChannelPlaneBytesOf(p));
        SegmentMemory.SetU64(b, HeaderOffset.QpcFrequency, 10_000_000ul);
        SegmentMemory.SetU32(b, HeaderOffset.WiredChannelMask, channelsWired);
        SegmentMemory.SetU32(b, HeaderOffset.DeviceState, (uint)DeviceState.Active);
        SegmentMemory.Release32(b, HeaderOffset.Magic, Magic);
    }

    public byte* Raw { get; }
    public IntPtr BasePtr => (IntPtr)Raw;
    public long Size { get; }
    public ulong WriteSeq => _writeSeq;

    public void Dispose() => _segment?.Dispose();

    private byte* SlotAt(ulong seq)
    {
        uint slotArrayOffset = SegmentMemory.U32(Raw, HeaderOffset.SlotArrayOffset);
        uint ringBlocks = SegmentMemory.U32(Raw, HeaderOffset.RingBlocks);
        uint slotBytes = SegmentMemory.U32(Raw, HeaderOffset.SlotBytes);
        return Raw + slotArrayOffset + (long)(seq % ringBlocks) * slotBytes;
    }

    /// <summary>
    /// Publish one block. Every channel's samples equal
    /// (firstSampleIndex + i) + ch * 1000, so a test can predict any sample.
    /// </summary>
    public void Publish(bool forceSilent = false)
    {
        byte* b = Raw;
        byte* slot = SlotAt(_writeSeq);
        uint channelCount = SegmentMemory.U32(b, HeaderOffset.ChannelCount);
        uint samplesPerBlock = SegmentMemory.U32(b, HeaderOffset.SamplesPerBlock);
        uint bytesPerSample = SegmentMemory.U32(b, HeaderOffset.BytesPerSample);
        uint slotPayloadOffset = SegmentMemory.U32(b, HeaderOffset.SlotPayloadOffset);
        uint channelPlaneBytes = SegmentMemory.U32(b, HeaderOffset.ChannelPlaneBytes);

        SeqLock.BeginWrite(ref *(ulong*)(slot + DescOffset.Seq), SlotBusy);

        uint silent = 0;
        for (uint ch = 0; ch < channelCount; ++ch)
        {
            byte* plane = slot + slotPayloadOffset + (long)ch * channelPlaneBytes;
            bool nz = false;
            for (uint i = 0; i < samplesPerBlock; ++i)
            {
                float v = forceSilent ? 0.0f : _first + i + ch * 1000.0f;
                if (v != 0.0f) nz = true;
                if (bytesPerSample == 2)
                {
                    short q = FloatToInt16(v / 100000.0f);
                    *(short*)(plane + i * 2) = q;
                }
                else
                {
                    *(float*)(plane + i * 4) = v;
                }
            }
            if (!nz) silent |= 1u << (int)ch;
        }

        uint fullMask = channelCount >= 32 ? 0xFFFFFFFFu : (1u << (int)channelCount) - 1u;
        uint flags = silent == fullMask ? (uint)BlockFlags.Silence : 0u;
        if (_writeSeq == 0) flags |= (uint)BlockFlags.Discontinuity;

        SegmentMemory.SetU64(slot, DescOffset.FirstSampleIndex, _first);
        SegmentMemory.SetU64(slot, DescOffset.TimestampQpc, (_writeSeq + 1) * 1000);
        SegmentMemory.SetU32(slot, DescOffset.Flags, flags);
        SegmentMemory.SetU32(slot, DescOffset.SilentChannelMask, silent);
        // Producer obligation: stamp the layoutSeq the payload was encoded
        // under, so a consumer that re-snapshots to a newer layout rejects
        // this block instead of decoding it with the wrong strides.
        SegmentMemory.SetU32(slot, DescOffset.LayoutEpoch,
                             Volatile.Read(ref *(uint*)(b + HeaderOffset.LayoutSeq)));

        SegmentMemory.Release64(slot, DescOffset.Seq, _writeSeq);
        SegmentMemory.Release64(b, HeaderOffset.LastPublishQpc, (_writeSeq + 1) * 1000);
        SegmentMemory.Release64(b, HeaderOffset.WriteSeq, _writeSeq + 1);

        ++_writeSeq;
        _first += samplesPerBlock;
    }

    /// <summary>
    /// Claims the next slot (kSlotBusy) without completing the block - for
    /// slot-recycling / mid-construction tests.
    /// </summary>
    public void BeginPartialPublish()
    {
        SegmentMemory.Release64(SlotAt(_writeSeq), DescOffset.Seq, SlotBusy);
    }

    /// <summary>
    /// A partial block with <paramref name="samplesEmitted"/> samples already
    /// written is abandoned. Those samples were emitted and then discarded, so
    /// they count toward the stream position.
    /// </summary>
    public void AbandonPartialBlock(uint samplesEmitted)
    {
        BeginPartialPublish();
        _first += samplesEmitted;
    }
}
