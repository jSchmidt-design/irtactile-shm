# C++ consumer API

The C++ binding for the [stream-shm v1 format](../../spec/stream-shm/v1/README.md).
Header-only, zero dependencies, namespace `irtactile::shm::stream`. Namespaced
because a public header must not drop `ShmStreamHeader` into the global
namespace of somebody else's project.

The same tree carries the [telemetry-shm v1](../../spec/telemetry-shm/v1/README.md)
binding (`irtactile/shm/telemetry/*`, see *telemetry-shm: your first producer* at
the end) and the [channel model](../../spec/channels/README.md) it carries:

| Header | Content |
|---|---|
| `irtactile/shm/channels/model.h` | Kinds, flags, array layouts, provenance, `isValidChannelName()`. Hand-written. |
| `irtactile/shm/channels/std.h` | The `std.*` table, namespace `irtactile::shm::channels::standard`. Generated from `spec/channels/std.json`. |
| `irtactile/shm/channels.h` | Umbrella for the two above. Target `irtactile::channels`. |

The stream-shm headers:

| Header | Content |
|---|---|
| `irtactile/shm/stream/layout.h` | The wire format: structs, constants, size arithmetic, addressing. Constexpr only, zero behaviour. What a non-C++ binding is generated from. **Both sides** - see *Producer side*. |
| `irtactile/shm/common/atomic.h` | The one correct way to touch the six concurrently-written fields. **Both sides.** |
| `irtactile/shm/stream/reader.h` | `Reader` / `BlockView` / `StreamRequest` / `AttachResult` / `StreamState`. Portable. |
| `irtactile/shm/stream.h` | Umbrella for the three above. C++17, no `windows.h`, compiles anywhere. |
| `irtactile/shm/stream/win32.h` | `Client`: mapping, event, blocking `wait()`, liveness. Windows only, `#include`-gated. |
| `examples/stream/reader_min.cpp` | The portable half, self-contained, builds on Linux. |
| `examples/stream/reader_win32.cpp` | The whole thing via `Client`, ~40 lines. |

## The four-line consumer

```cpp
using namespace irtactile::shm::stream;

// What this build was written against. Field order: channelCount,
// sampleRateMilliHz, samplesPerBlock, sampleFormat - 0 ⇒ not checked.
// (C++20 callers can write designated initializers instead; the headers
// themselves require only C++17.)
constexpr StreamRequest kExpected{ /*channelCount*/ 4, /*sampleRateMilliHz*/ 375'000 };

Client c;
OpenResult r;
while (shouldRetry(r = c.open(L"MyBridge_256k", L"MyBridge_Event_256k", &kExpected)))
    sleep_for(250ms);           // producer not up yet - the normal case

if (r == OpenResult::Mismatch)  // deployed configuration is not this build's
    return fail("stream: %s", c.reader().mismatchDetail());

BlockView b;
while (c.wait() != WaitResult::Closed)
    while (c.next(b))
        render(b);
```

Every normative rule in the spec is already obeyed by that loop. A consumer
that also wants crash detection adds one line:

```cpp
if (c.state() == StreamState::Stalled) warn();
```

## Variations

- **Latest-value bridge, polling on its own cadence:** swap `next()` for
  `latest()` and pass `nullptr` as the event name; nothing else changes.
  Polling is first-class, not a fallback - with no event, `Client::wait()`
  polls `writeSeq` at `period / 4`, clamped to [0.5 ms, 5 ms], against the
  same timeout. Consumer source is byte-identical in both modes.
- **You own your frame loop** (engine plugin, overlay): `Client::nativeEvent()`
  hands out the `HANDLE` for `WaitForMultipleObjects` alongside your own quit
  handle; `state()` and `next()` are unchanged.

## Contract checking

`StreamRequest` compares counts and rates, not strings. `Mismatch` is
deliberately **not retryable**: the section is valid and the producer healthy,
so waiting changes nothing. `mismatchDetail()` names the disagreeing fields -
it survives `open()` returning `Mismatch` precisely so the failure can be
reported with an actionable remedy.

Assert what would break you, not everything you know. The loop above drains
with `while (next(b))` and handles any block size it is given, so it leaves
`samplesPerBlock` at 0 - pinning it would turn the operator's next latency
tweak into a hard `Mismatch` at open. Pin it only when the processing
downstream is genuinely fixed-size (an FFT, a fixed hop). To confirm the
cadence rather than assert it, attach without pinning and read `publishHz()`:
that reports the update rate the producer is actually running, which beats
asserting a block size derived from a figure read off the configuration
screen.

## Derived timing

Never compute timeouts yourself and never hardcode a period:

| `Reader` method | Spec formula |
|---|---|
| `periodMs()` | `samplesPerBlock × 1 000 000 / sampleRateMilliHz` |
| `publishHz()` | `(sampleRateMilliHz / 1000) / samplesPerBlock` - the update rate the operator configured |
| `waitTimeoutMs()` | `max(50 ms, 4 × periodMs)` |
| `staleAfterMs()` | `max(100 ms, 5 × periodMs)` |

