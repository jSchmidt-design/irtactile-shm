# stream-shm - wire format v1

**Format version 1.0** (`kVersionMajor = 1`, `kVersionMinor = 0`; minor versions
are additive - see [../../common/versioning.md](../../common/versioning.md)).

A named OS shared-memory section carrying one continuous, multi-channel sample
stream from a single producer, published on a fixed cadence with an optional
auto-reset event. A consumer maps the section read-only and reads a planar
float32/int16 ring - no command channel, no audio-driver detour.

**This is a signal transport, not an audio device.** irTactile models the stream
as an output device internally - that is how an operator configures it, in the
same list as sound cards - and the header carries `deviceName`, `deviceGuid` and
`deviceState` as a consequence. To a consumer those three are stream identity and
liveness, nothing more. The payload is whatever signal the producer's graph
routes into the stream: transducer drive, control values, telemetry-derived
envelopes. Nothing obliges a consumer to treat the samples as audio or hand them
to a sound API, and no audio endpoint exists on either side of the section.

This tree is the normative specification a consumer implements against:

| Document | Content |
|---|---|
| [fields.md](fields.md) | Byte layout: section map, `ShmStreamHeader`, `ShmBlockDesc`, alignment |
| [../../common/concurrency.md](../../common/concurrency.md) | Field concurrency classes (pinned / layout / concurrent), memory-ordering rules |
| [../../common/seqlock.md](../../common/seqlock.md) | The `layoutSeq` seqlock |
| [../../common/versioning.md](../../common/versioning.md) | Forward-compatibility rules |
| [../../common/liveness.md](../../common/liveness.md) | `deviceState`, QPC liveness, thresholds |
| [../../../bindings/cpp/README.md](../../../bindings/cpp/README.md) | The C++ consumer API (`Reader` / `Client`) - the reference implementation |
| [../../../bindings/csharp/README.md](../../../bindings/csharp/README.md) | The C# consumer API (`Reader` / `Client`) - reader-side only |

Producer behaviour is specified only where a consumer must reason about it.

---

## Stream semantics - read first

### The stream is not band-limited

Content above the stream's Nyquist frequency is **folded into the band**, not
removed. In a 3000 Hz stream a 5 kHz component arrives at 1 kHz, with nothing in
the data marking it as an alias.

`sourceSampleRate` and `rateDivisor` are **provenance only**: they report where
the stream came from and carry no assertion that it was band-limited on the way.
No header field makes that assertion, and no consumer-side treatment can undo
the folding.

If your consumer draws conclusions from frequency content, that is the
assumption to build on. Whether a particular stream is clean depends on how the
producer's graph was configured, and the section cannot tell you the answer.

It can also change without notice. A producer that band-limits before decimating
adds no header field and bumps no version, so a consumer sees cleaner data and no
signal that anything changed. Build for "may contain aliases" and you stay
correct either way; build a detector or a compensation and you are pinned to a
behaviour the contract never promised.

### A slower update rate batches; it does not reduce the data volume

The operator chooses how often the producer publishes. A slower choice raises
`samplesPerBlock` and lowers the notification count and the CPU cost of
receiving, and it raises latency. The samples per second are **unchanged**:
halving the update rate does not halve the bandwidth. Only `sampleRateMilliHz`
does that.

Everything you need follows from `sampleRateMilliHz` and `samplesPerBlock`. The
producer's internal batching count reaches you only as `diagFramesPerPublish`,
which is diagnostic output rather than contract - and, despite the name, is not
a sample count.

### The producer creates. The consumer only ever opens.

| Object | Consumer's call |
|---|---|
| Section | `OpenFileMappingW(FILE_MAP_READ, FALSE, name)` |
| View | `MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)` |
| Event | `OpenEventW(SYNCHRONIZE, FALSE, name)` |

Never `CreateFileMappingW`. Open-only turns a mistyped name into
`ERROR_FILE_NOT_FOUND` instead of a silently created empty section that polls
zeros forever. A consumer that starts before the producer gets the same error
and must retry (`AttachResult::NotReady` / `OpenResult::NotFound`).
`FILE_MAP_READ` also makes the mapped pages read-only, so a consumer bug faults
instead of corrupting the producer's ring.

**Map the whole section in one call** - `MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)`.
You never need the size before mapping; it costs address space only, and
physical pages arrive on first touch. The two-step alternative (map a header
window, read `totalBytes`, remap) is *worse*: between unmap and remap the
producer can restart and replace the section, so the second map would be
validated against a header no longer held.

**One check is mandatory.** Mapping blind means trusting `header.totalBytes`
before validating it:

