# Channel model

The vocabulary every irTactile telemetry producer and consumer shares: what a
*channel* is, how it is named, and which channels exist in the standard tables.
It is independent of any transport. telemetry-shm (planned) carries channels
over shared memory, and irTactile's own signal bus uses the same kinds, flags
and name rules.

Normative sources in this directory:

| File | Content |
|---|---|
| `channels/core.json` | `core.*` - required in every simulator adapter |
| `channels/ext.json` | `ext.*` - optional, same meaning in every sim that provides it; `ext::Surface` |
| this file | kinds, flags, naming rules, append-only rule |

The generated tables (`bindings/cpp/include/irtactile/telemetry/core_channels.h`,
`ext_channels.h` and the C# equivalents under
`bindings/csharp/src/IrTactile.Shm/Telemetry/`) are produced from the JSON by
`tools/gen_channels/gen_channels.py` and committed. CI regenerates them and
fails on a difference. `channel_model.h` / `ChannelModel.cs` (kinds, flags, name
rules) are hand-written and implement this file.

## Values

A channel value is a `float32` in physical units (as given by `unit`). Booleans
are `0.0 / 1.0`; enums and the gear are exact integers stored as floats. Every
channel has one **validity bit per frame**: a producer that does not deliver a
value in a frame leaves the channel invalid. The gear has no in-band "unknown"
value; an unknown gear is an invalid `core.drivetrain.gear`.

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

All `core.*` channels are required (flag clear). All `ext.*` channels are
`OPTIONAL`.

## Names

```
<namespace>.[<owner>.]<path...>[.<suffix>][[<index>]]
```

| Namespace | Meaning | Owner segment |
|---|---|---|
| `core.*` | required in every sim adapter | none |
| `ext.*` | optional, standardized across sims | none |
| `sim.<simId>.*` | specific to one simulator or external source | `simId` |
| `plugin.<pluginId>.*` | output of a signal plugin | `pluginId` |

Rules (implemented by `isValidChannelName()` in each binding):

- At most **63 characters** (the descriptor field is 64 bytes including the NUL).
- Segments are separated by `.`; no empty segment, no leading or trailing dot.
- Characters in every segment but the last: `[a-z0-9_]`. The **last** segment
  may additionally contain upper-case letters (`FL`, `RR`, `X`).
- An array element ends in `[<index>]`: decimal, no leading zeros, at most five
  digits, value 0-65535. Nothing follows the closing bracket.
- `core` and `ext` names have at least two segments, `sim` and `plugin` names at
  least three (namespace, owner, path).
- The owner segment follows the **owner id** rule: `[a-z0-9_]{1,24}`.

Reserved owner ids: `core`, `ext`, `sim`, `plugin`, `irtactile`, and the
built-in simulator ids `iracing`, `ams2`, `lmu`, `rf2`, `ac`, `acc`, `ac_evo`,
`ac_rally`, `rbr`. An external source may declare a built-in simulator id as its
`simId` (it then extends that simulator's `sim.*` namespace); it may not use the
others.

## Arrays

An array is a fixed number of consecutive channels `<name>[0] ... <name>[n-1]`
with `arrayCapacity = n` on every element. The element count a producer
currently delivers is expressed by the validity bits: elements beyond it are
invalid. In `ext.json`, wheel arrays are ordered axle by axle from front to
back, left before right.

## Per-wheel channels

Per-wheel `core.*` channels are the **4-corner view**: front axle -> `FL`/`FR`,
rearmost axle -> `RL`/`RR`. Cars with more axles additionally fill the `ext.*`
wheel arrays. A producer that fills a wheel array also fills the 4-corner
channels.

## Append-only

Channel names are part of the contract. Once published, a name keeps its unit
and meaning forever.

- A release may **add** channels to `core.json` / `ext.json`, add enum values,
  or use reserved `ext::Surface` values.
- A release never removes, renames, or changes the unit or meaning of a name.
  A retired name stays in the table, marked `"retired": true`, so it cannot be
  re-used for something else.
- `kind` and `flags` of a published name never change.
- Table order carries no meaning on the wire - consumers resolve channels by
  name. The generated tables follow the JSON order; new entries are appended.
- `core.*` grows only when a value is available in every sim. Everything else
  goes to `ext.*` or `sim.<simId>.*`.
