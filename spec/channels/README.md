# Channel model

The vocabulary every irTactile telemetry producer and consumer shares: what a
*channel* is, how it is named, and which channels exist in the standard tables.
It is independent of any transport. [telemetry-shm](../telemetry-shm/v1/README.md) carries
channels over shared memory, and irTactile's own signal bus uses the same kinds, flags
and name rules.

Normative sources in this directory:

| File | Content |
|---|---|
| `std.json` | `std.*` - the standard table: scalars, wheel / axle arrays, `std::ConnectionStatus`, `std::Surface` |
| this file | kinds, flags, array layouts, provenance, naming rules, append-only rule |

`tools/gen_channels/gen_channels.py` generates the tables from the JSON:
`bindings/cpp/include/irtactile/shm/channels/std.h` (namespace
`irtactile::shm::channels::standard`) and
`bindings/csharp/src/IrTactile.Shm/Channels/StdChannels.g.cs` (class `Std`). Both are
committed, and CI fails if regenerating them gives a different result.
`channels/model.h` and `ChannelModel.cs` (kinds, flags, name rules) are hand-written and
implement this file.

## Values

A channel value is a `float32` in physical units (as given by `unit`). Booleans
are `0.0 / 1.0`; enums and the gear are exact integers stored as floats. Every
channel has one **validity bit per frame**: a producer that does not deliver a
value in a frame leaves the channel invalid. The gear has no in-band "unknown"
value; an unknown gear is an invalid `std.drivetrain.gear`.

`rangeMin` / `rangeMax` are **nominal**: the default normalization range for
generators and the display range of editors. They are not clamps.

## Kinds

| Value | Name | Meaning |
|---|---|---|
| 0 | `CONTINUOUS` | physical quantity sampled per frame |
| 1 | `DISCRETE` | integer-valued (gear, enum, bool) |
| 2 | `EVENT` | impulses; valid only in frames that carry an event, the amplitude is the value |

The kind describes the data, not how it is played back. Consumers may default
their interpolation from it (`linear`, `hold`, `event`).

## Flags

| Bit | Name | Meaning |
|---|---|---|
| 0 | reserved | producers write 0, readers ignore |
| 1 | `OPTIONAL` | the producer may leave the channel invalid for the whole session |

Every `std.*` channel is `OPTIONAL`: availability is decided by what a source declares, and any value may be
invalid in a frame (the car lacks the quantity).

## Names

```
<namespace>.[<owner>.]<path...>[.<suffix>][[<index>]]
```

| Namespace | Meaning | Owner segment |
|---|---|---|
| `std.*` | the standard table; same meaning in every source that provides it | none |
| `sim.<simId>.*` | specific to one simulator or external source | `simId` |
| `plugin.<pluginId>.*` | output of a signal plugin | `pluginId` |

Rules (implemented by `isValidChannelName()` in each binding):

- At most **63 characters** (the descriptor field is 64 bytes including the NUL).
- Segments are separated by `.`; no empty segment, no leading or trailing dot.
- Characters in every segment but the last: `[a-z0-9_]`. The **last** segment
  may additionally contain upper-case letters (`FL`, `RR`, `X`).
- An array element ends in `[<index>]`: decimal, no leading zeros, at most five
  digits, value 0-65535. Nothing follows the closing bracket.
- `std` names have at least two segments, `sim` and `plugin` names at
  least three (namespace, owner, path).
- The owner segment follows the **owner id** rule: `[a-z0-9_]{1,24}`.