```cpp
MEMORY_BASIC_INFORMATION mbi{};
VirtualQuery(base, &mbi, sizeof(mbi));
if (header->totalBytes > mbi.RegionSize) { /* reject; trust no offset */ }
```

`mbi.RegionSize` is the real mapped size. **Every header offset must be
validated against it before use** - including the composite
`slotArrayOffset + ringBlocks × slotBytes`.

Naming consequences on Windows:

- An unprefixed name lands in the caller's **session namespace**; producer and
  consumer must share a session. Cross-session requires an explicit `Global\`
  prefix on both sides *and* `SeCreateGlobalPrivilege` on the producer.
- The default DACL restricts access to the same user.

### What the operator configured, and what reaches you

The stream is set up in irTactile's device configuration. Most of those choices
arrive as header fields. Two never reach the section at all - plan for that
before you design around them:

| The operator sets | You read |
|---|---|
| Sample rate | `sampleRateMilliHz` (the same figure ×1000) |
| Update rate, shown as *"X Hz - N samples per update"* | `publishHz` = X, derived; `samplesPerBlock` = N |
| Output channels | `channelCount` |
| Sample format | `sampleFormat` |
| Ring blocks | `ringBlocks` |
| Shared memory size | the `_256k` / `_512k` / `_1m` suffix on the name you open |
| Section name, event name | the names you open |
| Device name | `deviceName` - informational, and rewritable on a restart |
| **Channel labels** | **nothing** - they stay in the editor |

Channel labels never enter the section, so channel identity remains positional
and established out of band. See *There is no channel table* below.

Every one of these is fixed for the producer's lifetime: changing any of them
requires restarting the producer, which a consumer observes as a
`layoutSeq` rewrite or a fresh section, never as a field mutating underneath a
running read.

---

## Rate and cadence

The stream rate is one of eleven rates, `48000 / 2^k` Hz for k = 0..10 - 48000 Hz
down to 46.875 Hz, each half the one above. `sampleRateMilliHz` carries it ×1000, so the field holds
`48'000'000 / 2^k`. The publish cadence is **fully derivable from two header
fields** - a consumer must derive it and hardcode nothing:

| Quantity | Formula |
|---|---|
| `periodSeconds` | `samplesPerBlock / (sampleRateMilliHz / 1000)` |
| `periodMs` | `samplesPerBlock × 1 000 000 / sampleRateMilliHz` |
| `publishHz` | `(sampleRateMilliHz / 1000) / samplesPerBlock` |
| `ringSeconds` | `ringBlocks × periodSeconds` |

The current producer publishes at no more than **375 Hz** - see *Limits*, and
note that this is a bound on the producer, not on the format. Below that ceiling
the operator picks the update rate and `samplesPerBlock` follows; the two always
multiply back to the sample rate. `samplesPerBlock` is 1 where the operator asked
to be woken on every single sample, which the current ceiling permits at stream
rates of 375 Hz and below.

A block is accumulated before it is published, so it arrives with up to one
period of inherent latency. That period is your real latency floor, and it is
the operator's choice of update rate - not the sample rate - that sets it. Read
it from the table above rather than assuming a figure: it is the one quantity
that changes when the stream is reconfigured or the producer is upgraded.

`diagEngineBlockSize` and `diagFramesPerPublish` describe the producer's
internals for support purposes only; a binding that reads them for anything
beyond printing them is misusing them. `diagFramesPerPublish` in particular
counts the producer's internal render periods, **not samples** - the sample
count is `samplesPerBlock`. The producer's render geometry appears nowhere else
in the header, deliberately: a consumer that hardcodes it is wrong the moment
the stream is reconfigured.

---

## Addressing *(normative)*

> **The header fields are normative. A consumer reads the fields and never
> re-derives them.**

```
slot_index   = seq % ringBlocks
slot_base    = base + slotArrayOffset + slot_index × slotBytes

