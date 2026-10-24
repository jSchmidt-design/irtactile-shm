// IrTactile.Shm.Stream - Reader: bytes in a mapped segment to blocks.
//
// SPDX-License-Identifier: MIT
//
// A managed reimplementation of the protocol sections of
// spec/stream-shm/v1/README.md, written against the spec (and checked against
// the conformance vectors), not transpiled from the C++ headers.
//
// Reader is bytes-in-a-buffer to blocks. The OS layer (mapping, event,
// synthetic clock) is Client.
//
// Minimal consumer:
//
//     var b = new BlockView();
//     while (reader.Next(b))
//         for (int ch = 0; ch < b.ChannelCount; ++ch)
//             foreach (float s in b.Channel(ch)) use(s);
//
// Threading: a Reader instance is single-threaded, like the C++ one. The
// segment it reads is concurrently written by the producer - all cross-thread
// fields are touched exclusively through Volatile.Read (acquire).

using System.Runtime.CompilerServices;

namespace IrTactile.Shm.Stream;

/// <summary>
/// Bytes in a mapped segment to blocks. The liveness helpers take a synthetic
/// clock reading as a parameter so they are unit-testable.
/// </summary>
public sealed unsafe class Reader
{
    /// <summary>Classification of the most recent snapshot change.</summary>
    public enum ReconfigureKind : uint
    {
        /// <summary>No change since the last snapshot was taken.</summary>
        None,

        /// <summary>An addressing-relevant field changed.</summary>
        Geometry,

        /// <summary>Only diagnostic / non-addressing fields changed.</summary>
        Cosmetic,
    }

    /// <summary>
    /// Attach to a mapped segment. <paramref name="expected"/> may be null ⇒
    /// no structural assertion. The pointer must stay valid for the reader's
    /// lifetime (<see cref="Client"/> owns that for real mappings).
    /// </summary>
    public AttachResult Attach(IntPtr basePtr, long mappedBytes, StreamRequest? expected = null)
    {
        _base = (byte*)basePtr;
        _mappedBytes = mappedBytes;
        _attached = false;
        _dropped = 0;
        _mismatch = "";

        if (_base == null || mappedBytes < StreamLayout.HeaderBytes)
            return AttachResult.Malformed;

        // A segment is zero-filled at creation: magic == 0 means "header not
        // written yet", not an error.
        uint magic = AcquireU32(StreamLayout.HeaderOffset.Magic);
        if (magic == 0) return AttachResult.NotReady;
        if (magic != StreamLayout.Magic) return AttachResult.Malformed;

        if (!SnapshotHeader()) return AttachResult.NotReady;

        if (_hdr.VersionMajor != StreamLayout.VersionMajor) return AttachResult.BadVersion;
        // A higher versionMinor is tolerated: additives live behind runtime
        // offset fields. Unknown enum values are NOT - they would misread the
        // payload silently. Checked here for the BadVersion result, and again
        // inside ValidateLayout() so a rewrite cannot smuggle one past a reader
        // that is already attached.
        if (!KnownEnums(_hdr)) return AttachResult.BadVersion;

        if (!ValidateLayout()) return AttachResult.Malformed;

        if (expected != null)
        {
            AttachResult r = CheckRequest(expected);
            if (r != AttachResult.Ok) return r;
        }

        _attached = true;

        // Start at the head, not 0: the producer may have been running for
        // hours. RewindToOldest() reads whatever history the ring holds.
        _cursor = AcquireU64(StreamLayout.HeaderOffset.WriteSeq);
        return AttachResult.Ok;
    }

    /// <summary>
    /// After Mismatch: which fields disagreed and by how much, ready to log.
    /// Empty in every other case. Valid until the next Attach().
    /// </summary>
    public string MismatchDetail => _mismatch;

    /// <summary>True between a successful Attach and a hostile reconfigure.</summary>
    public bool Attached => _attached;

    /// <summary>
    /// Start from the oldest block the ring still holds rather than from the
    /// head. The readable window is ringBlocks - 1: the writer owns slot
    /// head % ringBlocks for the whole of the current publish period.
    /// </summary>
    public void RewindToOldest()
    {
        if (!_attached) return;
        ulong head = AcquireU64(StreamLayout.HeaderOffset.WriteSeq);
        ulong history = _hdr.RingBlocks - 1;
        _cursor = head > history ? head - history : 0;
    }

