# Vehicle profiles

A profile tells CANary three things about a car: what to ask for, how to decode the answers,
and what to check. Profiles are JSON files in [`profiles/`](../profiles). They build on each
other, so most of a new car is usually already written.

```
generic-obd2          every standard reading, 13 checks
└─ vw-mqb             control-unit addresses, cluster odometer, trouble codes of 9 modules
   └─ vw-ea888-gen3   engine-specific readings and 8 checks
      └─ vw-tiguan-mk2   VIN patterns, nothing else
```

## Trying a profile

There are three ways, from quickest to most permanent.

**In the simulator.** `canary-sim --profiles DIR` loads profiles from a folder. The simulated
car answers standard OBD-II plus the Volkswagen identifiers, which is enough to check that a
profile parses and its checks behave.

**On the device, from the SD card.** Put the file in `canary/profiles/<id>.json` on the card.
It appears under **Device > Vehicle** next to the built-in ones, and a file with the same id as
a built-in profile replaces it. No rebuild needed.

**Built in.** Add the file to `profiles/` and build. A file that is not valid JSON stops the
build, and the host tests load every profile and fail on one the engine rejects.

Two tools on the device help when working out a car. **Device > Advanced > What this car
answers** lists every request in the profile and whether the car answered it. **Ask a control
unit directly** sends a single read request and shows the raw reply.

## A small example

A profile for a model that shares an engine with one already described needs only a name and a
way to recognise it:

```json
{
  "schema": 1,
  "id": "vw-golf-mk7-gti",
  "name": "Volkswagen Golf GTI (2015+)",
  "description": "Seventh-generation Golf GTI with the EA888 Gen 3 2.0 TSI.",
  "extends": ["vw-ea888-gen3"],
  "match": { "vin": ["3VW??7AU*", "WVW???AU*"], "priority": 20 }
}
```

## Top level

| Key | Meaning |
| --- | --- |
| `schema` | `1` |
| `id` | Lower-case letters, digits, `-` and `_`. The file is `<id>.json`. |
| `name`, `description` | Shown under **Device > Vehicle** |
| `extends` | Profiles to build on, applied in order before this one |
| `match` | `vin`: patterns where `?` stands for one character and `*` for any number. `priority`: among all matching profiles the highest wins (default 10). A profile without `match` is only used when chosen by hand or extended. |
| `can` | `bitrate` and `id_bits` (11 or 29) if the car needs them pinned. Leave out to detect. |
| `ecus` | The control units to talk to |
| `signals` | The readings |
| `monitors` | The checks |
| `remove_signals`, `remove_monitors` | Ids inherited from a parent to drop |

### How inheritance merges

- A signal or check with a new id is added.
- One with an existing id is updated key by key: only the keys you write change.
- A signal's `src` list is the exception. Your sources go **in front of** the inherited ones,
  so a car-specific source is tried first and the generic one remains as the fallback. Write
  `"replace_src": true` to discard the inherited sources instead.

`GET /api/profile` on the device returns the profile in use with everything resolved.

## Control units

```json
"ecus": {
  "engine": { "tx": "7E0", "rx": "7E8", "name": "Engine", "dtc": true },
  "abs":    { "tx": "713", "rx": "77D", "name": "Brakes / ABS", "dtc": true }
}
```

`tx` is the address requests are sent to and `rx` the address replies come from, in hex. With
`"dtc": true` the unit's trouble codes are read (UDS service 19). `name` is what people see
beside a code.

## Signals

The generic profile defines fuel rail pressure from the two standard PIDs that can carry it:

```json
{ "id": "rail_pressure", "name": "Fuel rail pressure", "unit": "kPa", "group": "fuel",
  "rate": "fast", "dec": 0,
  "src": [ { "obd": "59", "len": 16, "mul": 10 }, { "obd": "23", "len": 16, "mul": 10 } ] }
```