descriptor   = slot_base                                        // ShmBlockDesc
plane(ch)    = slot_base + slotPayloadOffset + ch × channelPlaneBytes
sample(ch,i) = plane(ch) + i × bytesPerSample                   // i < samplesPerBlock
```

Every term is a byte offset from `base`. The only sample-unit quantities in the
contract are `samplesPerBlock` and `firstSampleIndex`, and neither is ever
multiplied into an offset. The loop bound is `header.samplesPerBlock`, validated
at attach - see [fields.md](fields.md), *Every published block is full*.

Four addressing fields are derivable (`channelPlaneBytes`, `slotPayloadOffset`,
`slotBytes`, `bytesPerSample`). **Do not derive them.** A consumer that
recomputes rather than reads works until a future version pads differently or
inserts a table, then walks into the middle of a slot with no error.
`slotPayloadOffset` is a runtime field rather than a hardcoded descriptor size
precisely so that rule has no exceptions.

### Worked example

8 channels, 3000 Hz, `samplesPerBlock` 48, float32, `ringBlocks` 2 →
`bytesPerSample` 4, `channelPlaneBytes` 192, `slotPayloadOffset` 64,
`slotBytes` 1600:

```
    0   header                                                256 B
  256   slot 0          desc @256   ch0 @320   ch1 @512
                        ch2 @704    ch3 @896   ...  ch7 @1664 1600 B
 1856   slot 1          desc @1856  ch0 @1920  ch1 @2112
                        ch2 @2304   ch3 @2496  ...  ch7 @3264 1600 B
 3456   end of the live layout (totalBytes is the size class - 262144 here)
