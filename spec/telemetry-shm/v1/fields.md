# telemetry-shm v1 - field tables

All integers little-endian. All offsets are bytes from the segment base. 64-bit fields are 8-byte aligned. Strings
are UTF-8, NUL-terminated within the field, tail zeroed. **Class** is the concurrency class
([concurrency.md](../../common/concurrency.md)): *pinned*, *layout* (under `layoutSeq`), *concurrent*
(per-field acquire loads), *frame* (under the region's frame seqlock: `frameSeq` inbound, `retSeq` return; never
cached across reads).

## Inbound header - 256 bytes

| Offset | Size | Field | Class | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | `magic` | pinned | `0x45545449` (`'I','T','T','E'` in memory). `0` = created, header not yet written |
| 4 | 2 | `versionMajor` | pinned | `1`; reject a mismatch |
| 6 | 2 | `versionMinor` | pinned | `1` (`0` before `nominalRateMilliHz`); tolerate higher |
| 8 | 4 | `headerBytes` | pinned | `256`; never copy more than this |
| 12 | 4 | `totalBytes` | pinned | segment size; must not exceed the mapped size |
| 16 | 4 | `capacity` | layout | descriptor / value slots, multiple of 64, `[64, 8192]` |
| 20 | 4 | `channelCount` | layout | highest used slot + 1 (`<= capacity`) |
| 24 | 4 | `descOffset` | layout | `ChannelDesc[capacity]` |
| 28 | 4 | `validOffset` | layout | `uint64[capacity/64]` |
| 32 | 4 | `eventCountOffset` | layout | `uint32[capacity]` |
| 36 | 4 | `valuesOffset` | layout | `float32[capacity]` |
| 40 | 8 | `qpcFrequency` | layout | `QueryPerformanceFrequency()` of the producer |
| 48 | 4 | `producerPid` | layout | process id, `0` = unknown |
| 52 | 4 | `nominalRateMilliHz` | layout | nominal publish rate in milli-Hertz (`60000` = 60 Hz); sets the stale threshold (see [Liveness](README.md#liveness-consumer)). `0` = unknown (minor-0 producers zeroed this field). Otherwise `[1000, 1000000]` (1 Hz to 1 kHz); reject (`Malformed`) any other value. A change is a layout rewrite |
| 56 | 24 | `simId` | layout | namespace of local names (`sim.<simId>.*`); `[a-z0-9_]{1,23}`, NUL-padded to 24 bytes; empty = chosen by the consumer. Reject longer ids; do not truncate |
| 80 | 8 | `frameSeq` | concurrent | frame seqlock: odd = frame being written |
| 88 | 8 | `lastPublishQpc` | concurrent | QPC of the last publish (liveness field only) |
| 96 | 4 | `producerState` | concurrent | `0` Inactive, `1` Active, `2` Closed; reject others |
| 100 | 4 | `layoutSeq` | concurrent | odd = layout (header + descriptors) being rewritten |
| 104 | 8 | `frameSeqEnd` | concurrent | equals `frameSeq` after a consistent frame |
| 112 | 8 | `frameQpc` | frame | QPC of the sim sample in this frame |
| 120 | 32 | `simName` | layout | display name of the simulator / source |
| 152 | 64 | `carId` | layout | a change is a layout rewrite |
| 216 | 32 | `carName` | layout | display name of the car |
| 248 | 8 | `wheelsPerAxle` | layout | `uint8_t[8]`: wheels per axle, front to back (car `2,2`; truck `2,4,4`). The first `0` ends the list; all zero = unknown. Reject (`Malformed`) an entry above 8 or a non-zero entry after a zero. Defines the element order of `wheel` arrays and the element count of `axle` arrays; a change (car change) is a layout rewrite |

The concurrent range is `[80, 112)`. The next field, `frameQpc`, is protected by the frame seqlock.

## `ChannelDesc` - 128 bytes

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 64 | `name` | table name or local name; array elements carry `[i]` |
| 64 | 16 | `unit` | physical unit; must match the table for a standard channel |
| 80 | 4 | `rangeMin` | `float32`, nominal |
| 84 | 4 | `rangeMax` | `float32`, nominal |
| 88 | 4 | `kind` | `0` CONTINUOUS, `1` DISCRETE, `2` EVENT; reject others |
| 92 | 4 | `flags` | bit 1 `OPTIONAL`; bit 0 and all others reserved (producer zeroes, reader ignores) |
| 96 | 2 | `arrayIndex` | element index, `0` for scalars |
| 98 | 2 | `arrayCapacity` | `0` = scalar, else the number of elements the producer declares (at most the table capacity for a `std.*` array); `arrayIndex` must be below it |
| 100 | 1 | `provenance` | `0` unspecified (read as native), `1` native, `2` derived, `3` estimated; a descriptor with a larger value is rejected (that channel only) |
| 101 | 1 | `layout` | array element layout: `0` index, `1` wheel, `2` axle; scalars carry `0`. A larger value, or a non-zero value on a scalar, rejects that descriptor. For a `std.*` array it must equal the table entry |
| 102 | 26 | `reserved` | producer zeroes, reader ignores |

A descriptor with an empty `name` is an unused slot.

## Frame arrays

| Array | Element | Meaning |
|---|---|---|
| `valid` | `uint64[capacity/64]` | bit `slot & 63` of word `slot >> 6`; set = the slot carries a value in this frame |
| `eventCount` | `uint32[capacity]` | EVENT channels: free-running event counter; other slots `0` |
| `values` | `float32[capacity]` | value, or the amplitude of the latest event (retained through frames without an event) |

## Return header - 128 bytes

| Offset | Size | Field | Class | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | `magic` | pinned | `0x52545449` (`'I','T','T','R'` in memory). `0` = not yet written |
| 4 | 2 | `versionMajor` | pinned | `1` |
| 6 | 2 | `versionMinor` | pinned | `0` |
| 8 | 4 | `headerBytes` | pinned | `128` |
| 12 | 4 | `totalBytes` | pinned | `4096` |
| 16 | 4 | `capacity` | frame | slot count of the producer layout the masks refer to |
| 20 | 4 | `acceptedOffset` | frame | `128` |
| 24 | 4 | `demandedOffset` | frame | `128 + roundUp8(capacity / 8)`; follows `capacity` |
| 28 | 4 | `reserved0` | pinned | |
| 32 | 8 | `retSeq` | concurrent | frame seqlock of the return region |
| 40 | 8 | `retSeqEnd` | concurrent | equals `retSeq` after a consistent frame |
| 48 | 4 | `forLayoutSeq` | frame | producer `layoutSeq` the masks refer to |
| 52 | 4 | `consumerState` | concurrent | `0` Idle, `1` Active, `2` Closed |
| 56 | 8 | `lastConsumeQpc` | concurrent | QPC of the last consumed frame |
| 64 | 4 | `consumerPid` | concurrent | irTactile process id; rewritten at every consumer start |
| 68 | 60 | `reserved1` | | zero / ignore |

Both masks hold `capacity / 64` words. The consumer rewrites every *frame* field on each publish, so
`demandedOffset` moves when the producer capacity changes. 4096 bytes fit the masks for the maximum of 8192 slots.
While `capacity == 0` no masks have been published yet: both offsets are unspecified and the masks are empty.
