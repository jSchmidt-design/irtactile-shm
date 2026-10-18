# Conformance vectors

With three hand-written bindings, tests that live inside any single binding are that
binding's opinion. This directory is the opinion-free half of the test strategy:
golden binary
sections plus a JSON manifest of expected results, emitted by
[`tools/mkvectors`](../tools/mkvectors) from
the *producer* side and committed to the repo. Every binding's test suite decodes the
same blobs and compares against the same expectations.

A binding that passes the suite here has implemented the spec, not the C++ headers.

## Layout

```
conformance/
└─ <api>/v<N>/
   ├─ decode/            # golden sections → expected results (the binding READS)
   ├─ encode/            # scripted writes → expected bytes  (the binding WRITES)
   └─ manifest.json      # the suite: cases, steps, expectations
```

Direction is per API and, for bidirectional APIs, per region. stream-shm flows one
way, so it has `decode/` only. telemetry-shm's **inbound** region (third party writes)
needs `encode/` vectors - byte-identical output catches padding and endianness
mistakes a decode test cannot - and its **return** region `decode/` again. Both ship in
`conformance/telemetry-shm/v1` (manifest schema 1, its own op set: `attach`, `frame`,
`return`, `encode`; emitted by `tools/mkvectors_telemetry`).

## How a binding runs the suite

First, check `schema`. A runner must **refuse** a manifest whose schema it does not
implement rather than run it: a later schema may change the meaning of an op under a
name the old runner still recognises. This document describes schema 2.

> **Schema 2** adds the `markHeader` and `reconfigureSince` ops (below) and
> changes nothing else; every schema 1 case is a valid schema 2 case. The bump
> exists because a schema 1 runner meeting those ops would fail the case rather
> than mis-execute it, which is the correct outcome but a confusing one to
> debug - a refused manifest says what is actually wrong.
>
> **Schema bumps are additive only.** A later schema is a strict superset of the
> previous one: it may add ops, add optional fields, or rename an expectation
> key, and old cases remain valid against the new schema. A schema that needed
> to remove or repurpose an op would land as a new suite (`stream-shm/v2/`) with
> its own manifest, not a higher schema number on this one. A runner therefore
> only needs to recognise one schema - the oldest one it implements - to
> back-stop the rest.

Then, for each case in `manifest.json`:

