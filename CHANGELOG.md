# Changelog

## 1.3.0 - Unreleased

- Terminology: "section" is now "segment" (named shared memory; Windows: file mapping) in the bindings, spec and
  docs. The wire format and the shared memory names are unchanged. Renamed without aliases: C++ `ProducerSection`,
  `ConsumerSection`, `ReturnSection`, `SectionOpen`, `OpenResult::SectionFailed` (now `ProducerSegment`,
  `ConsumerSegment`, `ReturnSegment`, `SegmentOpen`, `SegmentFailed`); C# `TelemetrySection` (now
  `TelemetrySegment`); `irtp_probe --section` (now `--segment`).
- `std.*` table review before the first release: every entry gets a `description` (meaning, sign, zero point; emitted
  as `///` / `<summary>` by `gen_channels`, not on the wire); `spec/channels/README.md` gains "Axes and signs".
  `std.wheel.contact` and `std.wheel.surface_speed` added; `std.suspension.acceleration`, `std.engine.turbo_boost`
  and `std.tyre.temp` removed; `std.assist.abs_level` replaced by discrete `std.assist.abs_active`.
  The table is now 34 scalars, 17 wheel arrays and 1 axle array (174 handles).
- `bindings/cpp`, additive (no wire-format change):
  - `LocalChannel` + `Producer::declareLocal(const LocalChannel&)`: named fields instead of seven positional
    arguments (the positional overload stays).
  - `Producer::declareLocalArray()`: local arrays `name[i]` with an `ArrayLayout` (e.g. per-wheel values),
    all or nothing.
  - `SlotRange` + `Producer::declareArrayRange()`: every element slot of an array, not only element 0.
  - `Frame::isValid(slot)`; `kindOf` / `provenanceOf` / `layoutOf` typed views of a `ChannelDesc`.
  - `toCString()` for `OpenResult`, `ReadResult`, `ProducerState`, `SegmentOpen`.
  - `irtactile/shm/common/qpc.h`: `shm::qpcNow()` / `shm::qpcFrequency()` without the segment handling;
    `telemetry::qpcNow` / `qpcFrequency` remain as aliases.
- telemetry-shm: single-writer rule for the inbound region. A producer holds the named event `<inbound>#Writer`
  while it writes; if the event exists, another producer is live and the segment is left alone. C++:
  `ProducerSegment::create()` returns the new `OpenResult::WriterActive`; `writerLockNameFor()`. Before this, a
  second producer on the same name was taken for a restart and both wrote the same seqlocks.
- `bindings/cpp` fix: `Reader::readFrame()` and `ReturnReader::read()` copied into the caller's `Frame` /
  `ReturnView` before the seqlock check, so a `Torn` or `LayoutChanged` read (or a rejected return read) left a
  torn mix in it. Both now copy into internal scratch storage and swap it in only on success, so a failed read keeps
  the previous frame or view as documented. `ReturnReader::read()` is no longer `const`. The C# binding was not
  affected.
- telemetry-shm: consumer liveness for the return region. A consumer that crashed while `Active` left its demand
  masks in effect indefinitely. Now a producer treats an `Active` consumer as gone (every accepted channel demanded)
  when `lastConsumeQpc` is more than 1 s old or `consumerPid` names no running process, and a consumer that starts
  into an existing return region resets it to `Idle` with empty masks for `forLayoutSeq = 0` first. C++:
  `ReturnView::consumerGone`, `isConsumerStale()`, `kConsumerStaleMs`; `ReturnWriter::init()` does the reset.
  C#: `TelemetryReturnView.ConsumerGone` / `ConsumerPid`, `TelemetrySegment.IsConsumerStale()`.
- `bindings/cpp` fix: a failed `ProducerSegment::create()` on an existing instance (`WriterActive`, `SegmentFailed`
  or a rejected `Producer::open`) left the `Producer` bound to the unmapped previous view, so later calls wrote to
  invalid memory. `create()` now unbinds it first; new `Producer::close()` / `isOpen()`; `Producer::setState()` is a
  no-op and `pollReturn()` returns false while unbound.
- telemetry-shm fix: liveness and the layout check were applied only after a successful return read. A consumer that
  crashed mid-publication left `retSeq` odd, every read failed, and the producer kept the last view with
  `consumerGone == false` and its demand masks indefinitely. C++ `ProducerSegment::pollReturn()` and C#
  `TelemetrySegment.PollReturn()` now age the kept view on every poll once the region is open: `consumerGone` is
  re-evaluated from its `lastConsumeQpc` / `consumerPid`, and masks for another layout are cleared, even when the
  call returns false.

## 1.2.0 - Unreleased

- `spec/telemetry-shm/v1/` - the telemetry-shm wire format: an inbound region the
  producer creates (header, descriptor table, one latest-value frame with a
  frame seqlock) and a return region irTactile creates (`accepted[]`,
  `demanded[]`, consumer liveness). The producer ships with a fixed section name
  (recommended `irTactile_Telemetry_<simId>`), the consumer is configured with it
  and derives the return name from it. Descriptors carry
  `provenance` and the array `layout`; the header carries the vehicle's
  `wheelsPerAxle`. A table array may be declared with fewer elements than the
  table capacity (`1 <= arrayCapacity <= table capacity`).
  `producerState` changes on transitions only, never per frame. The return
  region's mask offsets are *frame* fields under `retSeq`; the new *frame*
  class is defined in `spec/common/concurrency.md`.
- `conformance/telemetry-shm/v1/` - 60 vector cases (attach, frame, return region
  decode; byte-exact producer encode), emitted by `tools/mkvectors_telemetry`.
- `bindings/cpp` - `irtactile::telemetry-shm` (header-only): `Producer`, `Reader`,
  `ReturnWriter` / `ReturnReader`, Win32 sections (`ProducerSection`,
  `ConsumerSection`, `ReturnSection`); doctest suite and conformance runner;
  `examples/telemetry_demo_producer`.
- `bindings/csharp` - `TelemetryProducer` / `TelemetrySection`; the encode vectors
  must reproduce the C++ bytes; `samples/TelemetryDemoProducer`.

## 1.1.0 - Unreleased

- `spec/channels/` - the transport-independent channel model: `README.md` (kinds, flags,
  array layouts, provenance, name rules, append-only) and `std.json`
  (the normative `std.*` table: 35 scalars, 17 wheel arrays of capacity 8 and one
  axle array of capacity 4, `std::ConnectionStatus`, `std::Surface`). Every
  `std.*` channel is `OPTIONAL`.
  The telemetry-shm wire format itself is not part of this release.
- `tools/gen_channels/` - generates the tables into `bindings/cpp/.../shm/channels/`
  and `bindings/csharp/.../Channels/`; the output is committed and CI fails if
  regeneration differs (`.github/workflows/channels.yml`).
- `bindings/cpp` - `irtactile::channels` (header-only): `irtactile/shm/channels/model.h`,
  `irtactile/shm/channels/std.h` (umbrella `irtactile/shm/channels.h`; namespace
  `irtactile::shm::channels`, table in `irtactile::shm::channels::standard`), with a doctest suite.
- `bindings/csharp` - `IrTactile.Shm.Channels` (`ChannelModel`, `Std`) with xunit tests.
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