```

Sample 5 of channel 3 in the block held by slot 1 is at
`1856 + 64 + 3×192 + 5×4 = 2516`.

### The two masks: wired vs silent

The difference is **lifetime**, not degree:

Both are **set-means-true** on bit c, but they assert opposite things:

| | Where | Bit c set says | What to do with it |
|---|---|---|---|
| `wiredChannelMask` | header | Channel c **has** a source. Bit **clear** ⇒ no source, so zeros for as long as the current graph stands | Decide **once** at attach: for a clear bit, do not allocate the buffer, do not budget the bandwidth |
| `silentChannelMask` | descriptor | Channel c is all-zero **in this block** | Skip one transmission, keep the channel, re-check next block |

An unwired channel is **clear** in `wiredChannelMask` and **set** in
`silentChannelMask`, every block. Both are processing hints, acted on by index -
neither carries channel *identity*.

### There is no channel table

The section is addressed by index and carries no per-channel strings. Channel
identity is established by whoever configured the producer, out of band.
**No consumer should look up a channel by name at runtime.** What a consumer
needs instead - an assertion that the section matches the structure it was
built against - is a count/rate comparison at attach (`StreamRequest` →
`AttachResult::Mismatch` in the C++ API), which compares numbers, not strings.

`channelArrayOffset` is a header field with a documented `0` meaning "no
channel table", so a future minor version can add one without breaking v1
readers.

---

## The publish protocol *(normative)*

### Section creation

A freshly created section is zero-filled by the OS, and `magic == 0` is the
consumer's "not ready yet" signal - which makes the creation ordering
load-bearing:

1. Fill **every** other header field: pinned, layout (with `layoutSeq` even),
   the concurrent defaults (`writeSeq = 0`, `deviceState = Inactive`).
2. **Release**-store `magic = kMagic`, as the **last** store.

A reader that passes the magic check must never be able to observe a
half-written header: its attach-time validation failing there is `Malformed`,
which is **fatal**, not a retry - a producer that stores `magic` early turns a
startup race into a consumer that gives up permanently on a healthy stream.

### Restarting into an existing section

A producer that reopens a section a consumer may still hold **must**:

- **Preserve `writeSeq`** - resume from the value found in the section, never
  reset it. It is the one counter that always moves forward.
- **Cycle `layoutSeq`** (odd while rewriting, then even, ending strictly
  greater) **on every restart, even when no field changes.** The seqlock bump
  is the only signal a holding consumer gets that a restart happened - it is
  what triggers the re-read of `deviceName`, the re-check of any structural
  assertion, and the consumer's awareness that `firstSampleIndex` restarted
  at 0. A leftover **odd** `layoutSeq` (a producer that crashed mid-rewrite)
  is stepped past, not decremented, so the final value is even and greater
  than anything a spinning reader has seen.

### Publishing a block

The slot under construction is written **in place, incrementally**; there is no
staging buffer. Only the final chunk of a publish makes the block visible:

1. `slot_base = slotArrayOffset + (writeSeq % ringBlocks) × slotBytes`
2. *(first chunk only)* **Release**-store `desc.seq = kSlotBusy` - the slot is
   declared under construction *before* the first sample of the new block
   lands in it.
3. Write this chunk's samples into the channel planes at the running offset.
4. *(last chunk only)* Fill `firstSampleIndex`, `timestampQpc`,
   `flags`, `silentChannelMask`, `layoutEpoch`.
5. *(last chunk only)* **Release**-store `desc.seq = writeSeq`.
6. *(last chunk only)* Store `header.lastPublishQpc`, **then** release-store
   `header.writeSeq = writeSeq + 1` - in that order, so `lastPublishQpc` is
   never older than the block count implies.
7. *(last chunk only)* Signal the event, if one is configured.

**Step 2 is load-bearing.** At `ringBlocks: 2` a reader preempted mid-copy
resumes into a slot the writer has already reclaimed for the next block.
Without `kSlotBusy` the descriptor still reads as the old block, the reader's
re-check passes, and it returns torn data. The exposure is the whole
construction window (up to one publish period), not the copy. The same store
also distinguishes a never-written zero-filled slot from block `seq == 0`.

If the producer breaks mid-publish, the slot is **abandoned** at `kSlotBusy` -
never published half-full.

---

## The reader protocol *(normative)*

This is what `Reader::attach()` / `next()` / `latest()` implement. It is
written out because a non-C++ binding has to reimplement it - not because a C++
consumer should ever see it.

### Attach

1. Map the whole section. `magic == 0` means "created, header not yet written"
   - `NotReady`, retry. A *wrong* magic is `Malformed`, fatal.
2. Reject a `versionMajor` mismatch (`BadVersion`). Tolerate a higher
   `versionMinor`; reject unknown `sampleFormat` / `channelLayout` values.
3. `VirtualQuery` the base; reject if `totalBytes > mbi.RegionSize`, then
   validate every offset against `mbi.RegionSize` before use. Validate the
   alignment invariants too: `slotArrayOffset`, `slotBytes`,
   `slotPayloadOffset` and `channelPlaneBytes` must each be a multiple of
   `bytesPerSample` - that is what backs the plane-alignment guarantee a
   typed view's soundness rests on (see [fields.md](fields.md),
   *Representation, alignment and padding*).
4. Take a header snapshot under the `layoutSeq` seqlock
   ([../../common/seqlock.md](../../common/seqlock.md)).
5. If you hold an expected configuration, compare it against the snapshot and
   fail with `Mismatch` on any disagreement.
6. Initialise `cursor = header.writeSeq` - **not 0**, or the first read
   reports a spurious multi-million-block overrun. To read ring history,
   start at `head - min(head, ringBlocks - 1)`.

### Per-read

1. **Acquire**-load `header.writeSeq` → `head`, and `layoutSeq` alongside it.
   `head == cursor` means nothing new.
2. **The readable window is `ringBlocks - 1` blocks, not `ringBlocks`.** The
   writer owns slot `head % ringBlocks` for the whole current period, so the
   newest complete block is `head - 1`. If `head - cursor > ringBlocks - 1`
   you fell behind: report the gap and snap
   `cursor = head - (ringBlocks - 1)`.
3. For each `seq` in `[cursor, head)`: address the slot; acquire-load
   `desc->seq`; if `!= seq` - including `kSlotBusy` - discard the block and
   continue with the next `seq`. **Copy** the descriptor and all
   `channelCount` planes out. Then acquire-load **three** values and discard
   the block unless all three agree:

   | Load | Must equal |
   |---|---|
   | `desc->seq` | the `seq` you entered the copy with |
   | `header.layoutSeq` | the `layoutSeq` your header snapshot was taken under |
   | `desc->layoutEpoch` | the same snapshot `layoutSeq` |

4. `cursor = head`.

The two `desc->seq` checks are plain equality against the requested `seq`;
`kSlotBusy` needs no special case because a claimed slot can never compare
equal. The `layoutEpoch` check is not redundant with the `layoutSeq` one: it
proves the payload you copied was *written* under the geometry you validated,
not merely that no rewrite has landed since.

A latest-value-wins consumer skips the loop and reads only
`slot = (head - 1) % ringBlocks`, with the same checks around the copy. It
still wants step 2: `head == cursor` is how it knows its value is current.

**The copy in step 3 is mandatory.** There is no way to read a slot safely
without copying it and re-checking; that is why the C++ `BlockView` owns its
data rather than pointing into the section.

### Unsigned arithmetic

`head`, `cursor` and `seq` are `uint64_t` and every comparison is a difference
(`head - cursor`), never `<`. Differences are correct under wrap and survive a
`cursor` momentarily ahead of `head` - which a 32-bit consumer reading the two
halves of `writeSeq` non-atomically can manufacture. A binding that writes
`if (cursor < head)` will work for years and then not.

---

## Ring depth

The reader's budget is `(ringBlocks - 1)` publish periods. What must fit
depends on consumer semantics:

| Mode | What must fit | Typical magnitude |
|---|---|---|
| Latest-value-wins | **copy duration**, with retry on tear | ~300 ns for 4 KB |
| Lossless streaming | wake latency + copy | 0.1–20 ms (scheduling-bound) |

Wake latency drops out for a latest-value consumer: waking late just yields a
newer block, which is what it wanted. `ringBlocks: 2` is sufficient for that
mode - the writer occupies one slot for the whole period and the reader has
the other, with a torn copy detected by the `seq` re-check and retried in
microseconds. That argument depends entirely on `kSlotBusy`. A lossless
consumer needs headroom sized from `ringSeconds` above.

---

## The event *(normative)*

The event is **auto-reset**, created by the producer, signalled as step 7 of
the publish protocol and never earlier - a woken reader always finds the
counter already advanced.

- **Auto-reset wakes exactly one waiter: a configured event means one consumer
  per stream.** Two waiting processes each miss half the wakeups. A second
  process that needs its own wake-up needs its own stream on the producer side,
  not a second consumer on this one.
- **Open the event, never create it.** `CreateEventW` on an existing name
  silently ignores the manual-reset and initial-state arguments, so a process
  that creates the name first imposes its semantics on everybody.
- **Wakes coalesce.** One wake does not mean one block once you fall behind.
  Always drain: `while (next(b))`.
- **Polling `writeSeq` has no such restriction**, needs no handle, and is
  trivial in any language. Polling consumers can share one section freely; for
  latest-value-wins consumers it is arguably the better pattern.
- Producer shutdown and deactivation signal the event once after storing the
  new `deviceState`, so a blocked consumer learns the stream went quiet instead
  of waiting out a timeout.

The honest cost of polling: up to a quarter-period of added latency where the
event adds microseconds. Irrelevant for a display, worth knowing for a haptic
path.

**Derive the poll interval from `periodMs()`, never from a constant.** A fixed
interval that comfortably beats the period today is the wrong interval on a
faster stream, and it degrades silently - the consumer keeps working and simply
runs a cadence behind. A poll interval also has a floor the event does not: at a
short enough period, sleeping between polls costs more than the period itself and
the only way to hold the quarter-period figure is to spin. Where that trade is
unacceptable, take the event.

---

## Limits *(normative)*

| Bound | Value | Rationale |
|---|---|---|
| `channelCount` | 1–32 | A `uint32_t` mask covers the maximum exactly |
| Stream rate | 46.875 Hz – 48 kHz | Eleven rates, `48000 / 2^k`, each half the one above |
| `publishHz` | **≤ 375 Hz** for the current producer | A producer-side bound, not a property of the format: it is the producer's render cadence that sets it, and a later producer may publish faster. **Derive the period from `samplesPerBlock` and `sampleRateMilliHz`; hardcode nothing.** A consumer that sizes a buffer or a poll interval against 2.67 ms is pinned to a behaviour the contract does not promise |
| `ringBlocks` | 2–256 | 1 offers no protection - writer and reader always share the slot |
| Ring duration | warn above 1 s | Beyond that it is a recording buffer in shared memory - the wrong tool |

### Size classes and resolved names

The section is allocated into one of three discrete size classes:

| Class | Section bytes | Name suffix |
|---|---|---|
| `"256k"` | 256 KiB | `_256k` |
| `"512k"` | 512 KiB | `_512k` |
| `"1m"` | 1 MiB | `_1m` |

`header.totalBytes` is the class size, normally larger than the live layout
footprint `align64(sizeof(ShmStreamHeader)) + ringBlocks × slotBytes`; the
trailing bytes are producer-zeroed padding. The class is chosen against a
Float32 envelope even when the stream is `int16`, so the sample format never
changes the class.

The producer appends the class suffix to the configured base names before
creating the kernel objects:

```
resolvedShmName   = shmName   + sizeClassSuffix
resolvedEventName = eventName + sizeClassSuffix
```

A class switch creates a **new** section and event under a different name.
Before creating it, the producer stamps any abandoned section at the old name
`Closed` and signals its event, so a parked consumer there wakes with `Closed`
rather than timing out. A section at another class that is still being
published into belongs to a *second producer* and is left untouched.

**Pin the class; do not discover it.** Both names are yours to choose - you
supply the base name and the size class the stream is deployed with - so both
resolved names are known when you build and belong in your binary as constants.
A stream left on the default class can move to a different class when its
configuration changes, silently renaming both objects.

> **Never probe the three class names in order.** A zombie section held open by
> a stuck consumer answers the probe, and you attach to a stream nobody writes
> to.

---

## Multiple streams

Multiple independent streams coexist freely - each has its own section and
event names, its own rate and channel set. The only shared resource is the OS
kernel name space, and a producer must fail loudly on a duplicate name rather
than let two streams scribble over one section.

## Out of scope in v1

- Bidirectional use (a consumer feeding samples *in*). The stream is a sink.
- Consumer-owned structure - the consumer creating or laying out the section.
- Non-power-of-two rates, and rates above 48 kHz.
- Cross-machine transport.
