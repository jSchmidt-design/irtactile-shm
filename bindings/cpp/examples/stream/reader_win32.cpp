// reader_win32.cpp - the whole thing, via Client.
//
// The acceptance criterion for this half of the API is that it COMPUTES NO
// TIMEOUTS. The wait timeout, the staleness threshold, the polling interval and
// the coalescing rule all live inside Client, derived from two header fields it
// is already holding.
//
//     cl /std:c++17 /EHsc /I..\include reader_win32.cpp
//
// Usage: reader_win32 <resolvedShmName> [resolvedEventName]
//   With no event name the client polls. Consumer source is byte-identical in
//   both modes; only the latency differs.
//
// The producer appends a size-class suffix ("_256k", "_512k" or "_1m") to the
// base name before creating the kernel objects. Pass the resolved name
// (e.g., "irTactile_Stream_Primary_256k"), not the base name.

#include <irtactile/shm/stream/win32.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace irtactile::shm::stream;

int wmain(int argc, wchar_t** argv)
{
    // Example defaults: 8 ch @ 3000 Hz, P=6, ring 2. That fits the smallest
    // size class, so the resolved name carries the "_256k" suffix.
    const wchar_t* shmName   = argc > 1 ? argv[1] : L"irTactile_Stream_Primary_256k";
    const wchar_t* eventName = argc > 2 ? argv[2] : nullptr;

    // Pin the channel count this build expects; a mismatch returns Mismatch
    // from open() rather than silently mis-decoding the stream.
    static const StreamRequest kExpected{ /*channelCount*/ 8, 0, 0,
                                          static_cast<SampleFormat>(0) };

    Client     c;
    OpenResult r;
    // NotFound is the normal case, not an error: the consumer may legitimately
    // start before irTactile. Only shouldRetry() decides -- a consumer that
    // treats NotFound as fatal fails whenever it wins the startup race, and one
    // that retries BadVersion spins forever.
    while (shouldRetry(r = c.open(shmName, eventName, &kExpected))) {
        std::printf("waiting for irTactile (%s)...\n", toCString(r));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (r == OpenResult::Mismatch) {
        std::printf("irTactile stream: %s\n", c.reader().mismatchDetail());
        return 2;   // the installed profile is not this build's. Do not retry.
    }
    if (r != OpenResult::Ok) {
        std::printf("open failed: %s\n", toCString(r));
        return 1;
    }

    const Reader& rd = c.reader();
    std::printf("attached to \"%s\": %u ch @ %.3f Hz (decimated %u:1 from %u Hz), "
                "%u samples/block, period %.2f ms, %s mode\n",
                rd.deviceName(), rd.channelCount(), rd.sampleRateMilliHz() / 1000.0,
                rd.rateDivisor(), rd.sourceSampleRate(), rd.samplesPerBlock(),
                rd.periodMs(), c.nativeEvent() ? "event" : "polling");
    if (c.eventFallback())
        std::printf("NOTE: event \"%ls\" not found -- falling back to polling. "
                    "Check the resolved event name.\n", eventName);

    // wiredChannelMask is decided ONCE, at attach: an unwired channel is zeros
    // for as long as this graph stands, so there is no buffer to allocate, no
    // port to open and no bandwidth to budget for it. silentChannelMask, in the
    // descriptor, is the per-block question and is re-read every block.
    const uint32_t wired = rd.wiredChannelMask();
    std::printf("wired channels:");
    for (uint32_t ch = 0; ch < rd.channelCount(); ++ch)
        if (wired & (1u << ch)) std::printf(" %u", ch);
    std::printf("\n");

    BlockView b;
    uint64_t  blocks = 0;
    for (;;) {
        const WaitResult w = c.wait();

        if (w == WaitResult::Reconfigured) {
            // An ADDRESSING field changed: channel count, block size, ring
            // depth, sample format or a stride. Client has already re-snapshotted
            // the header before returning this, so every accessor below is
            // current. What the consumer must redo is (a) its own structural
            // assertion -- the structure may not be ours any more -- and (b)
            // everything it sized from the old layout: de-interleave buffers,
            // channel labels, resamplers, GPU allocations. A snapshot is valid
            // for exactly one layoutSeq.
            std::printf("stream reconfigured: now %u ch, %u samples/block; "
                        "re-checking structure and resizing\n",
                        rd.channelCount(), rd.samplesPerBlock());
            continue;
        }
        if (w == WaitResult::ProducerRestarted) {
            // The header was rewritten but the memory layout did not move --
            // typically a restart that renamed the device. Nothing derived from
            // the geometry needs rebuilding, which is the entire reason this is
            // a separate value from Reconfigured: a consumer with an expensive
            // rebuild must not pay for it on every producer restart.
            std::printf("producer restarted (\"%s\"); layout unchanged\n", rd.deviceName());
            continue;
        }
        if (w == WaitResult::Closed) break;

        // Drain. Wakes coalesce -- the event is auto-reset, so one wake does
        // not mean one block once the consumer falls behind.
        while (c.next(b)) {
            ++blocks;
            if (blocks % 100 == 0) {
                std::printf("block %llu  seq=%llu  n=%u  flags=0x%x  silent=0x%08x  dropped=%llu\n",
                            static_cast<unsigned long long>(blocks),
                            static_cast<unsigned long long>(b.seq), b.sampleCount, b.flags,
                            b.silentChannelMask,
                            static_cast<unsigned long long>(rd.droppedBlocks()));
            }
        }

        // The whole of crash detection from the consumer side, in one line.
        // Quiet (engine up, this device not driven) and Stalled (Active but the
        // timestamp went cold) both answer "no blocks"; telling them apart is
        // the entire reason deviceState exists.
        if (c.state() == StreamState::Stalled)
            std::printf("WARNING: irTactile appears wedged (no publish in %.0f ms)\n",
                        rd.staleAfterMs());
    }

    std::printf("producer closed. %llu blocks, %llu dropped.\n",
                static_cast<unsigned long long>(blocks),
                static_cast<unsigned long long>(rd.droppedBlocks()));
    return 0;
}
