# Changelog

## 1.2.0 - Unreleased

- `spec/telemetry-shm/v1/` - the telemetry-shm wire format: an inbound region the
  producer creates (header, descriptor table, one latest-value frame with a
  frame seqlock) and a return region irTactile creates (`accepted[]`,
  `demanded[]`, consumer liveness). Names `irTactile_Telemetry_<sourceId>` /
  `irTactile_TelemetryReturn_<sourceId>`; the producer reads its identity from
  `IRTACTILE_TELEMETRY_SOURCE` / `IRTACTILE_TELEMETRY_SECTION`.
- `conformance/telemetry-shm/v1/` - 45 vector cases (attach, frame, return region
  decode; byte-exact producer encode), emitted by `tools/mkvectors_telemetry`.
- `bindings/cpp` - `irtactile::telemetry-shm` (header-only): `Producer`, `Reader`,
  `ReturnWriter` / `ReturnReader`, Win32 sections (`ProducerSection`,
  `ConsumerSection`, `ReturnSection`); doctest suite and conformance runner;
  `examples/telemetry_demo_producer`.
- `bindings/csharp` - `TelemetryProducer` / `TelemetrySection`; the encode vectors
  must reproduce the C++ bytes; `samples/TelemetryDemoProducer`.

## 1.1.0 - Unreleased

- `spec/telemetry-shm/` - the channel model: `channel_model.md` (kinds, flags,
  name rules, append-only), `channels/core.json` and `channels/ext.json`
  (the normative `core.*` / `ext.*` tables, `ConnectionStatus`, `ext::Surface`);
  `ext.*` also carries `input.handbrake`, `vehicle.tc_active` and
  `tyre.pressure.{FL,FR,RL,RR}` (appended, ids of earlier channels unchanged).
  The telemetry-shm wire format itself is not part of this release.
- `tools/gen_channels/` - generates the tables into `bindings/cpp/.../telemetry/`
  and `bindings/csharp/.../Telemetry/`; the output is committed and CI fails if
  regeneration differs (`.github/workflows/channels.yml`).
- `bindings/cpp` - `irtactile::telemetry-model` (header-only):
  `channel_model.h`, `core_channels.h`, `ext_channels.h`, with a doctest suite.
- `bindings/csharp` - `IrTactile.Shm.Telemetry` (`ChannelModel`, `CoreChannels`,
  `ExtChannels`) with xunit tests.
- `spec/common/seqlock.md` generalized to a counter guarding a whole frame
  (`frameSeq`); `spec/common/versioning.md` gains the channel-model
  append-only rule.

## 1.0.0 - 2026-08-09

First public release. The repository publishes the irTactile shared-memory
contracts as a wire format, with independent bindings implementing them.

- `spec/stream-shm/v1/` - normative layout, field tables, format reference for
  the stream contract; `spec/common/` - the seqlock protocol, concurrency
  classes, versioning rules and the liveness model both contracts build on.
- `conformance/stream-shm/v1/` - the binding-independent vector suite: golden
  sections plus `manifest.json` (schema 2), covering attach validation, forward
  compatibility, ring overrun, slot recycling, the seqlock in both its even and
  odd states, lossless and latest-value reads, reconfigure classification, int16
  conversion and liveness. Every layout-validation rule gets its own vector, so
  a binding implementing only a subset fails the specific case it is missing.
  `tools/mkvectors/` regenerates the blobs and manifest from the producer side.
- `bindings/cpp/` - header-only C++17 binding (`irtactile::stream-shm`,
  `irtactile::shm-common`) under `irtactile/shm/`, with examples and the doctest
  format suite plus the reference conformance harness.
- `bindings/csharp/` - the managed binding (`IrTactile.Shm`, namespace
  `IrTactile.Shm.Stream`): `MemoryMappedFile`, `EventWaitHandle`, `Volatile.Read`
  for the acquire/release fields, explicit-layout wire structs - no native
  dependency, no P/Invoke. Reader-side only for the stream, `net8.0` and
  `netstandard2.0`, x64 only (see `bindings/csharp/README.md`, *Platform*).

The package version and the wire version are independent: this can go to 1.1.0
while the section it reads stays stream-shm v1
(see `spec/common/versioning.md`).
