# telemetry-shm v1

A third-party **producer** (a sim adapter, a rig controller, a test generator) publishes real-time telemetry;
irTactile reads it as a signal source. The payload is the channel model in
[spec/channels](../../channels/README.md): named `float32` channels with a kind, a unit and a validity bit per frame.
This document specifies the wire format.

Field tables: [fields.md](fields.md). Shared mechanics: [../../common](../../common) - the seqlock protocol
([seqlock.md](../../common/seqlock.md), including the *frame* generalization), concurrency classes
([concurrency.md](../../common/concurrency.md)), versioning ([versioning.md](../../common/versioning.md)) and
liveness ([liveness.md](../../common/liveness.md)).

## Two regions, one writer each

| Region | Name | Created by | Direction |
|---|---|---|---|
| **inbound** | the segment name, e.g. `irTactile_Telemetry_<simId>` | the producer | producer -> irTactile |
| **return** | derived from the inbound name, e.g. `irTactile_TelemetryReturn_<simId>` | irTactile | irTactile -> producer |

Each side **creates the region it writes and only opens the region it reads.** Absence is normal in both directions:
a producer must run without the return region (irTactile is not up, or does not use this source), and irTactile
treats a missing inbound region as "source not running".

### Names

The inbound name (the *segment name*) is the configuration value used to locate the source. A producer ships with
a fixed name and documents it; the user enters that name in the irTactile configuration (sim catalogue).
Discovery is configuration-based: the consumer does not enumerate segments, and irTactile does not pass the name
to the producer.

Inbound names starting with `irTactile_TelemetryReturn_` or ending in `_Return` are reserved and must be rejected.
This keeps inbound and return names disjoint.