The EA888 profile adds the engine computer's own identifiers in front, in one line:

```json
{ "id": "rail_pressure", "src": [ { "uds": "2027", "ecu": "engine", "len": 16, "mul": 10 },
                                  { "uds": "1169", "ecu": "engine", "len": 16, "sign": true, "mul": 10 } ] }
```

| Key | Meaning |
| --- | --- |
| `id` | The name used in expressions. Reuse the generic profile's ids (`rpm`, `speed`, `coolant_temp`, `boost_actual`, …) where the meaning is the same, so the generic checks apply. |
| `name` | Shown to people |
| `unit` | Write metric units: `°C`, `kPa`, `km`, `km/h`, `L`, `L/h`, `Nm`. The interface converts them. Other units (`%`, `V`, `rpm`, `°`, `s`) are shown as written. Use `Δ°C` for a temperature difference, so it converts as a difference. |
| `group` | Heading on the Live page: `engine`, `boost`, `fuel`, `ignition`, `valve timing`, `temperatures`, `lubrication`, `electrical`, `status` |
| `rate` | `fast` (as often as the bus allows), `medium` (1 s), `slow` (10 s), `rare` (60 s), `once` |
| `dec` | Decimals to show |
| `min`, `max` | A decoded value outside this range is thrown away as implausible |
| `log` | `false` keeps the signal out of recordings |
| `src` | Where to get it; the first source the car answers is used |

### Sources

