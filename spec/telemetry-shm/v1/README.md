# telemetry-shm v1

A third-party **producer** (a sim adapter, a rig controller, a test generator) publishes real-time telemetry;
irTactile reads it as a signal source. The payload is the channel model in
[../channel_model.md](../channel_model.md): named `float32` channels with a kind, a unit and a validity bit per frame.
This document is the wire format that carries it.

Field tables: [fields.md](fields.md). Shared mechanics: [../../common](../../common) - the seqlock protocol
([seqlock.md](../../common/seqlock.md), including the *frame* generalization), concurrency classes
([concurrency.md](../../common/concurrency.md)), versioning ([versioning.md](../../common/versioning.md)) and
liveness ([liveness.md](../../common/liveness.md)).

## Two regions, one writer each

| Region | Name | Created by | Direction |
|---|---|---|---|
| **inbound** | `irTactile_Telemetry_<sourceId>` | the producer | producer -> irTactile |
| **return** | `irTactile_TelemetryReturn_<sourceId>` | irTactile | irTactile -> producer |

Each side **creates the region it writes and only opens the region it reads.** Absence is normal in both directions:
a producer must run without the return region (irTactile is not up, or does not use this source), and irTactile
treats a missing inbound region as "source not running".

### Names

`sourceId` is `[a-z0-9_]{1,24}` (the *owner id* rule of the channel model). Windows has no supported way to
enumerate named sections, so the consumer always opens names it was configured with; the name is therefore a
contract between producer and configuration, and **only `sourceId` has to match**:

- inbound name: `irTactile_Telemetry_<sourceId>`
- return name: `irTactile_TelemetryReturn_<sourceId>`

A configuration may override the inbound name with a free `sectionName`. The return name is then derived from the
inbound name alone, so both sides compute it without further configuration:

```
inbound starts with "irTactile_Telemetry_"  ->  "irTactile_TelemetryReturn_" + <rest>
otherwise                                   ->  <inbound> + "_Return"
```

Both are plain (unprefixed) names, i.e. the session namespace, as in stream-shm. A producer running elevated or as a
service lives in another namespace and is not found; that is a deployment problem, not a protocol state.

When irTactile launches the producer it passes `IRTACTILE_TELEMETRY_SOURCE=<sourceId>` and
`IRTACTILE_TELEMETRY_SECTION=<inbound name>` in the environment. A producer takes its identity from them and falls
back to its own default when they are absent.

## Inbound region

One header, a descriptor table, and exactly **one frame** - the newest. There is no ring: `CONTINUOUS` and
`DISCRETE` channels are sampled state, and a frame overwritten before the consumer reads it carries nothing the next
frame does not supersede.

```
 [ header 256 B ][ desc[capacity] x 128 B ][ valid u64[capacity/64] ][ eventCount u32[capacity] ][ values f32[capacity] ]
```

Offsets are fields of the header; a consumer uses them, never `sizeof`. The producer fixes the section size
(`totalBytes`) when it creates the section; a layout rewrite must fit inside it.

### The producer's slots

A producer numbers its channels `0 .. channelCount-1` (a *slot*), declares them in the descriptor table, and writes
`values[slot]`, `valid` bit `slot`, and `eventCount[slot]` for EVENT channels. Slot order is irrelevant to the
consumer - channels are resolved **by name**.

### Names a producer may declare

| Descriptor `name` | Meaning |
|---|---|
| a `core.*` or `ext.*` table name (`core.engine.rpm`, `ext.tyre.temp.FL`) | the standard channel. `unit`, `rangeMin`, `rangeMax`, `kind`, `flags` and `arrayCapacity` **must equal the table entry**; the bindings' `declare()` fills them from the table. |
| a *local* name (`demo_wave`, `pump[2]`) | a producer-specific channel; the consumer publishes it as `sim.<simId>.<name>` with the descriptor as given. `[a-z0-9_]` in every segment, last segment may also use upper case, optional `[i]` suffix. |

Everything else is **rejected per channel** (not per source): a `plugin.*` or `sim.*` name, an unknown `core.*` /
`ext.*` name, a table name whose properties contradict the table, invalid syntax, a duplicate name, an unknown
`kind`. A rejected channel stays invalid on the consumer side and its bit in `accepted[]` (return region) is clear.

### Header validation (consumer)

A consumer treats the section as **untrusted**. It maps the whole section, learns the mapped size from the OS and
rejects the source (`Malformed`) when:

- `magic` is not `ITTE` (`0` means "created, header not yet written" - retry, `NotReady`);
- `versionMajor != 1` (`BadVersion`); a higher `versionMinor` is tolerated;
- `headerBytes < 256`, or `headerBytes` / `totalBytes` exceed the mapped size, or `totalBytes < headerBytes`;
- `capacity` is not a multiple of 64 in `[64, 8192]`, or `channelCount > capacity`;
- any of `descOffset`, `validOffset`, `eventCountOffset`, `valuesOffset` is not 8-byte aligned, lies before
  `headerBytes`, or its array does not end inside `totalBytes`;
