# C# binding

The C# binding for the irTactile shared-memory contracts. One assembly,
`IrTactile.Shm`. **Fully managed** - `MemoryMappedFile`, `EventWaitHandle`,
`Volatile.Read` / `Volatile.Write` for the acquire/release fields. No native
dependency, no P/Invoke.

| Namespace | Content |
|---|---|
| `IrTactile.Shm.Stream` | [stream-shm v1](../../spec/stream-shm/v1/README.md) consumer: `Reader`, `Client`. Reader-side only; the segment is mapped read-only. |
| `IrTactile.Shm.Telemetry` | [telemetry-shm v1](../../spec/telemetry-shm/v1/README.md) producer: `TelemetryProducer`, `TelemetrySegment`. See *telemetry-shm: your first producer*. |
| `IrTactile.Shm.Channels` | The [channel model](../../spec/channels/README.md): `ChannelModel` (kinds, flags, name rules) and the generated `std.*` table `Std`. |

Most of this guide is the stream consumer; the telemetry producer is at the end.

## Install

There is **no NuGet package yet** - `IrTactile.Shm` is not published. Two paths
today:

- **Source, via submodule.** Reference the one project directly. The `.slnx` in
  this directory is a development solution - it builds the samples and the test
  suite and plays no part in consuming the binding:
  ```xml
  <ProjectReference Include="third_party\irtactile-shm\bindings\csharp\src\IrTactile.Shm\IrTactile.Shm.csproj" />
  ```
- **Binary drop**, for hosts with no package restore: `IrTactile.Shm.dll` plus
  `IrTactile.Shm.xml` (the doc comments) from a release, referenced directly.

Nothing under `samples/` or `tests/` reaches a consumer by either. A package is
planned - the project already carries its metadata and `dotnet pack` produces it
(see *Building*) - and `dotnet add package IrTactile.Shm` becomes the third path
when it ships.

Targets **`net8.0`** and **`netstandard2.0`**. The second exists so that the tools
most likely to want this - sim plugins on .NET Framework 4.6.1+, Unity, anything
not yet on .NET 8 - are not shut out; it pulls `System.Memory` for the
`ReadOnlySpan<float>` on the consumer surface. On `net8.0` there are no
dependencies at all.

## Platform

**64-bit hosts only**, and Windows for `Client` (the segment and the event are
named kernel objects; `Reader` itself is portable and runs over any mapped
buffer). The bitness is a real constraint, not caution:
[`spec/common/concurrency.md`](../../spec/common/concurrency.md)
requires an 8-byte atomic load for `writeSeq`, `lastPublishQpc` and `desc.seq` on a
32-bit consumer, and names `Interlocked.Read` - which **cannot** be used here, because
on x86 it lowers to `lock cmpxchg8b`, a read-modify-write that access-violates
against the read-only view this binding maps. `Volatile.Read` is what remains,
and its 8-byte atomicity on x86 is a property of the hardware rather than a
documented .NET guarantee. On x64 the loads are tear-free by construction and
the question does not arise. irTactile ships no 32-bit producer, so the
supported configuration is x64 on both sides.

The assembly is built AnyCPU, so nothing stops it loading into a 32-bit host -
it is simply not a configuration this binding claims to be correct in. The
`netstandard2.0` target makes that worth saying twice: a .NET Framework host
still defaults to 32-bit in more places than a .NET 8 one does, and neither the
package nor the runtime will object. Check `Environment.Is64BitProcess` if your
host's bitness is not yours to decide.

## Building

The solution is an `.slnx`, which needs the **.NET SDK 9.0.200 or newer**, even
though the library itself targets `net8.0` and `netstandard2.0`. Building the
library project directly (`dotnet build src/IrTactile.Shm`) works on any .NET 8
SDK, and `dotnet pack src/IrTactile.Shm -c Release` produces the package plus its
`.snupkg` without the samples or the tests being involved.

