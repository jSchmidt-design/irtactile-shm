# stream-shm v1 - field layout *(normative)*

The byte layout of the segment. Concurrency classes referenced below
(**pinned** / **layout** / **concurrent**) are defined in
[../../common/concurrency.md](../../common/concurrency.md); addressing formulas
that consume these fields are in [README.md - Addressing](README.md).

## Segment map

```
byte 0                              ShmStreamHeader          256 B, alignas(64)
     (= header.slotArrayOffset)     slot[0]              ─┐
                                    slot[1]               │ slotBytes apart
                                    ...                   │ ringBlocks entries
                                    slot[ringBlocks-1]   ─┘
     (= header.totalBytes)          end of segment

each slot, at slot_base:
     slot_base + 0                            ShmBlockDesc   64 B, alignas(64)
     slot_base + slotPayloadOffset            ch0 plane   ─┐
     slot_base + slotPayloadOffset +   1×CPB  ch1 plane    │ channelPlaneBytes (CPB)
     ...                                                   │ apart; channelCount
     slot_base + slotPayloadOffset + (N-1)×CPB chN-1 plane ─┘ planes

each plane, at plane_base:
     plane_base + 0                  sample 0   ─┐
     plane_base + 1×bytesPerSample   sample 1    │ samplesPerBlock samples,
     ...                                         │ contiguous, per channel
     plane_base + (S-1)×bytesPerSample sample S-1┘
```

The descriptor lives **inside** the slot, so descriptor plus payload is one
contiguous run: one base pointer per slot, one region to bounds-check. There is
no contiguous descriptor array - the slot is always computed from
`header.writeSeq`, never scanned.

Sample data is **planar per slot**: channel 0's samples contiguous, then
channel 1's. `channelLayout` exists so `Interleaved` remains addable without a
major-version break; v1 is always `Planar`.

**Trailing bytes.** `header.totalBytes` is the size-class size, normally larger
than the live footprint `align64(sizeof(ShmStreamHeader)) + ringBlocks × slotBytes`.
Bytes between the last slot and `totalBytes` are unused producer-zeroed
padding. Padding also exists inside `slotBytes` and in the fixed `reserved[]`
arrays.

## `ShmStreamHeader` - 256 bytes at offset 0