`Client::wait()` already waits with `waitTimeoutMs()`. The two acceptance
criteria the API is held to: the minimal consumer **multiplies nothing** and
**computes no timeouts**.

## Producer side

Everything above is the consumer API. A producer uses **`layout.h` and
`common/atomic.h`, and nothing else** - no `Reader`, no `Client`, and no
`Writer`, because there is none. That omission is deliberate: a producer owns
its own section lifetime, its own threading and its own publish cadence, so a
writer class would either wrap one `memcpy` or dictate a design its host has
already made. What the binding owes the producer is the wire format and the
memory ordering, and those are exactly the two headers.

| Producer needs | `layout.h` / `atomic.h` provides |
|---|---|
| Validate the configuration | `kMinChannels` / `kMaxChannels`, `kMinRingBlocks` / `kMaxRingBlocks`, `kWarnRingSeconds`, `rateDivisorFromHz()` |
| Size the section | `LayoutParams`, `computeTotalSize()`, `smallestSizeClassFor()`, `sizeClassFromString()`, `kSizeClassBytes` / `kSizeClassSuffix` |
| Fill the header | `ShmStreamHeader`, `kMagic`, `kVersionMajor` / `kVersionMinor`, `bytesPerSampleOf()`, `slotPayloadOffsetOf()`, `slotArrayOffsetOf()`, `slotBytesOf()`, `channelPlaneBytesOf()` |
| Address a slot and its planes | `slotAt()`, `planeAt()` (non-const overloads) |
| Encode samples | `floatToInt16()` |
| Publish, and adjudicate a zombie section | `ShmBlockDesc`, `kSlotBusy`, `staleAfterMsOf()`, `detail::releaseStore()` / `detail::acquireLoad()` |

The publish protocol those pieces implement - claim the slot with `kSlotBusy`,
fill it, release-store the real `seq`, then bump `writeSeq` - is normative and
lives in [the spec](../../spec/stream-shm/v1/README.md); the header table is in
[fields.md](../../spec/stream-shm/v1/fields.md), and the seqlock rules for
rewriting the header in place are in
[spec/common/seqlock.md](../../spec/common/seqlock.md).

For a worked producer that is not irTactile, read
[`tools/mkvectors/mkvectors.cpp`](../../tools/mkvectors/mkvectors.cpp): it
builds every conformance vector from `layout.h`'s arithmetic alone, and
deliberately never includes `reader.h`, so a reader bug cannot bless itself.

## telemetry-shm: your first producer

The C++ binding for the [telemetry-shm v1 format](../../spec/telemetry-shm/v1/README.md):
you publish telemetry, irTactile reads it as a source. Header-only, namespace
`irtactile::shm::telemetry`, target `irtactile::telemetry-shm`.

| Header | Content |
|---|---|
| `irtactile/shm/telemetry/layout.h` | Wire structs, offsets, validation, section names. |
| `irtactile/shm/telemetry/producer.h` | `Producer`: `declare`, `commitLayout`, `beginFrame / set / event / endFrame`, `setState`. Portable (works on any aligned buffer). |
| `irtactile/shm/telemetry/reader.h` | `Reader` (consumer), `ReturnReader` (producer reads the masks), `ReturnWriter` (irTactile). Portable. |
| `irtactile/shm/telemetry/win32.h` | `ProducerSection`, `ConsumerSection`, `ReturnSection`, QPC, `sectionNameFromEnvironment()`. Windows only. |
| `examples/telemetry_demo_producer` | The whole thing in ~100 lines. |

```cpp
using namespace irtactile::shm::telemetry;
namespace channels = irtactile::shm::channels;

ProducerSection s;
s.create(sectionNameFromEnvironment("mysim"), /*capacity*/ 256, Identity{/*simId*/ "", "My Sim"});
Producer& p = s.producer();

int rpm  = p.declare(channels::standard::Id::EngineRpm, channels::Provenance::Native);   // properties come from the table
int defl = p.declareArray(channels::standard::ArrayId::SuspensionDeflection, 4);     // 4 of the table's 8 elements
p.setWheelLayout({2, 2});                                            // wheels per axle, front to back
int pump = p.declareLocal("pump_pressure", "bar", 0, 10, channels::Kind::Continuous);  // -> sim.<sourceId>.pump_pressure
p.commitLayout();                                                    // the section becomes visible here
p.setState(ProducerState::Active, qpcNow());

for (;;) {
    p.beginFrame();
    p.set(rpm, 6500.f);
    p.set(pump, 4.2f);
    p.endFrame(/*sample*/ qpcNow(), /*publish*/ qpcNow());
}
```

- Declare every channel before `commitLayout()`; changing the set later is another `commitLayout()` (a layout
  rewrite). `setCar()` after the commit is one too.
- Use `Inactive` while the sim is not running; keep calling `setState(Inactive, qpcNow())` as a heartbeat.
- `ProducerSection::pollReturn()` shows which channels of the current layout irTactile `accepted` (name and
  properties matched the table) and `demanded` (something consumes them). Skipping undemanded channels is optional.
- `tests/telemetry_conformance_runner` runs the vectors in `conformance/telemetry-shm/v1`.
