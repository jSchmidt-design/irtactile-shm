// IrTactile.Shm.Stream - enumerations of the stream-shm v1 wire format.
//
// SPDX-License-Identifier: MIT
//
// Every numeric value here is normative: it is part of the wire format, not an
// implementation choice. See spec/stream-shm/v1/fields.md.

namespace IrTactile.Shm.Stream;

/// <summary>Payload sample encoding (header field <c>sampleFormat</c>).</summary>
public enum SampleFormat : uint
{
    /// <summary>32-bit IEEE-754 float, little-endian.</summary>
    Float32 = 1,

    /// <summary>16-bit two's complement, ±1.0 maps to ±32767.</summary>
    Int16 = 2,
}

/// <summary>Sample arrangement inside a slot (header field <c>channelLayout</c>).</summary>
public enum ChannelLayout : uint
{
    /// <summary>Channel 0's samples contiguous, then channel 1's, … The only v1 layout.</summary>
    Planar = 1,

    /// <summary>Reserved; never produced by a v1.0 producer. Readers reject it.</summary>
    Interleaved = 2,
}

/// <summary>
/// What the producer is doing with this device (header field <c>deviceState</c>).
/// Inactive is 0 so a freshly created, zero-filled section reads as "not being
/// driven" before the header is written.
/// </summary>
public enum DeviceState : uint
{
    /// <summary>Engine alive, this device is not being driven.</summary>
    Inactive = 0,

    /// <summary>Being driven; blocks should be arriving.</summary>
    Active = 1,

    /// <summary>The producer's stop ran - clean shutdown.</summary>
    Closed = 2,
}

/// <summary>Per-block flags (descriptor field <c>flags</c>).</summary>
[Flags]
public enum BlockFlags : uint
{
    /// <summary>No flags.</summary>
    None = 0,

    /// <summary>Every channel is zero in this block.</summary>
    Silence = 1u << 0,

    /// <summary>First block after start/reconfigure.</summary>
    Discontinuity = 1u << 1,
}

/// <summary>
/// Sections are sized into three discrete buckets; the suffix is part of the
/// section and event names. A restart needing a larger layout switches name
/// instead of colliding with a consumer still holding the old section open.
/// </summary>
public enum SizeClass : uint
{
    /// <summary>256 KiB section.</summary>
    S256K = 0,

    /// <summary>512 KiB section.</summary>
    S512K = 1,

    /// <summary>1 MiB section.</summary>
    S1M = 2,
}

/// <summary>Why an attach did not succeed - and whether to try again.</summary>
public enum AttachResult : uint
{
    /// <summary>Attached.</summary>
    Ok,

    /// <summary>magic == 0: section exists, header not written yet. RETRY.</summary>
    NotReady,

    /// <summary>versionMajor mismatch, or an unknown enum value.</summary>
    BadVersion,

    /// <summary>Wrong magic, or an offset that does not fit mappedBytes.</summary>
    Malformed,

    /// <summary>
    /// Valid, but not the structure this consumer expects. Waiting will not fix it.
    /// </summary>
    Mismatch,
}

/// <summary><see cref="DeviceState"/> with the staleness question already answered.</summary>
public enum StreamState : uint
{
    /// <summary>Active, and publishing on time.</summary>
    Live,

    /// <summary>Inactive - engine up, this device not driven. Legitimate.</summary>
    Quiet,

    /// <summary>Active, but lastPublishQpc is stale. Wedged or crashed.</summary>
    Stalled,

    /// <summary>The producer's stop ran.</summary>
    Closed,
}

/// <summary>What the consumer should do next after a wait.</summary>
public enum WaitResult : uint
{
    /// <summary>writeSeq advanced - drain with Next() / Latest().</summary>
    Blocks,

    /// <summary>Woken or timed out with nothing new; ask State why.</summary>
    NoBlocks,

    /// <summary>
    /// Same-section rewrite of an addressing-relevant field. The snapshot is
    /// already refreshed, so all layout-derived accessors are valid for the new
    /// layoutSeq. Re-check any <see cref="StreamRequest"/>.
    /// </summary>
    Reconfigured,

    /// <summary>
    /// Producer restart that left the memory layout unchanged; only
    /// cosmetic/diagnostic fields differ.
    /// </summary>
    ProducerRestarted,

    /// <summary>
    /// Producer shut down cleanly; drain what is left, then stop. Also returned
    /// when a header rewrite failed re-validation - the reader is detached
    /// either way and the stream is over for this <see cref="Client"/>.
    /// </summary>
    Closed,
}

/// <summary>
/// <see cref="Client"/>'s failure surface is <see cref="Reader"/>'s plus lookup
/// failures. Same retry-vs-give-up split.
/// </summary>
public enum OpenResult : uint
{
    /// <summary>Opened and attached.</summary>
    Ok,

    /// <summary>Section not found - irTactile is not up yet. RETRY.</summary>
    NotFound,

    /// <summary>Section exists, header not yet written. RETRY.</summary>
    NotReady,

    /// <summary>Wrong session, or a Global\ name without the privilege.</summary>
    AccessDenied,

    /// <summary>As <see cref="AttachResult.BadVersion"/>.</summary>
    BadVersion,

    /// <summary>As <see cref="AttachResult.Malformed"/>.</summary>
    Malformed,

    /// <summary>As <see cref="AttachResult.Mismatch"/> - the installed device profile is not yours.</summary>
    Mismatch,
}

/// <summary>Retry classification, the only retry logic a consumer should contain.</summary>
public static class ResultExtensions
{
    /// <summary>Only <see cref="AttachResult.NotReady"/> is retryable.</summary>
    public static bool ShouldRetry(this AttachResult r) => r == AttachResult.NotReady;

    /// <summary>NotFound and NotReady are both "producer not up yet".</summary>
    public static bool ShouldRetry(this OpenResult r) =>
        r == OpenResult.NotFound || r == OpenResult.NotReady;
}