| Off | Size | Type | Field | Class | Meaning |
|---|---|---|---|---|---|
| 0 | 4 | u32 | `magic` | **pinned** | `kMagic` = `0x41535449` (`'ITSA'`). `0` ⇒ segment created, header not yet written |
| 4 | 2 | u16 | `versionMajor` | **pinned** | Reader must **reject** a mismatch |
| 6 | 2 | u16 | `versionMinor` | **layout** | Reader must **tolerate** a higher value |
| 8 | 4 | u32 | `headerBytes` | **pinned** | Size of this header. Never copy more than this |
| 12 | 4 | u32 | `totalBytes` | **pinned** | Size of the whole segment; the size class, not the live footprint |
| 16 | 4 | u32 | `sampleFormat` | **layout** | `SampleFormat`. Reject an unknown value |
| 20 | 4 | u32 | `channelLayout` | **layout** | `ChannelLayout`. Reject an unknown value |
| 24 | 4 | u32 | `channelCount` | **layout** | 1..32 |
| 28 | 4 | u32 | `sampleRateMilliHz` | **layout** | Operative rate ×1000; exact at every rate |
| 32 | 4 | u32 | `sourceSampleRate` | **layout** | Provenance: rate before decimation (Hz) |
| 36 | 4 | u32 | `rateDivisor` | **layout** | Provenance: decimation ratio, 1..1024 |
| 40 | 4 | u32 | `samplesPerBlock` | **layout** | Samples per channel in one block |
| 44 | 4 | u32 | `ringBlocks` | **layout** | Ring depth, in slots, 2..256 |
| 48 | 4 | u32 | `bytesPerSample` | **layout** | 4 (Float32) or 2 (Int16) |
| 52 | 4 | u32 | `slotPayloadOffset` | **layout** | Slot base → plane 0 |
| 56 | 4 | u32 | `channelArrayOffset` | **layout** | **`0` in v1 - no channel table** |
| 60 | 4 | u32 | `slotArrayOffset` | **pinned** | Segment base → `slot[0]` |
| 64 | 4 | u32 | `slotBytes` | **layout** | Slot to slot; multiple of 64 |
| 68 | 4 | u32 | `channelPlaneBytes` | **layout** | Plane to plane inside a slot |
| 72 | 4 | u32 | `diagEngineBlockSize` | **layout** | **Diagnostic only** - producer DSP block size. Unrelated to `samplesPerBlock` |
| 76 | 4 | u32 | `diagFramesPerPublish` | **layout** | **Diagnostic only** - producer render periods batched per publish. **Not a sample count**; that is `samplesPerBlock` |
| 80 | 8 | u64 | `writeSeq` | **concurrent** [acq/rel] | Blocks published. THE counter |
| 88 | 8 | u64 | `lastPublishQpc` | **concurrent** [acq/rel] | QPC at the last publish |
| 96 | 4 | u32 | `deviceState` | **concurrent** [acq/rel] | `DeviceState` |
| 100 | 4 | u32 | `layoutSeq` | **concurrent** [acq/rel] | Odd ⇒ header being rewritten |
| 104 | 4 | u32 | `wiredChannelMask` | **concurrent** [acq/rel] | Bit c ⇒ channel c has a source |
| 108 | 4 | u32 | `reserved1` | **concurrent** | Producer zeroes; reader ignores. Explicit padding that keeps the concurrent range at `[80, 112)` |
| 112 | 8 | u64 | `qpcFrequency` | **layout** | Ticks/second, for both QPC fields |
| 120 | 40 | char[40] | `deviceGuid` | **layout** | UTF-8, NUL-terminated |
| 160 | 64 | char[64] | `deviceName` | **layout** | UTF-8, NUL-terminated |
| 224 | 4 | u32 | `producerPid` | **layout** | Producer's OS process id; `0` ⇒ unknown. Diagnostics and zombie adjudication only - see below |
| 228 | 28 | u8[28] | `reserved[28]` | - | Producer zeroes; reader **ignores** |
| **256** | | | *(end)* | | |

Both strings are **UTF-8** and NUL-terminated; a producer must zero the unused
tail of each array so a byte-wise comparison of two headers is meaningful.

### `producerPid`