A segment name is 1-200 characters from `[A-Za-z0-9_.-]`. The recommended form is `irTactile_Telemetry_<simId>`
(the bindings' `inboundName()` builds it). The return name is derived from the inbound name alone, so both sides
compute it without further configuration:

```
inbound starts with "irTactile_Telemetry_"  ->  "irTactile_TelemetryReturn_" + <rest>
otherwise                                   ->  <inbound> + "_Return"
```

Both are plain (unprefixed) names, so each side resolves them in its own session namespace. Client processes use
their session namespace; a service runs in session 0, whose names are in the global namespace. Elevation alone does
not change the session namespace. A producer running as a service is therefore not found by irTactile in a user
session; that is a deployment problem, not a protocol state. Both sides also need permission to open each other's
regions. See [Windows kernel object namespaces](https://learn.microsoft.com/windows/win32/termserv/kernel-object-namespaces).

### Single writer

A segment that already exists is not proof that its producer is gone: irTactile keeps the mapping open across a
producer crash. So before it creates or opens the inbound region, a producer **must** create the named event
`<inbound>#Writer` (Windows: `CreateEventW`) and keep the handle open for as long as it writes. If the event already
exists, another producer is live: the producer must not touch the segment and reports the conflict (C++:
`OpenResult::WriterActive`). Nobody else opens the event, so the kernel destroys it with the producer's last
handle, including after a crash, and the next producer restarts into the segment as usual. `#` is outside the
segment-name alphabet, so the event name cannot collide with a segment.

The return region has no such lock: irTactile runs as a single instance.

## Inbound region

One header, a descriptor table, and exactly **one frame** - the newest. There is no ring: `CONTINUOUS` and
`DISCRETE` channels are sampled state, and a frame overwritten before the consumer reads it carries nothing the next
frame does not supersede.

```
 [ header 256 B ][ desc[capacity] x 128 B ][ valid u64[capacity/64] ][ eventCount u32[capacity] ][ values f32[capacity] ]
```

Offsets are fields of the header; a consumer uses them, never `sizeof`. The producer fixes the segment size
(`totalBytes`) when it creates the segment; a layout rewrite must fit inside it.

### The producer's slots

Each channel occupies a numbered *slot* in `0 .. channelCount-1`. The producer declares it in the descriptor table and writes
`values[slot]`, `valid` bit `slot`, and `eventCount[slot]` for EVENT channels. Slot order is irrelevant to the
consumer - channels are resolved **by name**.

### Names a producer may declare

| Descriptor `name` | Meaning |
|---|---|
| a `std.*` table name (`std.engine.rpm`, `std.suspension.deflection[2]`) | a standard channel, validated against the table as described below. The bindings' `declare()` fills its properties from the table. |
| a *local* name (`demo_wave`, `pump[2]`) | a producer-specific channel; the consumer publishes it as `sim.<simId>.<name>` with the descriptor as given. `[a-z0-9_]` in every segment, last segment may also use upper case, optional `[i]` suffix. |

For a standard channel:

- `unit`, `rangeMin`, `rangeMax`, `kind`, the `OPTIONAL` flag and `layout` must equal the table entry.
  Reserved flag bits are ignored.
- An array element requires `1 <= arrayCapacity <= table capacity`, `arrayIndex < arrayCapacity`, and
  `arrayIndex` equal to the numeric suffix in its name. See [arrays](../../channels/README.md#arrays).
- `provenance` is the producer's statement; it is validated as an enum but not compared with the table.

Invalid descriptors are **rejected per channel**: a `plugin.*` or `sim.*` name, an unknown `std.*` name,
properties that contradict the table, invalid syntax, a duplicate name, or invalid descriptor fields
(see [fields.md](fields.md#channeldesc---128-bytes)). A rejected channel stays invalid on the consumer side
and its bit in `accepted[]` (return region) is clear.

### Header validation (consumer)

A consumer treats the segment as **untrusted**. It maps the whole segment, learns the mapped size from the OS and
validates it as follows. Failures return `Malformed` unless another result is specified:

- `magic` is not `ITTE` (`0` means "created, header not yet written" - retry, `NotReady`);
- `versionMajor != 1` (`BadVersion`); a higher `versionMinor` is tolerated;
- `headerBytes < 256`, or `headerBytes` / `totalBytes` exceed the mapped size, or `totalBytes < headerBytes`;
- `capacity` is not a multiple of 64 in `[64, 8192]`, or `channelCount > capacity`;
- any of `descOffset`, `validOffset`, `eventCountOffset`, `valuesOffset` is not 8-byte aligned, lies before
  `headerBytes`, or its array does not end inside `totalBytes`;
- `producerState` is not `Inactive`, `Active` or `Closed`;
- `wheelsPerAxle` has an entry above 8, or a non-zero entry after a zero;
- `simId` is not empty and not a valid owner id, or is `std`, `sim`, `plugin` or `irtactile`
  (a built-in simulator id is allowed - the source then extends that simulator's `sim.*` namespace).

An empty `simId` leaves the namespace to the consumer (irTactile uses the id of the sim the source is configured for).
A non-empty `simId` has at most 23 ASCII characters: the 24-byte field includes the terminating NUL.
Producers must reject longer ids rather than truncate them. Strings are NUL-terminated within their field;
a field without a NUL is malformed.

### Publish protocol (producer)

Fields have four concurrency classes ([concurrency.md](../../common/concurrency.md)):

- **pinned**: `magic`, `versionMajor`, `versionMinor`, `headerBytes`, `totalBytes`. Written once, never changed. A different value
  needs a different segment.
- **layout** (read under `layoutSeq`): geometry, `qpcFrequency`, `producerPid`, `simId`, the session identity
  (`simName`, `carId`, `carName`) and the **descriptor table**.
- **concurrent** (bytes `[80, 112)`): `frameSeq`, `lastPublishQpc`, `producerState`,
  `layoutSeq`, `frameSeqEnd`; read with per-field acquire loads.
- **frame** (read under the frame seqlock, never cached across reads): `frameQpc` and the `valid`, `eventCount`
  and `values` arrays.

**Lifecycle.** A producer:

1. Acquires the writer event and creates or opens the inbound region (see *Single writer*).
2. Declares its channels and commits the layout (C++: `commitLayout()`).
3. Sets `producerState = Active` before publishing the first frame.
4. Publishes frames while active; while inactive, refreshes the heartbeat (see *States*).
5. Opens the return region once it exists (its absence is normal) and polls it on every publish cycle
   (see [Return region](#return-region)).
6. On clean shutdown, sets `producerState = Closed`, then releases both mappings and the writer event.

**First creation.** Create the segment (zero-filled by the OS), write everything except `magic`, then store `magic`
with release. A consumer that sees `magic == 0` retries.

**Per frame** (the frame seqlock, [seqlock.md](../../common/seqlock.md) - *Generalization*):

```
frameSeq += 1                      // odd, release
clear valid[]; write values, valid, eventCount, frameQpc
frameSeqEnd = frameSeq + 1         // release
frameSeq += 1                      // even, release
lastPublishQpc = QPC now           // release
```

A frame does not write `producerState` (see *States*).

Event channels: `eventCount[slot]` is a free-running `uint32` incremented once per event; `values[slot]` holds the
amplitude of the latest one. The producer never decrements or resets it while the layout is unchanged.
Frames without an event retain that counter and amplitude and leave the slot's validity bit clear.

**Layout rewrite** (channel set, `simId`, car or any layout field changes; also at every producer start):

```
layoutSeq += 1  (odd)  ->  rewrite header layout fields + descriptor table  ->  layoutSeq += 1  (even)
```

A restarting producer cycles `layoutSeq` even when it rewrites nothing, and steps past a leftover odd value
([seqlock.md](../../common/seqlock.md), *Writer side*). A car change is a layout rewrite: `carId` / `carName` are
layout fields.

**States.** `producerState` is `Inactive` while the sim is not running or sits in a menu, `Active` while frames
are published, and `Closed` after a clean shutdown. The producer writes it only on a transition (`lastPublishQpc`
first, then `producerState`, both release), so it sets `Active` before its first frame. A consumer treats any
state other than `Active` as not live, however fresh the frames are. While `Inactive`, the producer keeps
refreshing `lastPublishQpc` as a heartbeat.

Liveness follows [liveness.md](../../common/liveness.md) with `lastPublishQpc`. The stale threshold is
`max(100 ms, 5 x 1000/rateHz)`; without a known rate the consumer assumes 20 ms per frame, i.e. 100 ms.

### Read protocol (consumer)

```
l1 = acquire(layoutSeq); if odd -> keep previous / retry
if l1 != cached layoutSeq -> snapshot header + descriptor table, re-validate, re-map names
f1 = acquire(frameSeq); if odd -> retry; if f1 == last seen -> no new frame
copy valid / eventCount / values / frameQpc of the mapped slots
f2 = acquire(frameSeqEnd) must equal f1, acquire(frameSeq) == f1, acquire(layoutSeq) == l1
else bounded retry (at most 4 tries) and keep the previous frame
```

An odd `layoutSeq` that persists beyond 1 s, or a `producerPid` that is non-zero and no longer names a running
process, means the producer is gone: the source is *disconnected*.

**Events.** The consumer remembers the previous `eventCount` per EVENT channel and, per read, turns a non-zero
difference (modular `uint32` arithmetic) into **one** event with the latest amplitude. Several events of one channel
between two reads collapse into one: individual occurrences and earlier amplitudes are lost.
The counter is checked even when the latest frame's validity bit is clear: an event may have occurred in an
overwritten frame. A non-zero difference emits the retained amplitude, and every consistent observation updates
the baseline. The validity bit describes only whether an event occurred in the published frame.
Detection requires fewer than 2^32 events between observations; a full counter wrap can appear unchanged.
The first read after attach or after a layout rewrite takes the count as baseline and emits nothing.

## Return region

irTactile creates it and the producer may read it. It holds only the latest value, is indexed by the
**producer's** slot numbers, and is tagged with the producer `layoutSeq` it refers to.

```
 [ header 128 B ][ accepted u64[capacity/64] ][ demanded u64[capacity/64] ]
```

The region has a fixed size of **4096 bytes** (enough for the maximum capacity of 8192) so the same segment survives
a producer restart with another capacity.

- `accepted[]` - bit set: the consumer bound this slot. Clear: rejected (see *Names a producer may declare*) or not
  yet evaluated. A producer can use it to report rejected channels without reading irTactile's log.
- `demanded[]` - bit set: something consumes this slot (the active graph, editor watches, plugins, or a recording).
  A producer **may** skip expensive channels whose bit is clear. While `consumerState != Active`, or while the
  consumer is gone (see *Consumer liveness*), every accepted channel counts as demanded.
- `consumerState`: `Idle` (irTactile is up, this source is not driving the bus), `Active`, `Closed`.
- `lastConsumeQpc`: QPC of the last frame irTactile consumed from this source.
- `consumerPid`: irTactile's process id; `0` means unknown.
- `forLayoutSeq`: the producer `layoutSeq` the masks were computed for. A producer ignores masks whose
  `forLayoutSeq` differs from its current one.

The region uses the same frame seqlock (`retSeq` / `retSeqEnd`) with bounded reader retries. `capacity`, both
offsets, `forLayoutSeq` and both masks are *frame* fields: the producer copies them inside one `retSeq` window and
never caches an offset across reads. `consumerState`, `lastConsumeQpc` and `consumerPid` are concurrent and read
with live acquire loads.

**Validation.** Before reading masks, the producer checks the return header:

- `magic == 0` means `NotReady`; any value other than `ITTR` is `Malformed`.
- `versionMajor != 1` is `BadVersion`; higher minor versions are tolerated.
- `headerBytes` must be at least 128 and no larger than the mapped size;
  `headerBytes <= totalBytes <= mapped size` must hold.
- `capacity` is either zero (no masks yet) or a multiple of 64 in `[64, 8192]`.
- For non-zero capacity, both mask offsets must be 8-byte aligned, at least `headerBytes`, and each
  `capacity / 8`-byte mask must end within `totalBytes`. With zero capacity, offsets are ignored.
- A live acquire load of `consumerState` must be `Idle`, `Active` or `Closed`.

Invalid sizes, offsets, capacity or state are `Malformed`. Validate frame fields inside the same `retSeq`
window used to copy the masks. The v1 writer allocates 4096 bytes; readers use the declared bounds above.

**Consumer liveness.** The producer keeps the return region mapped, so it outlives a consumer that crashes, and an
`Active` state and its masks would stay in effect without these rules:

- While `Active`, irTactile refreshes `lastConsumeQpc` with every frame it consumes from this source.
- A producer treats an `Active` consumer as **gone**, i.e. as not `Active`, when `lastConsumeQpc` is more than
  **1 s** older than its own QPC (`0` = never consumed), or when `consumerPid` is non-zero and no longer names a
  running process. A live PID never overrides a stale `lastConsumeQpc` (PIDs are reused). The 1 s threshold is
  independent of the frame rate: a producer that publishes slower than 1 Hz, or has just resumed from `Inactive`,
  reads the consumer as gone and so publishes every accepted channel, which is always safe.
- The producer applies this rule, and the `forLayoutSeq` check above, on every poll, including one that finds no
  consistent frame: it rechecks the cached view for staleness. A consumer that crashes inside a publication leaves
  `retSeq` odd, so no read succeeds until the next consumer resets the region; its last masks must not stay in
  effect meanwhile.
- A consumer that starts into an existing return region first resets it: `consumerState = Idle`,
  `lastConsumeQpc = 0`, then, under `retSeq`, both masks cleared with `forLayoutSeq = 0` (no committed layout has
  `layoutSeq` 0), and only then writes `consumerPid`. Nothing a previous consumer published stays in effect.

## Versioning

The channel model is append-only ([versioning.md](../../common/versioning.md)); within v1 the header may grow
(`headerBytes`), reserved space may be used, and new enum values may appear. A consumer ignores `reserved` bytes and
rejects unknown enum values (`producerState`, descriptor `kind`).

## Conformance

`conformance/telemetry-shm/v1/` holds golden segments for the *decode* side (header validation, descriptor mapping,
frame and event reads, return region) and the *encode* side (byte-exact output of scripted producer calls). A
binding passes when it reproduces every expectation of the manifest.
