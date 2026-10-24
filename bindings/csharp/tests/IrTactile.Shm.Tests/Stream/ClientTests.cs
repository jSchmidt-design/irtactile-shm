// Client-level tests, over a real named segment.
//
// SPDX-License-Identifier: MIT
//
// Everything else in this suite runs Reader over a pinned byte[], which is the
// right level for the format itself. Client's own logic - the wait/classify
// bookkeeping that sits between Reader and the consumer - has no representation
// there and none in the conformance vectors either (those drive a Reader over a
// buffer; a Client owns a real mapping). So it gets a small Win32-backed suite.

using System.IO.MemoryMappedFiles;
using IrTactile.Shm.Stream;
using static IrTactile.Shm.Stream.StreamLayout;

namespace IrTactile.Shm.Tests.Stream;

public unsafe class ClientTests
{
    /// <summary>A real named segment, created and driven by a TestProducer.</summary>
    private sealed class MappedProducer : IDisposable
    {
        private readonly MemoryMappedFile _mmf;
        private readonly MemoryMappedViewAccessor _view;
        private readonly byte* _raw;

        public MappedProducer(LayoutParams p)
        {
            Name = "irtactile_shm_test_" + Guid.NewGuid().ToString("N");
            _mmf = MemoryMappedFile.CreateNew(Name, (long)ComputeTotalSize(p));
            _view = _mmf.CreateViewAccessor(0, 0, MemoryMappedFileAccess.ReadWrite);
            byte* ptr = null;
            _view.SafeMemoryMappedViewHandle.AcquirePointer(ref ptr);
            _raw = ptr;
            Producer = new TestProducer(p, _raw, (long)ComputeTotalSize(p));
        }

        public string Name { get; }
        public TestProducer Producer { get; }
        public byte* Raw => _raw;

        public void Dispose()
        {
            Producer.Dispose();
            _view.SafeMemoryMappedViewHandle.ReleasePointer();
            _view.Dispose();
            _mmf.Dispose();
        }
    }

    [Fact]
    public void Wait_GeometryRewriteConsumedByTheDrain_IsStillReportedAsReconfigured()
    {
        // The regression this exists for. Reader.Next() re-snapshots on every
        // read cycle, so a rewrite landing mid-drain is consumed by the drain
        // loop - and Reader.LastReconfigureKind, which describes the most
        // recent refresh, reads back None by the time Wait() looks. Classifying
        // from it reports a GEOMETRY change as ProducerRestarted ("layout
        // unchanged"), and the consumer skips resizing everything it sized from
        // the old layout. Client must classify against its own watermark
        // instead.
        if (!OperatingSystem.IsWindows()) return;

        using var segment = new MappedProducer(new LayoutParams(4, 8, 4, SampleFormat.Float32));
        using var c = new Client();
        Assert.Equal(OpenResult.Ok, c.Open(segment.Name, null));

        segment.Producer.Publish();
        Assert.Equal(WaitResult.Blocks, c.Wait(TimeSpan.FromMilliseconds(50)));

        var b = new BlockView();
        while (c.Next(b)) { }

        // The producer restarts into the segment with a different channel count.
        byte* raw = segment.Raw;
        SegmentMemory.SetU32(raw, HeaderOffset.ChannelCount, 2u);
        SegmentMemory.SetU32(raw, HeaderOffset.SlotBytes,
            Align64(SlotPayloadOffsetOf() + 2u * SegmentMemory.U32(raw, HeaderOffset.ChannelPlaneBytes)));
        SegmentMemory.Release32(raw, HeaderOffset.LayoutSeq,
            SegmentMemory.U32(raw, HeaderOffset.LayoutSeq) + 2u);

        // The drain consumes the rewrite before Wait() ever sees it: the
        // reader's latch shows Geometry here, and is spent by the time
        // Classify() runs its own RefreshSnapshot().
        while (c.Next(b)) { }
        Assert.Equal(Reader.ReconfigureKind.Geometry, c.Reader.LastReconfigureKind);

        Assert.Equal(WaitResult.Reconfigured, c.Wait(TimeSpan.FromMilliseconds(50)));
        Assert.Equal(2u, c.Reader.ChannelCount);
    }

    [Fact]
    public void Wait_CosmeticRewrite_IsReportedAsProducerRestarted()
    {
        // The other side of the same classification: a restart that moves only
        // diagnostic fields must NOT be escalated to Reconfigured, or every
        // consumer rebuilds its buffers on every producer restart.
        if (!OperatingSystem.IsWindows()) return;

        using var segment = new MappedProducer(new LayoutParams(4, 8, 4, SampleFormat.Float32));
        using var c = new Client();
        Assert.Equal(OpenResult.Ok, c.Open(segment.Name, null));

        byte* raw = segment.Raw;
        SegmentMemory.SetU32(raw, HeaderOffset.DiagEngineBlockSize, 128u);
        SegmentMemory.SetU32(raw, HeaderOffset.SourceSampleRate, 96000u);
        SegmentMemory.Release32(raw, HeaderOffset.LayoutSeq,
            SegmentMemory.U32(raw, HeaderOffset.LayoutSeq) + 2u);

        var b = new BlockView();
        while (c.Next(b)) { }

        Assert.Equal(WaitResult.ProducerRestarted, c.Wait(TimeSpan.FromMilliseconds(50)));
        Assert.Equal(4u, c.Reader.ChannelCount);
    }

    [Fact]
    public void ReleaseHandles_LeavesTheReadsSafeRatherThanDereferencingTheUnmappedView()
    {
        if (!OperatingSystem.IsWindows()) return;

        using var segment = new MappedProducer(new LayoutParams(2, 4, 2, SampleFormat.Float32));
        using var c = new Client();
        Assert.Equal(OpenResult.Ok, c.Open(segment.Name, null));
        segment.Producer.Publish();

        c.ReleaseHandles();

        // Both surfaces: Client's gate, and the Reader's own detached state.
        var b = new BlockView();
        Assert.False(c.IsOpen);
        Assert.False(c.Next(b));
        Assert.False(c.Latest(b));
        Assert.Equal(StreamState.Closed, c.State());
        Assert.False(c.Reader.Attached);
        Assert.False(c.Reader.Next(b));
    }

    [Fact]
    public void Open_MissingSegment_IsNotFoundAndRetryable()
    {
        if (!OperatingSystem.IsWindows()) return;

        using var c = new Client();
        OpenResult r = c.Open("irtactile_shm_test_definitely_absent_" + Guid.NewGuid().ToString("N"),
                              null);
        Assert.Equal(OpenResult.NotFound, r);
        Assert.True(r.ShouldRetry());
        Assert.False(c.IsOpen);
    }
}