| Source | Content |
|---|---|
| `src/IrTactile.Shm/Stream/StreamLayout.cs` | The wire format: constants, field offsets, size arithmetic, addressing. What the spec's field tables look like as code. |
| `src/IrTactile.Shm/Stream/ShmStreamHeader.cs` | The wire structs, `LayoutKind.Explicit`, offsets pinned by tests. |
| `src/IrTactile.Shm/Stream/Reader.cs` | `Reader` / `BlockView` / `StreamRequest` / `AttachResult` / `StreamState`. Portable. |
| `src/IrTactile.Shm/Stream/Client.cs` | Mapping, event, blocking `Wait()`, liveness. |
| `samples/StreamReader/Program.cs` | The whole thing via `Client`. |
| `src/IrTactile.Shm/Telemetry/TelemetryProducer.cs` | telemetry-shm: `TelemetryProducer`, `TelemetrySegment`, layout constants. |
| `src/IrTactile.Shm/Channels/` | `ChannelModel.cs` (hand-written) and `StdChannels.g.cs` (generated). |
| `samples/TelemetryDemoProducer/Program.cs` | A complete telemetry producer. |

## The four-line consumer

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
        Render(b);
```

Every normative rule in the spec is already obeyed by that loop. A consumer
that also wants crash detection adds one line:

```csharp
if (c.State() == StreamState.Stalled) Warn();
```

## Variations

- **Latest-value bridge, polling on its own cadence:** swap `Next` for
  `Latest` and pass `null` as the event name; nothing else changes. Polling
  is first-class, not a fallback - with no event, `Client.Wait` polls
  `WriteSeq` at `period / 4`, clamped to [0.5 ms, 5 ms], against the same
  timeout. Consumer source is identical in both modes.
- **You own your frame loop** (engine plugin, overlay): `Client.NativeEvent`
  hands out the `EventWaitHandle` for `WaitHandle.WaitAny` alongside your own
  handles; `State()` and `Next` are unchanged.

The segment is opened read-only (`MemoryMappedFileRights.Read`, the managed
`FILE_MAP_READ`), so a consumer bug faults instead of corrupting the producer's
ring. The **event** is the one place this binding asks for more than the spec's
`SYNCHRONIZE`: .NET's in-box `EventWaitHandle.OpenExisting` requests
`EVENT_MODIFY_STATE | SYNCHRONIZE` and offers no narrower overload, and
narrowing it would mean taking a dependency on `System.Threading.AccessControl`
for a right the `Client` then never exercises. The only configuration this
costs is a producer sharing its event wait-only: the open is refused and the
`Client` polls instead - a latency difference, not a correctness one.
`EventFallback` reports that it happened and `EventFallbackReason` says which
of the two causes it was.

## Contract checking

`StreamRequest` compares counts and rates, not strings. `Mismatch` is
deliberately **not retryable**: the segment is valid and the producer healthy,
so waiting changes nothing. `Reader.MismatchDetail` names the disagreeing
fields - it survives `Open` returning `Mismatch` precisely so the failure can
be reported with an actionable remedy.

Assert what would break you, not everything you know. The loop above drains
with `while (Next(b))` and handles any block size it is given, so it leaves
`SamplesPerBlock` at 0 - pinning it would turn the operator's next latency
tweak into a hard `Mismatch` at open. Pin it only when the processing
downstream is genuinely fixed-size (an FFT, a fixed hop). To confirm the
cadence rather than assert it, attach without pinning and read `PublishHz`:
that reports the update rate the producer is actually running, which beats
asserting a block size derived from a figure read off the configuration
screen.

## Derived timing

Never compute timeouts yourself and never hardcode a period:

| `Reader` member | Spec formula |
|---|---|
| `PeriodMs` | `samplesPerBlock × 1 000 000 / sampleRateMilliHz` |
| `PublishHz` | `(sampleRateMilliHz / 1000) / samplesPerBlock` - the update rate the operator configured |
| `WaitTimeoutMs` | `max(50 ms, 4 × periodMs)` |
| `StaleAfterMs` | `max(100 ms, 5 × periodMs)` |

`Client.Wait()` already waits with `WaitTimeoutMs`. The two acceptance
criteria the API is held to: the minimal consumer **multiplies nothing** and
**computes no timeouts**.

## Tests

```
dotnet test bindings/csharp
```

The suites mirror the C++ binding's:

| Suite | Mirrors | What it proves |
|---|---|---|
| `tests/IrTactile.Shm.Tests/Stream/LayoutTests.cs` | the header's `static_assert`s + size-arithmetic tests | Offset pinning via `Marshal.OffsetOf`/`Marshal.SizeOf` against `spec/stream-shm/v1/fields.md` - the guard against the silent-garbage class of error |
| `tests/IrTactile.Shm.Tests/Stream/StreamShmTests.cs` | `bindings/cpp/tests/stream/StreamShm_test.cpp`, case for case | The publish/read protocol over an in-memory segment |
| `tests/IrTactile.Shm.Tests/Conformance/` | `bindings/cpp/tests/conformance_runner.cpp` | The binding-independent vector suite: all of `conformance/stream-shm/v1/` decoded and compared |
| `tests/IrTactile.Shm.Tests/Telemetry/` | `bindings/cpp/tests/telemetry/` | Producer behaviour, plus the telemetry-shm `encode` vectors compared byte for byte |
| `tests/IrTactile.Shm.Tests/Channels/` | `bindings/cpp/tests/channels/` | Name rules and the generated `std.*` table |

The test project targets `net8.0` only, so the `netstandard2.0` assembly is
compiled (`dotnet build` builds both targets) but not exercised. The two targets
share every source file and differ only in `#if NET5_0_OR_GREATER` guards
(`Client.IsWindows`, the `SupportedOSPlatform` attributes) and the
`IsExternalInit.cs` polyfill, so the gap is narrow - but it is a gap, and closing
it means multi-targeting the test project to `net8.0;net48`.

