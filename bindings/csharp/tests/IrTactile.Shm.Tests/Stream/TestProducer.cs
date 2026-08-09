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
    private readonly PinnedSection? _section;   // null when driving a caller's buffer
    private readonly LayoutParams _p;
    private ulong _writeSeq;
    private ulong _first;

    public TestProducer(LayoutParams p, uint channelsWired = 0xFFFFFFFFu)
        : this(p, null, 0, channelsWired) { }

    /// <summary>
    /// Drive a buffer the caller owns - a real mapped section, for the tests
    /// that need a Client rather than a Reader. Pass null to allocate one.
    /// </summary>
    public TestProducer(LayoutParams p, byte* raw, long size, uint channelsWired = 0xFFFFFFFFu)
    {
        _p = p;
        if (raw == null)
        {
            _section = new PinnedSection((long)ComputeTotalSize(p));
            Raw = _section.Raw;
            Size = _section.Length;
        }
        else
        {
            Raw = raw;
            Size = size;
        }
        byte* b = Raw;

        // magic stays 0 until every other field is in place (not-ready window).
        SectionMemory.SetU16(b, HeaderOffset.VersionMajor, VersionMajor);
        SectionMemory.SetU16(b, HeaderOffset.VersionMinor, VersionMinor);
        SectionMemory.SetU32(b, HeaderOffset.HeaderBytes, StreamLayout.HeaderBytes);
        SectionMemory.SetU32(b, HeaderOffset.TotalBytes, (uint)ComputeTotalSize(p));
        SectionMemory.SetU32(b, HeaderOffset.SampleFormat, (uint)p.Format);
        SectionMemory.SetU32(b, HeaderOffset.ChannelLayout, (uint)ChannelLayout.Planar);
        SectionMemory.SetU32(b, HeaderOffset.ChannelCount, p.ChannelCount);
        SectionMemory.SetU32(b, HeaderOffset.SampleRateMilliHz, 3_000_000u);
        SectionMemory.SetU32(b, HeaderOffset.SourceSampleRate, 48000u);
        SectionMemory.SetU32(b, HeaderOffset.RateDivisor, 16u);
        SectionMemory.SetU32(b, HeaderOffset.SamplesPerBlock, p.SamplesPerBlock);
        SectionMemory.SetU32(b, HeaderOffset.RingBlocks, p.RingBlocks);
        SectionMemory.SetU32(b, HeaderOffset.BytesPerSample, BytesPerSampleOf(p.Format));
        SectionMemory.SetU32(b, HeaderOffset.SlotPayloadOffset, SlotPayloadOffsetOf());
        SectionMemory.SetU32(b, HeaderOffset.ChannelArrayOffset, 0u);
        SectionMemory.SetU32(b, HeaderOffset.SlotArrayOffset, SlotArrayOffsetOf());
        SectionMemory.SetU32(b, HeaderOffset.SlotBytes, SlotBytesOf(p));
        SectionMemory.SetU32(b, HeaderOffset.ChannelPlaneBytes, ChannelPlaneBytesOf(p));
        SectionMemory.SetU64(b, HeaderOffset.QpcFrequency, 10_000_000ul);
        SectionMemory.SetU32(b, HeaderOffset.WiredChannelMask, channelsWired);
        SectionMemory.SetU32(b, HeaderOffset.DeviceState, (uint)DeviceState.Active);
        SectionMemory.Release32(b, HeaderOffset.Magic, Magic);
    }

    public byte* Raw { get; }
    public IntPtr BasePtr => (IntPtr)Raw;
    public long Size { get; }
    public ulong WriteSeq => _writeSeq;

    public void Dispose() => _section?.Dispose();

    private byte* SlotAt(ulong seq)
    {
        uint slotArrayOffset = SectionMemory.U32(Raw, HeaderOffset.SlotArrayOffset);
        uint ringBlocks = SectionMemory.U32(Raw, HeaderOffset.RingBlocks);
        uint slotBytes = SectionMemory.U32(Raw, HeaderOffset.SlotBytes);
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
        uint channelCount = SectionMemory.U32(b, HeaderOffset.ChannelCount);
        uint samplesPerBlock = SectionMemory.U32(b, HeaderOffset.SamplesPerBlock);
        uint bytesPerSample = SectionMemory.U32(b, HeaderOffset.BytesPerSample);
        uint slotPayloadOffset = SectionMemory.U32(b, HeaderOffset.SlotPayloadOffset);
        uint channelPlaneBytes = SectionMemory.U32(b, HeaderOffset.ChannelPlaneBytes);

        SectionMemory.Release64(slot, DescOffset.Seq, SlotBusy);

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

        SectionMemory.SetU64(slot, DescOffset.FirstSampleIndex, _first);
        SectionMemory.SetU64(slot, DescOffset.TimestampQpc, (_writeSeq + 1) * 1000);
        SectionMemory.SetU32(slot, DescOffset.Flags, flags);
        SectionMemory.SetU32(slot, DescOffset.SilentChannelMask, silent);
        // Producer obligation: stamp the layoutSeq the payload was encoded
        // under, so a consumer that re-snapshots to a newer layout rejects
        // this block instead of decoding it with the wrong strides.
        SectionMemory.SetU32(slot, DescOffset.LayoutEpoch,
                             Volatile.Read(ref *(uint*)(b + HeaderOffset.LayoutSeq)));

        SectionMemory.Release64(slot, DescOffset.Seq, _writeSeq);
        SectionMemory.Release64(b, HeaderOffset.LastPublishQpc, (_writeSeq + 1) * 1000);
        SectionMemory.Release64(b, HeaderOffset.WriteSeq, _writeSeq + 1);

        ++_writeSeq;
        _first += samplesPerBlock;
    }

    /// <summary>
    /// Claims the next slot (kSlotBusy) without completing the block - for
    /// slot-recycling / mid-construction tests.
    /// </summary>
    public void BeginPartialPublish()
    {
        SectionMemory.Release64(SlotAt(_writeSeq), DescOffset.Seq, SlotBusy);
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
