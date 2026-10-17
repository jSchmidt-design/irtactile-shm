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


## Channel model: append-only

The channel model ([../telemetry-shm/channel_model.md](../telemetry-shm/channel_model.md))
is versioned by its contents, not by a field:

- A release may add channels, add enum values and use reserved values.
- A release never removes, renames or changes the unit, kind, flags or meaning of
  a published channel name. A retired name stays in the table (`"retired": true`)
  so it is never re-used.
- A reader resolves channels by **name**; table order and any producer-chosen
  slot are not part of the contract.
- A reader meets unknown names without error (they are simply not bound), and
  must tolerate known names being absent (the channel is `OPTIONAL` or the
  producer does not provide it).
