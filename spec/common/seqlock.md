# The `layoutSeq` seqlock

`layoutSeq` guards the **layout fields** of the header (see
[concurrency.md](concurrency.md), *Layout fields*). It allows the producer to rewrite the
section geometry in place - a restart into the same section - while readers
hold the mapping open.

## Reading

```
do {
    s1 = acquire_load(layoutSeq);
    if (s1 & 1) continue;              // rewrite in progress
    copy the header, up to headerBytes;
    s2 = acquire_load(layoutSeq);      // acquire fence before re-reading
} while (s1 != s2);
```

A snapshot is valid only if it was taken while `layoutSeq` was even and
unchanged across the copy.

Copying the whole header (clamped to `headerBytes`) is fine and is what the
C++ reader does: the bytes of the concurrent range land in the snapshot too,
but they are **meaningless there** - the concurrent fields change on every
publish without any `layoutSeq` movement, so those bytes may be mid-store
garbage. A reader must take concurrent values only from live per-field
acquire loads, never from a snapshot.

## Rules

- **Re-read `layoutSeq` on every read cycle, not only at attach.** It is free:
  the concurrent range sits in one cache line with `writeSeq`, which you are
  fetching anyway. If it changed, re-take the snapshot before using any
  offset - the copy you hold may name a layout the producer no longer has.
- **Every block copy is checked against the snapshot's epoch** (`layoutEpoch`
  in the block descriptor; see
  [../stream-shm/v1/fields.md](../stream-shm/v1/fields.md)). A block published
  under a different epoch is discarded - it defends against stale-geometry
  aliasing after a rewrite.
- **Writer side:** odd while rewriting, even when stable. A consumer never
  writes it. A producer restarting into an existing section **must cycle the
  seqlock even when it rewrites nothing** - the bump is the only restart
  signal a holding consumer gets - and must step **past** a leftover odd
  value (a predecessor crashed mid-rewrite) so the final value is even and
  strictly greater than anything a spinning reader has seen. See the
  format's publish protocol, *Restarting into an existing section*.

The per-read protocol that consumes the snapshot is in
[../stream-shm/v1/README.md](../stream-shm/v1/README.md) - "The reader
protocol".