- `producerState` is not `Inactive`, `Active` or `Closed`;
- `simId` is not empty and not a valid owner id, or is `core`, `ext`, `sim`, `plugin` or `irtactile`
  (a built-in simulator id is allowed - the source then extends that simulator's `sim.*` namespace).

An empty `simId` means "use the `sourceId`". Strings are NUL-terminated within their field; a field without a NUL
is malformed.

### Publish protocol (producer)

Fields have three concurrency classes ([concurrency.md](../../common/concurrency.md)):

- **pinned**: `magic`, `versionMajor`, `headerBytes`, `totalBytes`. Written once, never changed. A different value
  needs a different section.
- **layout** (read under `layoutSeq`): geometry, `qpcFrequency`, `producerPid`, `simId`, the session identity
  (`simName`, `carId`, `carName`) and the **descriptor table**.
- **concurrent** (bytes `[80, 120)`): `frameSeq`, `lastPublishQpc`, `producerState`, `layoutSeq`, `frameSeqEnd`,
  `frameQpc`; read with per-field acquire loads.

**First creation.** Create the section (zero-filled by the OS), write everything except `magic`, then store `magic`
with release. A consumer that sees `magic == 0` retries.

**Per frame** (the frame seqlock, [seqlock.md](../../common/seqlock.md) - *Generalization*):

```
frameSeq += 1                      // odd, release
clear valid[]; write values, valid, eventCount, frameQpc
frameSeqEnd = frameSeq + 1         // release
frameSeq += 1                      // even, release
lastPublishQpc = QPC now           // release
producerState  = Active            // release
```

Event channels: `eventCount[slot]` is a free-running `uint32` incremented once per event; `values[slot]` holds the
amplitude of the latest one. The producer never decrements or resets it while the layout is unchanged.

**Layout rewrite** (channel set, `simId`, car or any layout field changes; also at every producer start):

```
layoutSeq += 1  (odd)  ->  rewrite header layout fields + descriptor table  ->  layoutSeq += 1  (even)
```

A restarting producer cycles `layoutSeq` even when it rewrites nothing, and steps past a leftover odd value
([seqlock.md](../../common/seqlock.md), *Writer side*). A car change is a layout rewrite: `carId` / `carName` are
layout fields.

**States.** `producerState = Inactive` while the sim is not running or sits in a menu (the producer is up, no
telemetry), `Active` while frames are published, `Closed` on clean shutdown. Liveness follows
[liveness.md](../../common/liveness.md) with `lastPublishQpc`; the producer publishes at its own rate (the
stale threshold is `max(100 ms, 5 x 1000/rateHz)`, a consumer assumes 20 ms when no rate is known, i.e. 100 ms).

### Read protocol (consumer)

```
l1 = acquire(layoutSeq); if odd -> keep previous / retry
if l1 != cached layoutSeq -> snapshot header + descriptor table, re-validate, re-map names
f1 = acquire(frameSeq); if odd or f1 == last seen -> no new frame
copy valid / eventCount / values / frameQpc of the mapped slots
f2 = acquire(frameSeqEnd) must equal f1, acquire(frameSeq) == f1, acquire(layoutSeq) == l1
else bounded retry (at most 4 tries) and keep the previous frame
```

An odd `layoutSeq` that persists beyond 1 s, or a `producerPid` that is non-zero and no longer names a running
process, means the producer is gone: the source is *disconnected*.

**Events.** The consumer remembers the previous `eventCount` per EVENT channel and, per read, turns a non-zero
difference (modular `uint32` arithmetic) into **one** event with the latest amplitude. Several events of one channel
between two reads collapse into one; none is lost. The first read after attach or after a layout rewrite takes the
count as baseline and emits nothing.

## Return region

Created by irTactile, read by the producer, also latest-value and optional for the producer. It is indexed by the
**producer's** slot numbers and tagged with the producer `layoutSeq` it refers to.

```
 [ header 128 B ][ accepted u64[capacity/64] ][ demanded u64[capacity/64] ]
```

The region has a fixed size of **4096 bytes** (enough for the maximum capacity of 8192) so the same section survives
a producer restart with another capacity.

- `accepted[]` - bit set: the consumer bound this slot. Clear: rejected (see *Names a producer may declare*) or not
  yet evaluated. Lets a producer report mapping errors without irTactile's log.
- `demanded[]` - bit set: something consumes this slot (the active graph, editor watches, plugins, or a recording).
  A producer **may** skip expensive channels whose bit is clear. While `consumerState != Active`, every accepted
  channel counts as demanded.
- `consumerState`: `Idle` (irTactile is up, this source is not driving the bus), `Active`, `Closed`.
- `lastConsumeQpc`: QPC of the last frame irTactile consumed from this source; `consumerPid`.
- `forLayoutSeq`: the producer `layoutSeq` the masks were computed for. A producer ignores masks whose
  `forLayoutSeq` differs from its current one.

Protocol: the same frame seqlock (`retSeq` / `retSeqEnd`), bounded reader retries, latest value only.

## Versioning

The channel model is append-only ([versioning.md](../../common/versioning.md)); within v1 the header may grow
(`headerBytes`), reserved space may be used, and new enum values may appear. A consumer ignores `reserved` bytes and
rejects unknown enum values (`producerState`, descriptor `kind`).

## Conformance

`conformance/telemetry-shm/v1/` holds golden sections for the *decode* side (header validation, descriptor mapping,
frame and event reads, return region) and the *encode* side (byte-exact output of scripted producer calls). A
binding passes when it reproduces every expectation of the manifest.