1. Load `blob` (paths are relative to the manifest's directory) into a byte buffer.
   The buffer *is* the mapped section: `mappedBytes = blob length`. A blob that is
   missing, empty or short **fails the case** - it must never be treated as a case
   with nothing to do, which reports as a pass.
2. Execute `steps` in order against a fresh reader instance over that buffer.
3. Any failed expectation fails the case. An op or expectation key the binding
   does not recognise **fails the case** - silently skipping an assertion is how a
   suite goes green over a missing feature. Absent optional keys are not the same
   thing: a manifest that omits `flags` is not asking about `flags`.

The reference harness is [`bindings/cpp/tests/conformance_runner.cpp`](../bindings/cpp/tests/conformance_runner.cpp);
its op dispatch is the normative reading of the table below.

### Ops

| Op | Arguments | Semantics |
|---|---|---|
| `attach` | `expect`, optional `request` | Attach to the buffer. `expect.result` is one of `Ok` / `NotReady` / `BadVersion` / `Malformed` / `Mismatch`; `expect.retry` is whether a consumer may wait and try again (only `NotReady`). On `Ok`, optional `expect.fields` asserts observable reader state (same keys as `header` below, plus `sampleFormat`, `writeSeq`). On `Mismatch`, `expect.mismatchDetailContains` asserts the diagnostic names the disagreeing field. `request` is the structural assertion object (`channelCount`, `sampleRateMilliHz`, `samplesPerBlock`, `sampleFormat`; absent or 0 = don't check). |
| `patch` | `blob` | The producer advanced (or rewrote) the section: replace the **whole** buffer with the named blob. Sizes always match - a patch is a rewrite, never a remap. This is what makes seqlock rewrites, slot recycling and liveness changes expressible without threads. |
| `rewindToOldest` | - | Move the cursor to the oldest block the ring still holds. |
| `next` | `block` *or* `"expect": "end"` | Lossless read: either a block matching `block` is delivered, or the reader reports it is caught up. |
| `latest` | `block` *or* `"expect": "end"` | Latest-value-wins read, same expectation shape. Lossy **by contract**: the blocks it steps over are not gaps, so they must not reach `droppedBlocks`. |
| `droppedBlocks` | `expect` | The reader's dropped-block counter equals `expect`. |
| `writeSeq` | `expect` | The live `writeSeq` equals `expect`. |
| `attached` | `expect` | The reader's attached flag equals `expect` (false after a hostile rewrite detaches it). |
| `refreshSnapshot` | `expect` | Re-read `layoutSeq` and re-snapshot if it moved; result equals `expect` (false = validation failed and the reader detached). |
| `reconfigureKind` | `expect` | The kind recorded by the most recent refresh: `None` / `Geometry` / `Cosmetic`. **Describes that refresh, not "since you last asked"** - see `reconfigureSince`. |
| `markHeader` | - | Copy the reader's current header snapshot and hold it as a watermark, as a consumer tracking reconfigures for itself must. One watermark per case; a second `markHeader` replaces it. |
| `reconfigureSince` | `expect` | Classify the reader's current header snapshot against the `markHeader` watermark: `Geometry` / `Cosmetic`. Fails the case if no `markHeader` preceded it. This is the *only* reliable way to ask "what changed since I last looked": a read op refreshes the snapshot internally on every cycle, so a rewrite landing mid-drain is consumed before the caller can ask, and `reconfigureKind` then reads back `None`. A binding whose wrapper API classifies reconfigures from `reconfigureKind` will report a geometry change as a cosmetic restart. |
| `header` | `expect` | Assert current (re-snapshotted) header state. Keys: `channelCount`, `samplesPerBlock`, `ringBlocks`, `sampleRateMilliHz`, `rateDivisor`, `layoutSeq`, `slotPayloadOffset`, `slotBytes`, `channelPlaneBytes`, `deviceName`. |
| `state` | `nowQpc`, `expect` | Liveness at a synthetic clock reading: `Live` / `Quiet` / `Stalled` / `Closed`. |

### Block expectations

```json
"block": {
  "seq": 1, "firstSampleIndex": 8, "timestampQpc": 2000,
  "flags": 0, "silentChannelMask": 0,
  "channelCount": 4, "sampleCount": 8,
  "samples": {
    "tolerance": 0,
    "hash": "0x…",
    "spot": [[0, 0, 8.0], [0, 7, 15.0], [1, 0, 1008.0], [3, 7, 3015.0]]
  }
}
```

- Descriptor fields are compared exactly.
- `spot` entries are `[channel, sampleIndex, expectedFloat]`, compared with
  `|got − want| ≤ tolerance`.
- `hash` is the compact full-payload check: **FNV-1a 64** (offset basis
  `14695981039346656037`, prime `1099511628211`) over the little-endian IEEE-754
  bit patterns of the decoded float32 samples, **planar channel-major** (all of
  channel 0's samples, then channel 1, …). It is present exactly when the decode
  is bit-defined - float32 payloads. int16 decodes are tolerance-defined
  (`v × 1/32767` vs `v ÷ 32767` may differ by an ulp), so int16 cases carry no
  hash and check every sample via `spot` instead.

## Regenerating

The blobs and manifest are **generated, committed, and never hand-edited**:

```
cmake -S tools/mkvectors -B build-mkvectors && cmake --build build-mkvectors
./build-mkvectors/mkvectors conformance/stream-shm/v1
git diff --exit-code -- conformance     # the drift check
```

New cases are added in `tools/mkvectors/mkvectors.cpp`; expected values are computed
from what the generator wrote, never by running a reader - a binding bug must not be
able to bless itself.

## Coverage map - the test list

The suite is the format test list from
[`bindings/cpp/tests/stream/StreamShm_test.cpp`](../bindings/cpp/tests/stream/StreamShm_test.cpp),
turned into format assertions. Where v1 differs from the list's wording, the case
notes say so.

| Test case | Vector(s) |
|---|---|
| zero-filled section is NotReady and retryable | `attach.zero_filled` |
| wrong magic is Malformed and fatal | `attach.bad_magic` |
| higher versionMajor is BadVersion / higher versionMinor tolerated | `attach.higher_version_major` / `attach.higher_version_minor` |
| unknown sampleFormat / channelLayout rejected, not defaulted | `attach.unknown_sample_format`, `attach.unknown_channel_layout`, `attach.reserved_channel_layout` |
| an offset past mappedBytes is Malformed | `attach.offset_past_mapped` |
| non-zero reserved[] bytes tolerated | `attach.nonzero_reserved_tolerated` |
| cursor starts at writeSeq, not zero | `read.cursor_starts_at_write_seq` |
| hostile descriptor count bounded by the channel plane | `attach.hostile_samples_per_block`, `read.reconfigure_detach` - v1 has **no per-block count**; the bound is the header's `samplesPerBlock`, validated once at attach and on every re-snapshot |
| overrun boundary at ringBlocks=2 | `read.overrun_ring2_one_past_cursor`, `read.overrun_ring2_two_past_cursor` |
| slot recycling mid-construction at ring 2 | `read.slot_busy_ring2` |
| abandoned block: firstSampleIndex advances by the samples discarded | `read.abandoned_block` |
| layoutSeq seqlock rewrite picked up | `read.seqlock_geometry`, `read.seqlock_cosmetic` (+ `read.reconfigure_detach`, `read.layout_epoch_mismatch`) |
| inflated slotPayloadOffset / slotBytes honored | `read.inflated_offsets_honored` (also pads `channelPlaneBytes`, forcing the strided plane path) |
| int16 round-trip within 1 LSB, clamps not wraps | `read.int16_round_trip` |

Plus the forward-compatibility and protocol cases the C++ suite grew after the
proposal was written: `attach.grown_header_tolerated`, `attach.request_mismatch`,
`read.float32_blocks` (the sample-exact happy path every other case assumes),
`read.latest_wins` (the lossy read, and the drop counter it must *not* touch), and
`read.liveness_states` (the staleness model over a synthetic clock).

### The odd half of the seqlock

`read.seqlock_geometry` moves `layoutSeq` from one even value to another, which is
the *outcome* of a rewrite. The protocol's actual protection is the odd state in
between, and it gets its own two cases - one on each side of attach:

| Case | What a binding without the odd check does instead |
|---|---|
| `attach.rewrite_in_progress` | Reads the torn header and answers `Malformed` - fatal - where the correct answer is `NotReady`, and the completed rewrite then attaches cleanly |
| `read.seqlock_retry_bound` | Snapshots a header that is mid-edit, or spins forever waiting for a rewrite that never completes. A correct reader retries a bounded number of times, then detaches |

### Layout validation

`spec/common/seqlock.md` and `fields.md` describe a header whose offsets must be
*read*, never recomputed. The other side of that is that a read offset must be
checked, and the `attach.*` Malformed family covers each rule separately:

`header_bytes_below_minimum`, `channel_count_out_of_range`, `ring_blocks_below_minimum`,
`zero_samples_per_block`, `zero_sample_rate`, `rate_divisor_out_of_range`,
`bytes_per_sample_mismatch`, `payload_overlaps_descriptor`, `slot_bytes_below_planes`,
`slot_array_inside_header`, `slot_array_overruns_section`, and the four plane-alignment
terms - `unaligned_slot_array_offset`, `unaligned_slot_bytes`, `unaligned_payload_offset`,
`unaligned_channel_plane`.

Three properties of that family are deliberate:

- **One rule per vector.** Each section is otherwise valid and is grown where needed
  so no second rule fires as collateral. A vector breaking two rules still passes on
  a binding that implements only one of them, which defeats the purpose.
  `attach.header_bytes_below_minimum` is the unavoidable exception: a reader clamps
  its snapshot to `headerBytes`, so a short header necessarily zeroes later fields.
- **All four alignment terms, not a representative one.** The plane-alignment
  guarantee is what lets a binding build a typed view over mapped memory - in Rust a
  *safety* property. A binding may plausibly implement one term and forget three.
- **`slot_array_overruns_section` is the composite.** `slotArrayOffset`, `ringBlocks`
  and `slotBytes` are each individually in range; only the product runs past the
  mapping. It is the rule most likely to be missing from a hand-written binding, and
  the one whose absence is an out-of-bounds read rather than a wrong number.
