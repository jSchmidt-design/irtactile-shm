# The `layoutSeq` seqlock

`layoutSeq` guards the **layout fields** of the header (see
[concurrency.md](concurrency.md), *Layout fields*). It allows the producer to rewrite the
segment geometry in place - a restart into the same segment - while readers
hold the mapping open.

## Reading

```
do {
    s1 = acquire_load(layoutSeq);
    if (s1 & 1) continue;              // rewrite in progress
    copy the header, up to headerBytes;
    acquire_fence();                   // the copy completes before the recheck
    s2 = acquire_load(layoutSeq);
} while (s1 != s2);
```

The fence is required, not a detail of one binding: see *Memory ordering*.

A snapshot is valid only if it was taken while `layoutSeq` was even and
unchanged across the copy.

Copying the whole header (clamped to `headerBytes`) is fine and is what the
C++ reader does: the bytes of the concurrent range land in the snapshot too,
but they are **meaningless there** - the concurrent fields change on every
publish without any `layoutSeq` movement, so those bytes may be mid-store
garbage. A reader must take concurrent values only from live per-field
acquire loads, never from a snapshot.

## Rules

- **Re-read `layoutSeq` on every read cycle, not only at attach.** If it
  changed, re-take the snapshot before using any offset - the copy you hold
  may name a layout the producer no longer has.
- **In stream-shm, every block copy is checked against the snapshot's epoch** (`layoutEpoch`
  in the block descriptor; see
  [../stream-shm/v1/fields.md](../stream-shm/v1/fields.md)). A block published
  under a different epoch is discarded - it defends against stale-geometry
  aliasing after a rewrite.
- **Writer side:** odd while rewriting, even when stable. The odd store is
  followed by a release fence before the first rewritten byte (*Memory
  ordering*). A consumer never writes it. A producer restarting into an existing segment **must cycle the
  seqlock even when it rewrites nothing** - the bump is the only restart
  signal a holding consumer gets - and must step **past** a leftover odd
  value (a predecessor crashed mid-rewrite) so the final value is even and
  strictly greater than anything a spinning reader has seen. See the
  format's publish protocol, *Restarting into an existing segment*.

The formats define their complete read protocols:
[stream-shm](../stream-shm/v1/README.md) and
[telemetry-shm](../telemetry-shm/v1/README.md#read-protocol-consumer).

## Generalization: a counter that guards a whole frame

`layoutSeq` guards the layout fields of a header that is otherwise written
concurrently. A frame seqlock guards the **frame fields** published as one unit,
such as telemetry values and their sample timestamp. Other fields in the same
segment may be pinned, layout or concurrent fields. Each format names its frame
counter (`frameSeq` inbound or `retSeq` return in telemetry-shm) and specifies
which fields it protects. A segment may carry both layout and frame seqlocks.

Writer:

```
frameSeq += 1            // odd: write in progress     (release)
release_fence()          // frame stores stay after the odd store
write the frame fields
frameSeqEnd = frameSeq + 1                              (release)
frameSeq += 1            // even: stable                (release)
```

Reader (bounded retries):

```
for (try = 0; try < N; ++try) {
    s1 = acquire_load(frameSeq);
    if (s1 & 1) continue;                 // write in progress
    copy the frame bytes you need;
    acquire_fence();                      // the copy completes before the recheck
    s2 = acquire_load(frameSeqEnd);
    if (s1 == s2 && acquire_load(frameSeq) == s1) return OK;
}
return KEEP_PREVIOUS_FRAME;
```

Rules that differ from `layoutSeq`:

- **Bounded retries.** A reader on a real-time thread never spins unboundedly;
  after N failed tries it keeps its previous frame and tries again next cycle.
- **A partial copy is allowed** when the reader can locate the required bytes
  using validated offsets. Read frame offsets within the frame seqlock window;
  read layout offsets from a validated layout snapshot and recheck `layoutSeq`
  after the frame copy. The frame counters must also match after the copy.
- **Latest value only.** A frame segment carries one frame; there is no queue.
  A reader that falls behind loses frames, never reads a mix of two.
- **Every protected value comes from inside the window.** Read each frame
  field, offsets included, before the fence and the recheck, keep it in a local,
  and use only that local afterwards. A value loaded from the segment after the
  recheck belongs to no particular frame.

## Memory ordering

An acquire load keeps the accesses that follow it after it. A release store
keeps the accesses that precede it before it. A seqlock also needs the
opposite direction at two points, which neither gives, so both need a fence:

| Side | Where | Fence | Without it |
|---|---|---|---|
| Writer | after the odd counter store, before the first protected store | release | a reader can see new protected bytes while the counter still reads even and unchanged |
| Reader | after the copy, before the first closing counter load | acquire | the copy can complete after the recheck, so a torn copy passes it |

The opening reader load (acquire) and the closing writer stores (release) need
nothing extra. Every seqlock in these formats follows this rule: `layoutSeq`,
the telemetry `frameSeq` and `retSeq`, and the stream-shm slot descriptor
`seq` (`kSlotBusy` is its odd value; see the stream-shm publish protocol).

On x86-64 both fences are compiler barriers only, so leaving them out rarely
shows up in testing there. On ARM64 (including Windows on ARM) they are real
barriers (`dmb`), and leaving them out allows torn data to pass the recheck.

Per binding:

- **C/C++:** `std::atomic_thread_fence(std::memory_order_release)` and
  `std::atomic_thread_fence(std::memory_order_acquire)`. The C++ binding wraps
  them as `detail::seqWriteBegin()` and `detail::seqReadBeforeRecheck()`.
- **C#:** .NET has no standalone acquire or release fence; use
  `Thread.MemoryBarrier()` or `Interlocked.MemoryBarrier()` (full fences) at
  both points. `Volatile.Read` / `Volatile.Write` alone are not enough.
- **Rust:** `std::sync::atomic::fence(Ordering::Release)` and
  `fence(Ordering::Acquire)`.

The protected bytes are copied with plain loads and stores that race with the
other process. In ISO C++ that is formally a data race even with these fences;
the bindings rely on MSVC, Clang and GCC on x86-64 and ARM64 treating those
accesses as ordinary machine loads and stores, which they do. The counter
rechecks discard any torn copy.