Reserved owner ids: `std`, `sim`, `plugin`, `irtactile`, and the
built-in simulator ids `iracing`, `ams2`, `lmu`, `rf2`, `ac`, `acc`, `ac_evo`,
`ac_rally`, `rbr`. An external source may declare a built-in simulator id as its
`simId` (it then extends that simulator's `sim.*` namespace); it may not use the
others.

## Arrays

An array is a fixed number of consecutive channels `<name>[0] ... <name>[n-1]`
with `arrayCapacity = n` on every element.

**Capacity versus count.** The table fixes the *capacity*: the most elements the
name may have. A source declares the array with the number of elements it
fills, as `arrayCapacity` on every element descriptor; elements beyond that
count are **not provided** (like a whole channel the source does not declare),
not "invalid in every frame". A descriptor of a table array is valid when
`1 <= arrayCapacity <= table capacity` and `arrayIndex < arrayCapacity`. The
capacity is therefore not a frozen contract: a table may raise it later and
producers built against the old value keep working.

**Layout.** Every array has a layout, a table property that the descriptor
repeats (it must equal the table entry):

| Layout | Value | Element order |
|---|---|---|
| `index` | 0 | free; also the layout of every scalar |
| `wheel` | 1 | axle by axle front to back; within an axle left to right |
| `axle` | 2 | front to back |

The element order of a `wheel` array and the element count of an `axle` array
follow the vehicle's *wheel layout*, which the producer publishes in the header
(`wheelsPerAxle`, front to back: car `2,2`; motorcycle `1,1`; truck with dual
rear tyres `2,4,4`). Names on the wire keep numeric indices only; corner labels
such as `[FL]` are a consumer feature (irTactile bindings) and not part of the
channel model.

## Provenance

A descriptor states who computed the value: `0` unspecified (read as `1`),
`1` native (delivered by the sim API), `2` derived (exact computation from
native values), `3` estimated (approximation). The producer's statement; it is
not part of the table and consumers do not compare it with anything.

## Append-only

Channel names are part of the contract. Once published, a name keeps its unit
and meaning forever.

- A release may **add** channels to `std.json`, add enum values,
  or use reserved `std::Surface` values.
- A release never removes, renames, or changes the unit or meaning of a name.
  A retired name stays in the table, marked `"retired": true`, so it cannot be
  re-used for something else.
- `kind` and `flags` of a published name never change.
- Table order carries no meaning on the wire - consumers resolve channels by
  name. The generated tables follow the JSON order; new entries are appended.
- `std.*` grows only when at least two sources deliver a value with the same
  unit, sign and meaning. Everything else goes to `sim.<simId>.*`.
  Rows present at 1.0 that predate the rule say so in their `description`.
- Until the first release a name may still be deleted instead of marked
  `"retired": true`.

## Axes and signs

Every source converts to these conventions; the sim API's own convention never
reaches the bus. The `description` of each `std.json` entry states the meaning,
sign and zero point of that channel in one sentence; the rules below are the
shared part.

- Vehicle axes follow ISO 8855: `lon` + forward, `lat` + left, `vert` + up.
- Rates: yaw + = counter-clockwise seen from above (turning left), pitch + =
  nose down, roll + = right side down. Orientation angles use the same signs.
- `vehicle.accel.vert` has gravity removed (0 at rest).
- Pedals (`throttle`, `brake`, `clutch`, `handbrake`): 0 = released, 1 = fully
  pressed. `input.steering` and `steering.torque`: + = turned (turning) left.
- `assist.*_active`: 1 = the system is intervening now, not "switched on".
- `suspension.deflection`: + = compression, relative to the sim's reference
  position (not normalised); `suspension.velocity` is its derivative.
- `wheel.speed` and `wheel.surface_speed`: + = rolling forward.
  `wheel.slip_ratio` = (omega*r - v) / |v|: 0 = free rolling, -1 = locked,
  > 0 = wheelspin. `wheel.slip_angle`: + = contact-patch velocity to the left
  of the wheel heading. `wheel.force.*`: force on the tyre from the road in the
  wheel's own ISO axes.
- `tyre.temp.inner` / `.outer`: inner is the side toward the car's centre line
  on both sides of the car.
- Channels are provided only when the source delivers the value: no
  differentiating, integrating or estimating to fill a slot (a producer
  states `provenance` for what it does convert).

