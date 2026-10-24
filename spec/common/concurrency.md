# Concurrency classes and memory ordering

Every field of an irtactile shared-memory header belongs to exactly one of
three concurrency classes, or four in a format with a frame seqlock (see
*Frame fields*). The "Class" column of each format's `fields.md` (e.g.
[../stream-shm/v1/fields.md](../stream-shm/v1/fields.md)) is normative.

## Layout fields

Every field outside the concurrently-written byte range (`[80, 112)` in v1)
that is not **pinned** is a **layout** field. Layout fields are read under the
`layoutSeq` seqlock ([seqlock.md](seqlock.md)): take a snapshot while
`layoutSeq` is even and unchanged, then use that snapshot for all addressing
and interpretation until the next read cycle. A producer restart into the same
segment may rewrite any layout field; changing one invalidates every cached
snapshot.

Note what is deliberately a layout field: the **geometry** (`channelCount`,
`samplesPerBlock`, `ringBlocks`, `sampleFormat`, `slotBytes`,
`channelPlaneBytes`) and the **identity** (`deviceGuid`, `deviceName`,
`producerPid`, plus the clock calibration `qpcFrequency`). A restart that
renames the device, or a different producer process taking the segment over,
rewrites them - a consumer that cached `deviceName` at attach must re-read it
after any `layoutSeq` change.

## Pinned fields

A rewrite MUST NOT change `magic`, `versionMajor`, `headerBytes`, `totalBytes`
or `slotArrayOffset`. Consumers map once, blind, against those five and never
remap, and an OS segment cannot grow. Changing any of them requires a different
segment - which, since the size class is baked into the name, means a different
name. They are read once, at attach, and never again.

## Concurrent fields

Bytes `[80, 112)` are read directly with per-field acquire loads:
`writeSeq`, `lastPublishQpc`, `deviceState`, `layoutSeq`, `wiredChannelMask`,
`reserved1`. The seqlock does **not** cover them - that is not a restriction
on *when* they may be read but on *how*: each is a single naturally-aligned
word, individually coherent at any time, including while `layoutSeq` is odd.
Take their values from live loads only, never from a header snapshot (the
snapshot's bytes in this range may be mid-store garbage - see
[seqlock.md](seqlock.md)).

- `layoutSeq` is concurrent only because the seqlock word itself must be loaded
  outside the lock; the values it guards are layout fields.
- `wiredChannelMask` is a concurrent hint because it changes on every producer
  graph rebuild, which is not compatible with the snapshot model.
- All six words sit inside one cache line, so a reader loading `writeSeq` gets
  `layoutSeq` and `deviceState` for free.

## Frame fields

A format that guards a whole frame with a counter ([seqlock.md](seqlock.md),
*Generalization*) adds a fourth class: **frame** fields are rewritten on every
publish inside the frame seqlock window. A reader copies them inside one
window, validates the counter afterwards and uses the copy for that read only.
It never caches a frame field, including an offset, across reads. A field
that is rewritten on every publish is never pinned, even when its value rarely
changes.

## Why plain integers rather than `std::atomic`

`std::atomic<T>` has **no standard-guaranteed size, alignment or layout**, and
a Rust, C# or Python binding cannot see it at all - it sees bytes and hardcodes
an offset. The struct is a plain byte layout; the correct access discipline
lives in accessor code, and producer and reader use the same discipline.

Three facts a binding author must know - they make a non-C++ binding
straightforward and stop incorrect generalisation:

- **On x86-64, loads are acquire and stores are release in hardware (TSO).**
  The protocol needs no StoreLoad ordering, so on x64 the fences are compiler
  barriers only and a plain volatile read is correct. **This is not portable** -
  Windows on ARM64 requires real barriers.
- **64-bit fields must be naturally aligned** to be tear-free. The layout
  guarantees it, and it survives into the mapped view.
- **A 32-bit consumer process** must use an 8-byte atomic load intrinsic
  (`Interlocked.Read`, `cmpxchg8b`) for the 64-bit fields; a plain read can
  tear.