| Source | Meaning |
| --- | --- |
| `{ "obd": "0C" }` | Standard reading (service 01), PID in hex. Add `"ecu"` to ask one control unit instead of all. |
| `{ "uds": "2025", "ecu": "engine" }` | Manufacturer reading (service 22), identifier in hex, from the named control unit |
| `{ "device": "vbat" }` | Measured by the adapter itself: `vbat` (supply voltage) or `trip_time` (seconds since the drive began) |
| `{ "calc": "stft_b1 + ltft_b1" }` | Calculated from other signals with an [expression](#expressions) |

Decoding, for `obd` and `uds`:

| Key | Default | Meaning |
| --- | --- | --- |
| `bit` | 0 | Where the value starts in the reply's data, counted in bits from the first data byte |
| `len` | 8 | Its length in bits |
| `sign` | false | Two's complement |
| `lsb` | false | Bytes are least significant first |
| `mul`, `div`, `add` | 1, 1, 0 | value = raw × `mul` ÷ `div` + `add` |

## Checks

```json
{ "id": "fuel_trim_idle", "title": "Fuel mixture at idle", "alert": "Running lean at idle",
  "category": "Fuel mixture",
  "when": "rpm > 500 && rpm < 1100 && speed < 3 && coolant_temp > 70",
  "needs": "idling with a warm engine",
  "value": "fuel_trim_b1", "stat": "mean", "settle_s": 3, "min_time_s": 30,
  "unit": "%", "dec": 1,
  "levels": [ { "severity": "warning", "gt": 15 }, { "severity": "advisory", "gt": 9 },
              { "severity": "warning", "lt": -15 }, { "severity": "advisory", "lt": -9 } ],
  "summary": "The engine is adding {value} fuel at idle to keep the mixture right.",
  "ok": "Idle fuel trim is {value}.",
  "detail": "Extra fuel at idle that fades as load rises points to air getting in after the air meter.",
  "causes": [ "Vacuum or intake leak", "Leaking brake booster line" ],
  "low": { "alert": "Running rich at idle",
           "summary": "Idle fuel trim is {value}: the engine is taking fuel out to compensate.",
           "causes": [ "Leaking injector", "Purge valve stuck open" ] } }
```

| Key | Meaning |
| --- | --- |
| `title` | Names the check neutrally. It is what a passing check is called, so "Fuel mixture at idle", not "Running lean". |
| `alert` | The headline when it fails |
| `category` | Groups checks in listings |
| `when` | Expression: the check counts only while this is true. Leave out for "always". |
| `needs` | The same condition in words, completing "Needs 30 seconds of …" |
| `value` | Expression for the quantity being judged |
| `stat` | How the values are reduced to one figure (below) |
| `settle_s` | Skip this long each time `when` becomes true, to let transients pass |
| `min_time_s` | How long `when` must have held in total before there is a verdict |
| `unit`, `dec` | For display |
| `levels` | Up to six thresholds: `severity` (`advisory`, `warning`, `critical`) with one of `gt`, `ge`, `lt`, `le`. The most severe one that matches wins. |
| `summary` | One sentence when it fails. `{value}` is replaced by the figure in the reader's units. |
| `ok` | One sentence when it passes |
| `detail` | What it means, in a few sentences |
| `causes` | The usual suspects, likeliest first |
| `low` | For a check that can fail in both directions: `alert`, `summary`, `detail` and `causes` for the `lt`/`le` side |

### Statistics

| `stat` | The figure is |
| --- | --- |
| `mean`, `min`, `max` | Average, lowest, highest |
| `p50`, `p90`, `p95`, `p99` | Percentiles: `p90` ignores the worst tenth of the time, which is how a check rides out spikes |
| `last`, `first` | Latest or first value |
| `delta` | Last minus first |
| `range` | Highest minus lowest |
| `time` | Seconds for which `value` was true |
| `fraction` | Share of the counted time for which `value` was true, 0 to 1 |
| `time_to` | Seconds from the start of the drive until `value` first became true. A drive where it is already true at the start is not judged. |
| `count` | Number of times `value` went from false to true |

In Glovebox, a week's figure pools all the week's drives, except for `time_to`, `time`,
`count`, `delta` and `first`, which only mean something within one drive and report the worst
drive.

### Writing good checks

- **Gate on the conditions where the reading means something.** Fuel trims on a cold engine,
  boost at part throttle and knock retard at idle are noise.
- **Judge a statistic, not a moment.** Prefer `p90` or `p95` over `max` for anything that
  spikes.
- **Ask for enough time.** A short `min_time_s` gives verdicts sooner and wrong ones more
  often.
- **Leave room.** A healthy car should pass comfortably. An advisory is for "worth watching",
  a warning for "have it checked", critical for "stop driving it hard".
- **Say what the number is,** not what it proves. If a reading only suggests a fault, the
  headline should describe the reading and the detail should say what it may point to.
- **Write for someone who is not a mechanic.** Short sentences, no abbreviations.

## Expressions

Used by `calc`, `when` and `value`.

```
numbers          12  3.5
signals          rpm  coolant_temp
arithmetic       + - * / %        comparison   == != < <= > >=
logic            && || !          grouping     ( )
functions        abs(x)  min(a, b)  max(a, b)  if(condition, a, b)
```

Comparisons and logic give 1 or 0. A signal the car does not report, or whose last reading
has gone stale, has no value, and any expression using it has no value either. So a check
either works from real data or does not run; it never works from a guess.

The same language is implemented in Glovebox. [`profiles/tests/expr_vectors.json`](../profiles/tests/expr_vectors.json)
holds the cases both must pass; add to it when you find an edge.

## Where identifiers come from

Standard PIDs are defined by SAE J1979 and are the same for every car. Manufacturer
identifiers are not published. The Volkswagen ones here come from the
[OBDb](https://github.com/OBDb) community database (CC BY-SA 4.0), which collects what owners
have worked out. When adding a car, list several candidate sources for a reading if you are
unsure: CANary uses the first that answers and ignores the rest.

## Contributing a profile

1. Build on the closest existing profile.
2. Run it in the simulator, then on the car from the SD card.
3. Compare readings with a scan tool you trust, at idle and on a drive.
4. Open a pull request with the profile and the file from **Device > Advanced > What this car
   answers**. Say which model, year and engine it was tested on.