## telemetry-shm: your first producer

`IrTactile.Shm.Telemetry.TelemetryProducer` / `TelemetrySegment` implement the producer side of the
[telemetry-shm v1 format](../../spec/telemetry-shm/v1/README.md) (Windows; the segment is a named file mapping).

```csharp
using IrTactile.Shm.Channels;
using IrTactile.Shm.Telemetry;

using var s = TelemetrySegment.Create(TelemetryLayout.InboundName("mysim"), 256,
                                      new ProducerIdentity { SimName = "My Sim" });
TelemetryProducer p = s.Producer;
// Table channels copy their unit, range and kind from the table.
int rpm  = p.Declare(Std.All[(int)StdChannel.EngineRpm], Provenance.Native);
int defl = p.DeclareArray(Std.Arrays[(int)StdArray.SuspensionDeflection], 4);   // 4 of 8 elements
p.SetWheelLayout(2, 2);                                                         // front to back
// A local channel; irTactile sees it as sim.<simId>.pump_pressure.
int pump = p.DeclareLocal("pump_pressure", "bar", 0, 10, SignalKind.Continuous);
p.CommitLayout();   // the segment becomes visible here
p.SetState(ProducerState.Active, TelemetryClock.Now);

while (running)
{
    p.BeginFrame();
    p.Set(rpm, 6500f);
    p.Set(pump, 4.2f);
    p.EndFrame(TelemetryClock.Now, TelemetryClock.Now);
}
```

`Create` throws `InvalidOperationException` when the segment cannot hold the layout or the `SimId` is invalid.
`PollReturn` tells you which channels irTactile accepted and which it demands, the same as the C++
`pollReturn()`. `samples/TelemetryDemoProducer` is the full example.

## License

MIT - see [LICENSE](../../LICENSE).