    /// <summary>Lossless: the next block in sequence. False when caught up.</summary>
    public bool Next(BlockView outBlock)
    {
        if (!_attached) return false;
        if (!RefreshIfReconfigured()) return false;

        ulong head = AcquireU64(StreamLayout.HeaderOffset.WriteSeq);
        if (head == _cursor) return false;

        ulong window = _hdr.RingBlocks - 1;

        // Unsigned differences, never a '<': correct under wrap and under a
        // cursor briefly ahead of head (torn 32-bit read of writeSeq).
        if (head - _cursor > window)
        {
            _dropped += (head - _cursor) - window;
            _cursor = head - window;
        }

        while (_cursor != head)
        {
            ulong seq = _cursor;
            ++_cursor;
            if (CopyBlock(seq, outBlock)) return true;
            // Recycled or under construction: count it and move on.
            ++_dropped;
        }
        return false;
    }

    /// <summary>
    /// Latest-value-wins: the newest complete block, or false if there is none
    /// newer than the one already returned.
    /// </summary>
    public bool Latest(BlockView outBlock)
    {
        if (!_attached) return false;
        if (!RefreshIfReconfigured()) return false;

        ulong head = AcquireU64(StreamLayout.HeaderOffset.WriteSeq);
        if (head == 0 || head == _cursor) return false;

        ulong seq = head - 1;
        _cursor = head;
        if (!CopyBlock(seq, outBlock)) { ++_dropped; return false; }
        return true;
    }

    // --- layout-derived accessors, from the seqlock snapshot ---

#pragma warning disable CS1591
    public uint ChannelCount => _hdr.ChannelCount;
    public uint SamplesPerBlock => _hdr.SamplesPerBlock;
    public uint RingBlocks => _hdr.RingBlocks;
    public uint SampleRateMilliHz => _hdr.SampleRateMilliHz;
    public uint SourceSampleRate => _hdr.SourceSampleRate;
    public uint RateDivisor => _hdr.RateDivisor;
    public SampleFormat Format => (SampleFormat)_hdr.SampleFormat;
    public ulong QpcFrequency => _hdr.QpcFrequency;
    public string DeviceName => _hdr.GetDeviceName();
    public string DeviceGuid => _hdr.GetDeviceGuid();
    public uint ProducerPid => _hdr.ProducerPid;
#pragma warning restore CS1591

    /// <summary>
    /// Bit c ⇒ channel c has a source in the live graph. Read live, not from
    /// the snapshot: it is a concurrent field, not seqlock-protected.
    /// </summary>
    public uint WiredChannelMask =>
        _attached ? AcquireU32(StreamLayout.HeaderOffset.WiredChannelMask) : 0;

    /// <summary>Blocks lost to overrun, recycling and discards.</summary>
    public ulong DroppedBlocks => _dropped;

    /// <summary>Blocks published, read live.</summary>
    public ulong WriteSeq =>
        _attached ? AcquireU64(StreamLayout.HeaderOffset.WriteSeq) : 0;

    /// <summary>The live layoutSeq; odd ⇒ header being rewritten.</summary>
    public uint LayoutSeq =>
        _attached ? AcquireU32(StreamLayout.HeaderOffset.LayoutSeq) : 0;

    /// <summary>What the producer is doing with this device, read live.</summary>
    public DeviceState DeviceState =>
        _attached ? (DeviceState)AcquireU32(StreamLayout.HeaderOffset.DeviceState)
                  : DeviceState.Inactive;

    /// <summary>QPC at the last publish, read live.</summary>
    public ulong LastPublishQpc =>
        _attached ? AcquireU64(StreamLayout.HeaderOffset.LastPublishQpc) : 0;

    /// <summary>
    /// Liveness, evaluated in one place. A stale timestamp is only meaningful
    /// under Active; Inactive and Closed are legitimate quiet.
    /// </summary>
    public StreamState State(ulong nowQpc)
    {
        switch (DeviceState)
        {
            case DeviceState.Closed: return StreamState.Closed;
            case DeviceState.Inactive: return StreamState.Quiet;
            case DeviceState.Active: break;
        }
        ulong last = LastPublishQpc;
        ulong freq = _hdr.QpcFrequency;
        if (freq == 0 || last == 0 || nowQpc <= last) return StreamState.Live;
        double elapsedMs = (nowQpc - last) * 1000.0 / freq;
        return elapsedMs >= StaleAfterMs ? StreamState.Stalled : StreamState.Live;
    }

