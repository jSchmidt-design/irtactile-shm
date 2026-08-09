# Forward compatibility - versioning rules

Applies to every irtactile shared-memory format carrying
`versionMajor` / `versionMinor` (see
[../stream-shm/v1/fields.md](../stream-shm/v1/fields.md) for the field
definitions).

## Version fields

- **`versionMajor`** - a reader must **reject** a mismatch. A major bump is a
  layout break; nothing else is promised.
- **`versionMinor`** - a reader must **tolerate** a higher value. A minor bump
  is additive only: new enum values, use of previously reserved space, a grown
  header.

## The three reader rules

A v1.x producer may use the reserved space and may grow the header, so:

1. **`reserved[]` is producer-zeroed and reader-ignored.** Zeroing is a
   producer obligation only. A reader that *validates* zero would reject a
   later producer that starts using the space, defeating the additive scheme.
2. **Never copy more than `headerBytes`.** A reader compiled against this
   version reading a larger future header must clamp its copy rather than
   trust `sizeof`. Every offset it needs is a runtime field, so a bigger
   header costs it nothing.
3. **Reject unknown enum values.** `sampleFormat` and `channelLayout` are the
   two places a minor version can legitimately add a value. A reader that
   falls through to a default instead of refusing misreads the payload
   silently.

## Behaviour is not versioned

The version fields cover the **layout**. Producer behaviour that no header field
fixes - the conditioning applied to the signal before decimation, for instance -
may change between producer builds with no version bump and nothing observable
to mark it. Depend on what the header asserts, not on what the current producer
happens to do.

