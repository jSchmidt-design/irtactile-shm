// StreamReader - the whole stream-shm consumer, via Client.
//
// The acceptance criterion for this half of the API is that it COMPUTES NO
// TIMEOUTS. The wait timeout, the staleness threshold, the polling interval
// and the coalescing rule all live inside Client, derived from two header
// fields it is already holding.
//
//     dotnet run --project samples/StreamReader -- <resolvedShmName> [resolvedEventName]
//
//   With no event name the client polls. Consumer source is identical in both
//   modes; only the latency differs.
//
// The producer appends a size-class suffix ("_256k", "_512k" or "_1m") to the
// base name before creating the kernel objects. Pass the resolved name
// (e.g., "irTactile_Stream_Primary_256k"), not the base name.

using IrTactile.Shm.Stream;

// Example defaults: 8 ch @ 3000 Hz, P=6, ring 2. That fits the smallest size
// class, so the resolved name carries the "_256k" suffix.
string shmName = args.Length > 0 ? args[0] : "irTactile_Stream_Primary_256k";
string? eventName = args.Length > 1 ? args[1] : null;

// Pin the channel count this build expects; a mismatch returns Mismatch from
// Open rather than silently mis-decoding the stream.
var expected = new StreamRequest { ChannelCount = 8 };

using var c = new Client();
OpenResult r;
// NotFound is the normal case, not an error: the consumer may legitimately
// start before irTactile. Only ShouldRetry decides - a consumer that treats
// NotFound as fatal fails whenever it wins the startup race, and one that
// retries BadVersion spins forever.
while ((r = c.Open(shmName, eventName, expected)).ShouldRetry())
{
    Console.WriteLine($"waiting for irTactile ({r})...");
    Thread.Sleep(250);
}

if (r == OpenResult.Mismatch)
{
    Console.WriteLine($"irTactile stream: {c.Reader.MismatchDetail}");
    return 2;   // the installed profile is not this build's. Do not retry.
}
if (r != OpenResult.Ok)
{
    Console.WriteLine($"open failed: {r}");
    return 1;
}

Reader rd = c.Reader;
Console.WriteLine(
    $"attached to \"{rd.DeviceName}\": {rd.ChannelCount} ch @ {rd.SampleRateMilliHz / 1000.0:0.###} Hz " +
    $"(decimated {rd.RateDivisor}:1 from {rd.SourceSampleRate} Hz), " +
    $"{rd.SamplesPerBlock} samples/block, period {rd.PeriodMs:0.##} ms, " +
    $"{(c.NativeEvent != null ? "event" : "polling")} mode");
if (c.EventFallback)
    Console.WriteLine($"NOTE: event \"{eventName}\" could not be opened ({c.EventFallbackReason}) " +
                      "-- falling back to polling. Check the resolved event name.");

// wiredChannelMask is decided ONCE, at attach: an unwired channel is zeros
// for as long as this graph stands, so there is no buffer to allocate, no
// port to open and no bandwidth to budget for it. silentChannelMask, in the
// descriptor, is the per-block question and is re-read every block.
uint wired = rd.WiredChannelMask;
Console.Write("wired channels:");
for (int ch = 0; ch < rd.ChannelCount; ++ch)
    if ((wired & (1u << ch)) != 0) Console.Write($" {ch}");
Console.WriteLine();

var b = new BlockView();
ulong blocks = 0;
for (;;)
{
    WaitResult w = c.Wait();

    if (w == WaitResult.Reconfigured)
    {
        // An ADDRESSING field changed. Client has already re-snapshotted the
        // header before returning this, so every accessor is current. What
        // the consumer must redo is (a) its own structural assertion and (b)
        // everything it sized from the old layout. A snapshot is valid for
        // exactly one layoutSeq.
        Console.WriteLine($"stream reconfigured: now {rd.ChannelCount} ch, " +
                          $"{rd.SamplesPerBlock} samples/block; re-checking structure and resizing");
        continue;
    }
    if (w == WaitResult.ProducerRestarted)
    {
        // The header was rewritten but the memory layout did not move.
        // Nothing derived from the geometry needs rebuilding, which is the
        // entire reason this is a separate value from Reconfigured.
        Console.WriteLine($"producer restarted (\"{rd.DeviceName}\"); layout unchanged");
        continue;
    }
    if (w == WaitResult.Closed) break;

    // Drain. Wakes coalesce - the event is auto-reset, so one wake does not
    // mean one block once the consumer falls behind.
    while (c.Next(b))
    {
        ++blocks;
        if (blocks % 100 == 0)
        {
            Console.WriteLine($"block {blocks}  seq={b.Seq}  n={b.SampleCount}  " +
                              $"flags=0x{b.Flags:x}  silent=0x{b.SilentChannelMask:x8}  " +
                              $"dropped={rd.DroppedBlocks}");
        }
    }

    // The whole of crash detection from the consumer side, in one line.
    // Quiet (engine up, this device not driven) and Stalled (Active but the
    // timestamp went cold) both answer "no blocks"; telling them apart is the
    // entire reason deviceState exists.
    if (c.State() == StreamState.Stalled)
        Console.WriteLine($"WARNING: irTactile appears wedged (no publish in {rd.StaleAfterMs:0} ms)");
}

Console.WriteLine($"producer closed. {blocks} blocks, {rd.DroppedBlocks} dropped.");
return 0;