    /// <summary>Publish period, from samplesPerBlock and sampleRateMilliHz alone.</summary>
    public double PeriodMs => StreamLayout.PeriodMsOf(_hdr);

    /// <summary>
    /// Blocks published per second - the figure the device configuration calls
    /// the update rate, and the reciprocal of <see cref="PeriodMs"/>.
    /// </summary>
    public double PublishHz => StreamLayout.PublishHzOf(_hdr);

    /// <summary>The library's wait timeout: max(50 ms, 4 × period).</summary>
    public double WaitTimeoutMs => StreamLayout.WaitTimeoutMsOf(_hdr);

    /// <summary>The staleness threshold: max(100 ms, 5 × period).</summary>
    public double StaleAfterMs => StreamLayout.StaleAfterMsOf(_hdr);

    /// <summary>The kind of change detected by the last successful RefreshSnapshot().</summary>
    public ReconfigureKind LastReconfigureKind => _lastReconfigureKind;

    /// <summary>
    /// Re-read layoutSeq and, if it moved, take a fresh header snapshot under
    /// the seqlock. Returns false if the new snapshot fails validation (the
    /// reader is detached). The change kind is available via
    /// <see cref="LastReconfigureKind"/>.
    /// </summary>
    public bool RefreshSnapshot()
    {
        if (AcquireU32(StreamLayout.HeaderOffset.LayoutSeq) == _snapLayoutSeq)
        {
            _lastReconfigureKind = ReconfigureKind.None;
            return true;
        }
        if (!SnapshotHeader()) { _attached = false; return false; }
        if (!ValidateLayout()) { _attached = false; return false; }
        return true;
    }

    /// <summary>
    /// The current header snapshot (layout fields only; the concurrent range
    /// is always read live). Valid for the layoutSeq it was taken under -
    /// re-read after a wait reports Reconfigured or ProducerRestarted.
    /// </summary>
    public ref readonly ShmStreamHeader Header => ref _hdr;

    /// <summary>
    /// Drop the segment pointer without disturbing <see cref="MismatchDetail"/>.
    /// <see cref="Client.ReleaseHandles"/> calls this before unmapping the
    /// view, so no later call can dereference the released mapping. Re-attach
    /// with <see cref="Attach"/>; a full reset is Attach(IntPtr.Zero, 0), which
    /// also clears the mismatch detail.
    /// </summary>
    internal void DetachFromMapping()
    {
        _attached = false;
        _base = null;
        _mappedBytes = 0;
    }

    /// <summary>
    /// Test-only: drives one cursor step with an injected stale snapshot
    /// layoutSeq and skips the refresh, to reproduce a two-thread
    /// interleaving single-threaded.
    /// </summary>
    internal bool TestCopyStepWithStaleSnapLayoutSeq(uint staleSnapLayoutSeq, BlockView outBlock)
    {
        if (!_attached) return false;
        _snapLayoutSeq = staleSnapLayoutSeq;

        ulong head = AcquireU64(StreamLayout.HeaderOffset.WriteSeq);
        if (head == _cursor) return false;

        ulong seq = _cursor;
        ++_cursor;
        if (CopyBlock(seq, outBlock)) return true;
        ++_dropped;
        return false;
    }

    // =========================================================================
    // Internals
    // =========================================================================

