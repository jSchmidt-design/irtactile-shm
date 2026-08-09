# Changelog

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
