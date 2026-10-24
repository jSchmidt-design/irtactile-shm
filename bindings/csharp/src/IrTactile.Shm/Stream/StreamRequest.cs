// IrTactile.Shm.Stream - StreamRequest and BlockView.
//
// SPDX-License-Identifier: MIT
//
// StreamRequest pins layout fields the consumer requires; BlockView is the
// per-block decoded view handed back from Next / Latest. Both are pure data -
// no handles, no resources.

namespace IrTactile.Shm.Stream;

/// <summary>
/// Structural assertion for <see cref="Reader.Attach"/>; never a request. Any
/// field left at zero is not checked. On mismatch,
/// <see cref="Reader.MismatchDetail"/> reports the disagreements.
/// <para>
/// Assert what would break you, not everything you know. A consumer draining
/// with <c>while (Next(b))</c> handles any block size it is given, and pinning
/// <see cref="SamplesPerBlock"/> turns the operator's next latency tweak into a
/// hard Mismatch at open.
/// </para>
/// </summary>
public sealed class StreamRequest
{
    /// <summary>0 ⇒ do not check.</summary>
    public uint ChannelCount { get; set; }

    /// <summary>0 ⇒ do not check.</summary>
    public uint SampleRateMilliHz { get; set; }

    /// <summary>
    /// 0 ⇒ do not check - and the right value unless the processing downstream
    /// is genuinely fixed-size (an FFT, a fixed hop). To confirm the cadence
    /// rather than assert it, attach without pinning and read
    /// <see cref="Reader.PublishHz"/>.
    /// </summary>
    public uint SamplesPerBlock { get; set; }

    /// <summary>0 (default enum value) ⇒ do not check.</summary>
    public SampleFormat SampleFormat { get; set; }
}

/// <summary>
/// A block, copied out of the ring: the protocol requires a copy bracketed by
/// descriptor re-checks. Channel data is planar, converted to float32 whatever
/// the segment's format, so a consumer never branches on sampleFormat.
/// </summary>
public sealed class BlockView
{
    /// <summary>Block index.</summary>
    public ulong Seq { get; internal set; }

    /// <summary>Stream-rate samples emitted before this block, including any later discarded.</summary>
    public ulong FirstSampleIndex { get; internal set; }

    /// <summary>QPC at publish.</summary>
    public ulong TimestampQpc { get; internal set; }

    /// <summary>Samples per channel - always the header's samplesPerBlock.</summary>
    public int SampleCount { get; internal set; }

    /// <summary><see cref="Stream.BlockFlags"/>.</summary>
    public uint Flags { get; internal set; }

    /// <summary>Bit c set ⇒ channel c is all-zero in this block.</summary>
    public uint SilentChannelMask { get; internal set; }

    /// <summary>Channel count of the layout this block was decoded under.</summary>
    public int ChannelCount { get; internal set; }

    internal float[] Samples = Array.Empty<float>();   // ChannelCount × SampleCount, planar

    /// <summary>
    /// <see cref="SampleCount"/> floats of channel <paramref name="ch"/>,
    /// contiguous. Empty span for an out-of-range channel.
    /// </summary>
    public ReadOnlySpan<float> Channel(int ch) =>
        (uint)ch < (uint)ChannelCount
            ? Samples.AsSpan(ch * SampleCount, SampleCount)
            : ReadOnlySpan<float>.Empty;
}