    // The layoutSeq seqlock: odd → rewrite → even.
    private bool SnapshotHeader()
    {
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            uint s1 = AcquireU32(StreamLayout.HeaderOffset.LayoutSeq);
            if ((s1 & 1u) != 0) continue;   // rewrite in progress

            // Clamp to StreamLayout.HeaderBytes: a future producer may write
            // a larger headerBytes, but every offset we read is a runtime
            // field, so the v1 struct size is enough.
            uint producerHeaderBytes = AcquireU32(StreamLayout.HeaderOffset.HeaderBytes);
            if (producerHeaderBytes == 0) producerHeaderBytes = StreamLayout.HeaderBytes;
            if (producerHeaderBytes > StreamLayout.HeaderBytes) producerHeaderBytes = StreamLayout.HeaderBytes;
            if (producerHeaderBytes > _mappedBytes) return false;
            uint bytes = producerHeaderBytes;

            ShmStreamHeader prev = _hdr;
            bool hadSnapshot = _attached;

            ShmStreamHeader snap = default;
            Buffer.MemoryCopy(_base, &snap, StreamLayout.HeaderBytes, bytes);

            uint s2 = AcquireU32(StreamLayout.HeaderOffset.LayoutSeq);
            if (s1 == s2)
            {
                _hdr = snap;
                _snapLayoutSeq = s2;
                _lastReconfigureKind = hadSnapshot ? ClassifyReconfigureKind(prev, _hdr)
                                                   : ReconfigureKind.None;
                return true;
            }
        }
        return false;
    }

    // The two enums a minor version may legitimately extend. A reader that
    // falls through to a default instead of refusing misreads the payload
    // silently (spec/common/versioning.md, reader rule 3).
    private static bool KnownEnums(in ShmStreamHeader h)
    {
        var fmt = (SampleFormat)h.SampleFormat;
        if (fmt != SampleFormat.Float32 && fmt != SampleFormat.Int16) return false;
        // Unknown layouts and Interleaved (known, but reserved - no v1 producer
        // emits it) both land here: this reader only decodes planar payloads.
        return (ChannelLayout)h.ChannelLayout == ChannelLayout.Planar;
    }

    // Every offset validated against the real mapped size, not against itself.
    private bool ValidateLayout()
    {
        ref readonly ShmStreamHeader h = ref _hdr;
        // Re-checked on every snapshot, not only at attach: a producer restart
        // may rewrite either enum, and BytesPerSampleOf() answers 4 for any
        // unknown format - so without this an unknown sampleFormat would decode
        // as Float32, and Interleaved would decode as Planar, in both cases
        // silently.
        if (!KnownEnums(h)) return false;
        if (h.TotalBytes > (ulong)_mappedBytes) return false;
        if (h.HeaderBytes < 64 || h.HeaderBytes > h.TotalBytes) return false;
        if (h.ChannelCount < StreamLayout.MinChannels || h.ChannelCount > StreamLayout.MaxChannels) return false;
        if (h.RingBlocks < StreamLayout.MinRingBlocks || h.RingBlocks > StreamLayout.MaxRingBlocks) return false;
        if (h.SamplesPerBlock == 0) return false;
        if (h.SampleRateMilliHz == 0) return false;
        if (h.RateDivisor == 0 || h.RateDivisor > StreamLayout.MaxRateDivisor) return false;
        if (h.BytesPerSample != StreamLayout.BytesPerSampleOf((SampleFormat)h.SampleFormat)) return false;
        if (h.SlotPayloadOffset < StreamLayout.BlockDescBytes) return false;

        // The plane-alignment guarantee ("planes are bytesPerSample-aligned")
        // is what lets a binding build a typed view - for Rust a *safety*
        // requirement - so it is validated, not assumed. All four terms of the
        // plane address must be multiples of bytesPerSample.
        if (h.SlotArrayOffset % h.BytesPerSample != 0) return false;
        if (h.SlotBytes % h.BytesPerSample != 0) return false;
        if (h.SlotPayloadOffset % h.BytesPerSample != 0) return false;
        if (h.ChannelPlaneBytes % h.BytesPerSample != 0) return false;

        // >= rather than ==: a future producer may pad planes; the consumer
        // reads the stride it is given.
        ulong minPlane = (ulong)h.SamplesPerBlock * h.BytesPerSample;
        if (h.ChannelPlaneBytes < minPlane) return false;

        ulong minSlot = h.SlotPayloadOffset + (ulong)h.ChannelCount * h.ChannelPlaneBytes;
        if (h.SlotBytes < minSlot) return false;
        if (h.SlotArrayOffset < h.HeaderBytes) return false;

        // The composite, not just the parts: each can be in range alone while
        // the array overruns.
        ulong end = h.SlotArrayOffset + (ulong)h.RingBlocks * h.SlotBytes;
        if (end > (ulong)_mappedBytes || end > h.TotalBytes) return false;
        return true;
    }

    /// <summary>
    /// Classify one header snapshot against a later one: <c>Geometry</c> if any
    /// addressing-relevant field moved, <c>Cosmetic</c> otherwise.
    /// <para>
    /// Public because <see cref="LastReconfigureKind"/> cannot answer "what
    /// changed since <em>I</em> last looked" - it reports the most recent
    /// refresh, and <see cref="Next"/> refreshes on every read cycle, so a
    /// reconfigure landing mid-drain is consumed before the caller asks and
    /// reads back as <c>None</c>. A consumer tracking reconfigures itself (its
    /// own wait loop over <see cref="Client.NativeEvent"/>, say) keeps a copy of
    /// <see cref="Header"/> as its watermark and calls this against it, which is
    /// what <see cref="Client"/> does internally. Every observer then gets an
    /// answer relative to its own watermark, with nothing to consume and no
    /// ordering to get wrong.
    /// </para>
    /// <para>
    /// Never returns <c>None</c>: gate the call on layoutSeq having moved. A
    /// producer restart that rewrites nothing still cycles the seqlock, and that
    /// is a restart the consumer must hear about - <c>Cosmetic</c>.
    /// </para>
    /// </summary>
    public static ReconfigureKind ClassifyReconfigure(in ShmStreamHeader before,
                                                      in ShmStreamHeader after) =>
        ClassifyReconfigureKind(before, after);

    private static ReconfigureKind ClassifyReconfigureKind(in ShmStreamHeader oldH,
                                                           in ShmStreamHeader cur)
    {
        if (oldH.ChannelCount != cur.ChannelCount ||
            oldH.SampleFormat != cur.SampleFormat ||
            oldH.ChannelLayout != cur.ChannelLayout ||
            oldH.SamplesPerBlock != cur.SamplesPerBlock ||
            oldH.RingBlocks != cur.RingBlocks ||
            oldH.BytesPerSample != cur.BytesPerSample ||
            oldH.SlotPayloadOffset != cur.SlotPayloadOffset ||
            oldH.ChannelArrayOffset != cur.ChannelArrayOffset ||
            oldH.SlotArrayOffset != cur.SlotArrayOffset ||
            oldH.SlotBytes != cur.SlotBytes ||
            oldH.ChannelPlaneBytes != cur.ChannelPlaneBytes)
            return ReconfigureKind.Geometry;
        return ReconfigureKind.Cosmetic;
    }

    private AttachResult CheckRequest(StreamRequest want)
    {
        var sb = new System.Text.StringBuilder(256);
        void Note(string field, ulong got, ulong expect)
        {
            if (sb.Length != 0) sb.Append("; ");
            sb.Append(field).Append(": segment has ").Append(got)
              .Append(", this build expects ").Append(expect);
        }

        if (want.ChannelCount != 0 && want.ChannelCount != _hdr.ChannelCount)
            Note("channelCount", _hdr.ChannelCount, want.ChannelCount);
        if (want.SampleRateMilliHz != 0 && want.SampleRateMilliHz != _hdr.SampleRateMilliHz)
            Note("sampleRateMilliHz", _hdr.SampleRateMilliHz, want.SampleRateMilliHz);
        if (want.SamplesPerBlock != 0 && want.SamplesPerBlock != _hdr.SamplesPerBlock)
            Note("samplesPerBlock", _hdr.SamplesPerBlock, want.SamplesPerBlock);
        if ((uint)want.SampleFormat != 0 && (uint)want.SampleFormat != _hdr.SampleFormat)
            Note("sampleFormat", _hdr.SampleFormat, (uint)want.SampleFormat);

        if (sb.Length == 0) return AttachResult.Ok;
        _mismatch = sb.ToString();
        return AttachResult.Mismatch;
    }

    // Re-check layoutSeq on every read cycle, so a producer restart rewriting
    // the header is noticed. Cheap: the concurrent range shares writeSeq's
    // cache line.
    private bool RefreshIfReconfigured() => RefreshSnapshot();

    // The copy, with the two descriptor checks around it. Both are plain
    // equality against the requested seq: kSlotBusy can never compare equal.
    private bool CopyBlock(ulong seq, BlockView outBlock)
    {
        byte* slot = _base + StreamLayout.SlotOffsetOf(_hdr, seq);

        if (Volatile.Read(ref *(ulong*)(slot + StreamLayout.DescOffset.Seq)) != seq)
            return false;

        int nCh = (int)_hdr.ChannelCount;

        // Every published block is full: a producer that cannot finish one
        // abandons it rather than publishing a short one, and reports the loss
        // through firstSampleIndex. So the bound is the header's configured
        // constant, taken from the seqlock snapshot whose extent
        // ValidateLayout() has already checked against channelPlaneBytes -
        // never from an unvalidated per-block field.
        int count = (int)_hdr.SamplesPerBlock;

        int total = nCh * count;
        if (outBlock.Samples.Length < total) outBlock.Samples = new float[total];

        bool isInt16 = (SampleFormat)_hdr.SampleFormat == SampleFormat.Int16;

        // The whole payload is one run unless the producer pads its planes.
        // No v1 producer does - the v1 formula is exact - but ValidateLayout()
        // tolerates channelPlaneBytes > the minimum so a future one could, and
        // the strided fallback below is what makes that tolerance honest.
        bool contiguous = _hdr.ChannelPlaneBytes == (ulong)count * _hdr.BytesPerSample;

        fixed (float* out0 = outBlock.Samples)
        {
            if (contiguous)
            {
                byte* payload = slot + _hdr.SlotPayloadOffset;
                if (isInt16)
                {
                    for (int i = 0; i < total; ++i)
                        out0[i] = StreamLayout.Int16ToFloat(
                            Unsafe.ReadUnaligned<short>(payload + i * 2));
                }
                else
                {
                    Buffer.MemoryCopy(payload, out0, (long)total * sizeof(float),
                                      (long)total * sizeof(float));
                }
            }
            else
            {
                for (int ch = 0; ch < nCh; ++ch)
                {
                    byte* plane = slot + StreamLayout.PlaneOffsetInSlot(_hdr, (uint)ch);
                    float* dst = out0 + ch * count;
                    if (isInt16)
                    {
                        for (int i = 0; i < count; ++i)
                            dst[i] = StreamLayout.Int16ToFloat(
                                Unsafe.ReadUnaligned<short>(plane + i * 2));
                    }
                    else
                    {
                        Buffer.MemoryCopy(plane, dst, (long)count * sizeof(float),
                                          (long)count * sizeof(float));
                    }
                }
            }
        }

        // The descriptor is copied INSIDE the bracket, with the planes: the
        // producer claims a recycled slot (kSlotBusy) before it rewrites these
        // fields, so reading them after the re-check could pair this block's
        // samples with the next block's firstSampleIndex - which is what
        // gap_samples is computed from.
        ulong firstSampleIndex = *(ulong*)(slot + StreamLayout.DescOffset.FirstSampleIndex);
        ulong timestampQpc = *(ulong*)(slot + StreamLayout.DescOffset.TimestampQpc);
        uint flags = *(uint*)(slot + StreamLayout.DescOffset.Flags);
        uint silentChannelMask = *(uint*)(slot + StreamLayout.DescOffset.SilentChannelMask);

        // The slot may have been recycled, or the layout rewritten, mid-copy:
        // re-check the descriptor seq, the live layoutSeq, and the
        // descriptor's layoutEpoch.
        if (Volatile.Read(ref *(ulong*)(slot + StreamLayout.DescOffset.Seq)) != seq)
            return false;
        if (AcquireU32(StreamLayout.HeaderOffset.LayoutSeq) != _snapLayoutSeq)
            return false;
        if (Volatile.Read(ref *(uint*)(slot + StreamLayout.DescOffset.LayoutEpoch)) != _snapLayoutSeq)
            return false;

        outBlock.Seq = seq;
        outBlock.FirstSampleIndex = firstSampleIndex;
        outBlock.TimestampQpc = timestampQpc;
        outBlock.SampleCount = count;
        outBlock.ChannelCount = nCh;
        outBlock.Flags = flags;
        outBlock.SilentChannelMask = silentChannelMask;
        return true;
    }

    // Acquire loads for the concurrently-written fields (bytes [80, 112) plus
    // magic and headerBytes, read during attach/snapshot). Volatile.Read is an
    // acquire load; 64-bit fields are naturally aligned by the layout, so on a
    // 64-bit host the read is tear-free.
    //
    // 32-bit hosts: spec/common/concurrency.md asks for Interlocked.Read on the
    // 64-bit fields, which is NOT usable here - on x86 it lowers to
    // `lock cmpxchg8b`, a read-modify-write that access-violates against the
    // read-only view this binding maps. Volatile.Read is what remains, and its
    // 8-byte atomicity on x86 is a hardware property rather than a documented
    // .NET guarantee. This binding is therefore supported on 64-bit hosts only
    // (README, "Platform"); irTactile itself ships no 32-bit producer.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private uint AcquireU32(int offset) => Volatile.Read(ref *(uint*)(_base + offset));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private ulong AcquireU64(int offset) => Volatile.Read(ref *(ulong*)(_base + offset));

    private byte* _base;
    private long _mappedBytes;
    private bool _attached;
    private ShmStreamHeader _hdr;
    private uint _snapLayoutSeq;
    private ReconfigureKind _lastReconfigureKind = ReconfigureKind.None;
    private ulong _cursor;
    private ulong _dropped;
    private string _mismatch = "";
}
