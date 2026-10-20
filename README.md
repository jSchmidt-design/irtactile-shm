# irtactile-shm

The public shared-memory contracts of **[irTactile](https://irtactile.com/)** and
their language bindings. Third-party tools use them to exchange real-time data with
irTactile.

The product is a **wire format**, not a library: fixed byte layouts plus publish/read
protocols, with no link-time dependency on irTactile. That is what the spec pins
down, and what any binding is measured against.

Parsing those bytes yourself is not the intended path, though. The C++ and C# clients
in this repo already implement the spec, including the parts that are easy to get
subtly wrong, and using one is strongly recommended. Each is an independent
implementation checked against the same conformance vectors rather than against the
other, so the spec stays the authority either way. One exception: the telemetry-shm
*encode* vectors are the C++ producer's output, so they check the C# producer against
the C++ one; the expectations of every decode case are written from the spec.

**Platform.** The shared section and the event that signals new data are named Windows
kernel objects, so `Client`, which owns the mapping and the waiting, runs only on
Windows. `Reader` has no such tie: hand it a mapped buffer and it works anywhere, and
the C++ `reader_min.cpp` example builds on Linux to prove it. The same split holds for
telemetry-shm: `Producer` works on any buffer, the named sections (`win32.h`,
`TelemetrySection`) are Windows only. Both sides also need to
be 64-bit. The protocol depends on 8-byte atomic loads, which x64 gives you for free
and 32-bit cannot reliably guarantee, and irTactile ships no 32-bit producer in any
case.

## Contracts

| Contract | Spec | Status |
|---|---|---|
| **stream-shm** - irTactile publishes the per-device audio stream it drives; consumers read blocks of samples | [spec/stream-shm/v1](spec/stream-shm/v1/README.md) | v1.0 |
| **telemetry-shm** - a third-party bridge writes telemetry from a game irTactile has no native support for, and irTactile drives effects from it | [spec/telemetry-shm/v1](spec/telemetry-shm/v1/README.md) | v1.0 |

**What v1 promises.** The v1 layout is frozen. Minor versions are additive only -
new enum values, previously reserved space, a grown header - and a consumer written
against v1.0 keeps working across them. A major bump is a layout break, and a reader
must reject it. The rules a reader implements to earn that are in
[spec/common/versioning.md](spec/common/versioning.md).

Shared mechanics both contracts build on: [spec/common](spec/common) - the seqlock
protocol, concurrency classes, versioning rules, and the liveness model. The channel
model telemetry-shm carries (kinds, flags, name rules, the `std.*` table) is
transport-independent and lives in [spec/channels](spec/channels/README.md).

## The consumer loop

The same program in both languages. Every normative rule in the spec is already
obeyed by it.

```cpp
using namespace irtactile::shm::stream;

// What this build was written against; 0 ⇒ not checked.
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
        for (uint32_t ch = 0; ch < b.channelCount(); ++ch) {
            const float* s = b.channel(ch);   // b.sampleCount floats, contiguous
            for (uint32_t i = 0; i < b.sampleCount; ++i)
                render(ch, s[i]);
        }
```

```csharp
using IrTactile.Shm.Stream;

// What this build was written against. Any field left at 0 is not checked.
var expected = new StreamRequest { ChannelCount = 4, SampleRateMilliHz = 375_000 };

using var c = new Client();
OpenResult r;
while ((r = c.Open("MyBridge_256k", "MyBridge_Event_256k", expected)).ShouldRetry())
    Thread.Sleep(250);          // producer not up yet - the normal case

if (r == OpenResult.Mismatch)   // deployed configuration is not this build's
    return Fail($"stream: {c.Reader.MismatchDetail}");

var b = new BlockView();
while (c.Wait() != WaitResult.Closed)
    while (c.Next(b))
        for (int ch = 0; ch < b.ChannelCount; ++ch)
        {
            ReadOnlySpan<float> s = b.Channel(ch);   // b.SampleCount floats, contiguous
            for (int i = 0; i < s.Length; ++i)
                Render(ch, s[i]);
        }
```

The payload is **planar**: one channel's samples are contiguous, and `channel(ch)`
hands back that run directly. It is always `float`, converted on the way out when the
section carries int16, so a consumer never branches on `sampleFormat`. The block
carries the channel count of the layout it was decoded under, which is what makes the
loop above survive a reconfigure. `silentChannelMask` marks the channels that are
all-zero in this block, if skipping them is worth the branch.

Polling instead of the event, owning your own frame loop, crash detection: one line
each, in the guides below.

## Bindings

| | stream-shm v1 | telemetry-shm v1 | channel model |
|---|---|---|---|
| C++ ([bindings/cpp](bindings/cpp)) | read ✅, write ✅ | read ✅, write ✅ | ✅ |
| C# ([bindings/csharp](bindings/csharp)) | read ✅ | write ✅ | ✅ |

**read** is the full consumer API: `Reader` over any mapped buffer, `Client` for the
Windows mapping and event. **write** is the producer half of the contract - the wire
structs, the size arithmetic, the addressing helpers and the acquire/release
accessors a producer publishes through. There is deliberately no `Writer` class; see
the [C++ guide](bindings/cpp/README.md), *Producer side*, for why. For telemetry-shm the
roles flip: a third party writes (`Producer`, `TelemetryProducer`), irTactile reads; the
C++ `Reader` exists for tools that inspect a producer.

**C++** - see the [C++ guide](bindings/cpp/README.md). Header-only, zero
dependencies, C++17, `add_subdirectory()`-and-done (or point CMake at
`bindings/cpp/examples` for the standalone examples build). Its suite - the doctest
format tests plus the conformance vectors - runs standalone:
`cmake -S bindings/cpp/tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests`.

**C#** - see the [C# guide](bindings/csharp/README.md). One assembly,
`IrTactile.Shm`, fully managed (no native dependency, no P/Invoke), targeting
`net8.0` and `netstandard2.0`. Consume it as a `ProjectReference` to
`bindings/csharp/src/IrTactile.Shm`, or as a binary drop from a release; a NuGet
package is planned but **not yet published**. `dotnet test bindings/csharp` runs the
suite.

The format at a glance: [spec/stream-shm/v1/format.html](spec/stream-shm/v1/format.html),
a single-page field reference. GitHub serves it as source, so download it and open it
in a browser.

## Build, test, verify

The C++ binding is header-only; building means building the things that include it.
The C# binding is a regular .NET assembly. Everything below runs from the repo root
with no global state beyond the toolchain.

### Build

**C++ binding** - header-only, no build step of its own. Two ways to use it:
- `add_subdirectory(bindings/cpp)` from a CMake project that wants the
  `irtactile::shm` (umbrella), `irtactile::stream-shm`, `irtactile::telemetry-shm`,
  `irtactile::channels` and `irtactile::shm-common` targets.
- Standalone, just to compile the examples: `cmake -S bindings/cpp/examples -B build-examples && cmake --build build-examples`.

**C# binding** - `dotnet build bindings/csharp`. Targets `net8.0` and `netstandard2.0`.

**`tools/mkvectors`**, **`tools/mkvectors_telemetry`** - the generators that emit the
stream-shm and telemetry-shm conformance vectors (see *Verify* below). Only build them
if you intend to regenerate a suite:
`cmake -S tools/mkvectors -B build-mkvectors && cmake --build build-mkvectors`, and
`cmake -S tools/mkvectors_telemetry -B build-mkvt && cmake --build build-mkvt`.

**`tools/gen_channels`** - generates the `std.*` table bindings from
`spec/channels/std.json`: `uv run tools/gen_channels/gen_channels.py` (standard library
only; `--check` is what CI runs).

### Test

**C++ format suite** (doctest; publish protocol, addressing, reader forward-compatibility over a plain `std::vector<uint8_t>`):
```
cmake -S bindings/cpp/tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```
The build also produces the `conformance_runner` and `telemetry_conformance_runner`
targets - see *Verify*.

**C# suite** (xUnit; mirrors the C++ format tests, plus its own conformance runner):
```
dotnet test bindings/csharp
```

### Verify (conformance vectors)

`conformance/stream-shm/v1/` and `conformance/telemetry-shm/v1/` are the
binding-independent half of the test strategy
(see [conformance/README.md](conformance/README.md)): golden binary blobs plus a
`manifest.json` of expected results. Every binding's test suite runs the same
blobs against the same expectations. A binding that passes the suite here has
implemented the spec, not any other binding's headers.

The C++ suite runs the vectors via the `conformance_runner` and
`telemetry_conformance_runner` targets built in the *Test* step above;
`ctest --test-dir build-tests` invokes both. The C# suite runs the stream-shm vectors
via `IrTactile.Shm.Tests.Conformance` and replays the telemetry-shm encode vectors in
`TelemetryProducerTests`; `dotnet test` invokes both.

To regenerate the vectors - only when the spec or its producer-side encoding
changes - run the generator and inspect the diff:
```
./build-mkvectors/mkvectors conformance/stream-shm/v1
./build-mkvt/mkvectors_telemetry conformance/telemetry-shm/v1
git diff conformance/
```
The blobs are committed; any change is a spec change and belongs in its own
review.

## Repository layout

```
README.md          this file
CHANGELOG.md       release history
LICENSE            MIT
spec/              normative wire-format documentation (the deliverable)
bindings/          per-language implementations of the spec
conformance/       golden vectors every binding's test suite runs (conformance/README.md)
tools/             repo-local tooling - mkvectors / mkvectors_telemetry emit the
                   conformance vectors, gen_channels generates the std.* tables
```

## License

MIT - see [LICENSE](LICENSE).
