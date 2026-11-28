# Log format

Each drive leaves two files in `canary/logs/` on the SD card:

- `<id>.cnl`, the raw recording: every reading with its time, as text
- `<id>.json`, a summary written when the drive ends

A drive's id is its start time in UTC plus four random hex digits, for example
`20261121T150807Z-b595`. A drive that began before the device knew the time is named
`nt-` plus eight hex digits and gets its date when the clock is set.

## The recording (`.cnl`)

Plain text, one item per line, UTF-8.

```
#CNL 1
#device canary-51m0a7
#fw 0.1.0
#vin 3VV2B7AX5JM012345
#profile vw-tiguan-mk2 9e697770
#trip 20261121T150807Z-b595
#start 1795273687
#sig 0 rpm rpm fast Engine speed
#sig 1 speed km/h fast Vehicle speed
#sig 6 coolant_temp °C slow Coolant temperature
…
!0 dtc {"codes":[{"code":"P0299","kind":"pending","src":2024,"module":"Engine (OBD-II)"}]}
3 8 12
6 13 98
30 1 0
35 0 812.5
…
!2575412 end {"id":"20261121T150807Z-b595","duration_s":2575,…}
```

### Header

Lines beginning with `#`, before the first reading.

| Line | Meaning |
| --- | --- |
| `#CNL 1` | Format and version. Always first. |
| `#device`, `#fw` | Which device wrote it, with which firmware |
| `#vin` | The car's VIN, or `-` if it was not read |
| `#profile <id> <hash>` | The vehicle profile and the hash of its resolved form |
| `#trip <id>` | The drive's id |
| `#start <unix>` | Start time in Unix seconds, or `0` if the clock was not set |
| `#sig <n> <id> <unit> <rate> <name…>` | One line per signal of the profile: its column number, id, unit (`-` for none), polling rate and name. Every signal of the profile is listed, including ones this car never answers. |

### Readings

```
<milliseconds since the drive began> <signal number> <value>
```

Values are in the profile's units (metric), with up to six significant digits.

A reading is written when its value changes. While it stays the same it is written again at
intervals, so that a reader can tell "unchanged" from "stopped answering":

| Rate | Repeated every | Treat as stale after |
| --- | --- | --- |
| fast, medium | 2 s | 5 s |
| slow | 12 s | 30 s |
| rare | 72 s | 180 s |
| once | 60 s | never |

To reconstruct the car's state at a moment, take each signal's most recent reading and discard
those older than the stale time. This is what the checks do, on the device and in Glovebox.

### Events

```
!<milliseconds> <kind> <JSON>
```

| Kind | When | Content |
| --- | --- | --- |
| `dtc` | Each time trouble codes are read | `codes`: a list of `code`, `kind` (`stored`, `pending`, `permanent`), `module` (the control unit's name), and where known `ecu` (its id in the profile), `src` (the address that answered), `ftb` (failure type byte) and `status` (UDS status byte) |
| `end` | Last line of a complete recording | The summary below |

A recording without an `end` line was cut short, for instance by unplugging the device. It is
still valid up to its last complete line. Readers should ignore event kinds they do not know.

## The summary (`.json`)

```json
{
  "id": "20261122T164406Z-78a2",
  "start": 1795365846,
  "duration_s": 526,
  "distance_km": 7.13,
  "odo_start_km": 166612.4, "odo_end_km": 166619.4,
  "max_speed": 68,
  "fuel_l": 0.5,
  "samples": 33731, "bytes": 520667,
  "vin": "3VV2B7AX5JM012345",
  "profile": "vw-tiguan-mk2", "profile_hash": "9e697770",
  "fw": "0.1.0", "device": "canary-51m0a7",
  "reason": "ignition off",
  "worst": "warning",
  "findings": [
    { "id": "coolant_overheat", "title": "Coolant temperature", "headline": "Coolant temperature",
      "category": "Cooling", "status": "ok", "value": 88, "unit": "°C", "dec": 0,
      "text": "Coolant peaked at 88 °C, within the normal range.",
      "tpl": "Coolant peaked at {value}, within the normal range." }
  ],
  "dtcs": [ { "code": "P0299", "kind": "pending", "src": 2024, "module": "Engine (OBD-II)" } ],
  "signals": { "rpm": { "min": 750, "max": 4357.8, "avg": 1968.6, "n": 9910 } },
  "uploaded_bytes": 520667, "uploaded_at": 1795366012, "raw_deleted": false
}
```

| Field | Meaning |
| --- | --- |
| `distance_km` | Integrated from vehicle speed |
| `fuel_l` | Integrated from the fuel rate, when the car reports one |
| `max_speed` | km/h |
| `samples`, `bytes` | Lines and size of the recording |
| `reason` | Why the drive ended: `ignition off`, `engine off`, `restart`, `firmware update`, … or `power lost` for a drive the device finished after it restarts: such a log has no summary of its own until then, and its `end` line and summary carry only the id, start, length and sample count; the server works out the rest from the samples. |
| `worst` | The most serious verdict: `ok`, `advisory`, `warning`, `critical`, or `nodata` |
| `findings` | Every check that reached a verdict. `status` is the verdict, `value` the figure in the profile's unit, `text` the sentence in metric units and `tpl` the same sentence with `{value}` left open so a reader can use its own units. |
| `signals` | Lowest, highest and average value of each signal, and how many readings there were |
| `uploaded_bytes`, `uploaded_at` | How much of the recording the server has, and when the upload finished (Unix seconds, `0` if not yet) |
| `raw_deleted` | The recording was removed to free space; the summary remains |

## Size

A reading is about 15 bytes. A car that answers 50 signals produces roughly half a megabyte
for a twelve-minute commute and three to four megabytes for an hour on the motorway.
Compressed, as Glovebox stores them, recordings shrink to about a third.