The process id of the producer that last (re)wrote the header, stored under
the same `layoutSeq` rewrite that stamps the identity strings. It answers one
question: *is the process that owns this segment still alive?* - which makes
zombie adjudication (a segment left behind by a crashed producer, kept alive
by a consumer's open handle) deterministic instead of purely staleness-based.

**PIDs are reused by every OS.** A liveness verdict may treat "PID no longer
names a running process" as proof of abandonment, but never the converse:
`producerPid` alive says nothing by itself, and it must always be combined
with the `deviceState` / `lastPublishQpc` staleness rules in
[../../common/liveness.md](../../common/liveness.md). It is not a substitute
for them, and no addressing or timing behaviour may ever derive from it.

## `ShmBlockDesc` - 64 bytes at the head of every slot

| Off | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 8 | u64 | `seq` | Block index, or `kSlotBusy` (`~0`) while being filled. `slot = seq % ringBlocks` [acq/rel] |
| 8 | 8 | u64 | `firstSampleIndex` | Stream-rate samples emitted before this block, **including any later discarded** |
| 16 | 8 | u64 | `timestampQpc` | QPC at publish. **The** per-block timing field |
| 24 | 4 | u32 | `flags` | `BlockFlags` |
| 28 | 4 | u32 | `silentChannelMask` | Bit c set ⇒ channel c is all-zero in this block |
| 32 | 4 | u32 | `layoutEpoch` | `layoutSeq` snapshot at publish; reject on mismatch with the reader's snapshot |
| 36 | 28 | u8[28] | `reserved[28]` | Producer zeroes; reader **ignores** |
| **64** | | | *(end)* | |

### Every published block is full

There is no per-block sample count, because there is nothing for one to say: a
block carries exactly `header.samplesPerBlock` samples per channel, always.

- **Producer invariant (normative):** a block is either published complete or
  **abandoned**, never published short. A producer that cannot finish one -
  shutdown, reconfigure - leaves the slot at `kSlotBusy` and charges the
  partial fill to the *next* published block's `firstSampleIndex`, so the loss
  reaches the consumer through `gap_samples`, measured in samples.
- **Consumer bound:** loop to `header.samplesPerBlock` from the seqlock
  snapshot. Validate it **once, at attach**, with the rest of the layout:
  `samplesPerBlock × bytesPerSample <= channelPlaneBytes`. Every block is then
  in bounds by construction, and `layoutEpoch` guarantees the payload was
  written under the geometry you validated. Do not take a bound from anywhere
  else.

A ragged tail cannot arise in the first place: the producer refuses any
configuration whose block boundary would not fall on one of its own render
boundaries, so there is never a leftover partial block to publish. That
arithmetic is a producer concern and reaches you only as an already-validated
`samplesPerBlock`.

### `firstSampleIndex`

The absolute stream-rate position of the block's first sample - the number of
samples emitted since the stream began, **counting ones later discarded**. Its
one-line use:

```
gap_samples = firstSampleIndex - (prev.firstSampleIndex + samplesPerBlock)
```

Zero when continuous; non-zero reports lost samples (ring overrun or an
abandoned partial block) in *samples*, not blocks. It pairs with
`BlockDiscontinuity`, which flags the second case without quantifying it.

It is **monotonic within one producer lifetime, not across a segment's**: a
producer restart into a segment you still hold keeps `writeSeq` running while
the sample index restarts at 0. `writeSeq` is the counter that always moves
forward.

## Enumerated values

Values, not just names - a binding cannot implement "reject an unknown value"
without them. All are `uint32_t`.

| Enum | Field | Values |
|---|---|---|
| `SampleFormat` | `sampleFormat` | `Float32` = 1, `Int16` = 2 |
| `ChannelLayout` | `channelLayout` | `Planar` = 1, `Interleaved` = 2 *(reserved; never produced by a v1.0 producer)* |
| `DeviceState` | `deviceState` | `Inactive` = 0, `Active` = 1, `Closed` = 2 |
| `BlockFlags` | `flags` | bitmask: `BlockSilence` = 1<<0 (every channel zero in this block), `BlockDiscontinuity` = 1<<1 (first block after start or reconfigure) |

`0` is not a valid `SampleFormat` or `ChannelLayout`, so a zero-filled segment
fails the enum check rather than decoding as something. `DeviceState::Inactive`
*is* 0, deliberately: a freshly created segment reads as "not being driven"
before the header is written.

## Representation, alignment and padding

Little-endian throughout, IEEE-754 for `float32`, two's complement for `int16`,
every field naturally aligned, **no packing anywhere, and no implicit padding**.
Every 64-bit field lands on a multiple of 8 by construction.

Both structs are `alignas(64)`, `slotArrayOffset` is a multiple of 64,
`slotBytes` is padded to a multiple of 64, and mapping the whole segment
returns a 64 KB-aligned base, so those guarantees survive into the mapped view.
`slotBytes` being cache-line-sized also means the writer's slot never shares a
cache line with the reader's.

**Plane alignment is weaker.** Plane 0 is always 64-aligned. Every plane after
it is aligned to whatever `channelPlaneBytes` divides by - as little as 4
bytes.

> **Planes are guaranteed `bytesPerSample`-aligned and nothing more.**

That is exactly enough for the typed view every binding builds (`const float*`,
Rust's `slice::from_raw_parts::<f32>` - a *safety* requirement there -,
`MemoryMarshal.Cast`, a NumPy view), and not enough for an aligned SIMD load.
Use unaligned loads. At one block per publish the difference is not measurable.

Because a typed view's soundness rests on it, the guarantee is **validated at
attach, not assumed**: every term of the plane address - `slotArrayOffset`,
`slotBytes`, `slotPayloadOffset`, `channelPlaneBytes` - must be a multiple of
`bytesPerSample`, and a header violating that is rejected as malformed (see
[README.md](README.md), *The reader protocol - Attach*, step 3). A binding
that builds typed views must perform the same four checks before its first
cast.
