# Liveness *(normative)*

Two concurrent header fields - `deviceState` and `lastPublishQpc` - tell a
consumer whether the stream is alive, without consuming any blocks.

## The state table

| `deviceState` | `lastPublishQpc` | Meaning |
|---|---|---|
| `Active` | fresh | live |
| `Active` | **stale** | **producer crashed or wedged** |
| `Inactive` | any | engine alive, this device not being driven - legitimate quiet |
| `Closed` | any | clean shutdown |

`Inactive` covers every legitimate pause, which is what makes the crash row
trustworthy. Data-path degradation (upstream signal loss, output ramping down)
is **not** inactivity: blocks keep arriving and the stream stays `Active`.

Two refinements:

- **`producerPid`** (header, layout field) can strengthen the crash verdict:
  if it is non-zero and no longer names a running process, the segment is
  abandoned - a deterministic answer where staleness alone is a heuristic.
  The converse holds nothing: PIDs are reused, so a live PID never overrides
  the staleness rules below.
- **A producer that dies mid-rewrite leaves `layoutSeq` odd.** An attaching
  consumer then sees `NotReady` on every retry, and an attached one detaches
  - neither is an error in the consumer. The state resolves when a producer
  restarts into the segment: the restart steps past the odd value (see
  [seqlock.md](seqlock.md), writer side).

## Reading the timestamp

**QPC is system-wide on Windows**: compare `lastPublishQpc` against your own
`QueryPerformanceCounter()` with no clock-domain conversion. `qpcFrequency` is
in the header.

`lastPublishQpc` duplicates the newest block's `timestampQpc` exactly. It
exists because it is **stateless**: a diagnostic tool can attach, print, and
exit with a correct answer on its first read, where a block-derived check needs
two reads.

**It is a liveness field, not a data-timing field.** Correlating samples to
telemetry or video must use `ShmBlockDesc::timestampQpc` and nothing else.

## The thresholds

With `periodMs = samplesPerBlock × 1 000 000 / sampleRateMilliHz`:

- **Wait** with `max(50 ms, 4 × periodMs)`.
- **Declare stale** at `max(100 ms, 5 × periodMs)` - but only while
  `deviceState == Active`. Under `Inactive` or `Closed` a stale timestamp
  means nothing and must not be reported.

The floors keep very short periods from producing sub-millisecond timeouts no
host scheduler can satisfy. In the C++ API these are `Reader::waitTimeoutMs()`
and `Reader::staleAfterMs()` - compute them from the formulas, never hardcode
them (see [../../bindings/cpp/README.md](../../bindings/cpp/README.md)).
