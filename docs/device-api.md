# Device API

The device's web page is a client of a small JSON API, and anything else on your network can
use it the same way: a Home Assistant sensor, a script, a dashboard. Everything is under
`http://canary.local/api/`.

If a page password is set, send it with HTTP Basic authentication (any user name). Replies
are JSON. An error is `{"ok": false, "error": "…"}` with a 4xx or 5xx status.

Values are in the profile's units (metric), whatever the page is set to show.

## Reading

| Request | Returns |
| --- | --- |
| `GET /api/status` | One snapshot of everything: device, car, current drive, health, network, sync, storage, power |
| `GET /api/live` | The current value of every signal: `{"state": "running", "v": {"rpm": 2250, "speed": 45, …}}`. Cheap; made for polling once or twice a second. |
| `GET /api/signals` | Every signal of the profile: name, unit, group, rate, where it comes from, whether this car reports it, and its current, lowest, highest and average value. With `?hist=1`, a few minutes of history per signal. |
| `GET /api/findings` | Every check: verdict, figure, sentence, limits, what it is waiting for |
| `GET /api/dtcs` | Trouble codes, when they were last read, and which control units were asked |
| `GET /api/trips` | Recorded drives, newest first |
| `GET /api/trips/{id}` | A drive's [summary](log-format.md#the-summary-json) |
| `GET /api/trips/{id}/log` | A drive's [raw recording](log-format.md#the-recording-cnl) |
| `GET /api/trips/{id}/series?sig=rpm,speed&points=300` | The named signals of a drive, reduced to at most `points` steps (20 to 800): time, mean, lowest and highest per step |
| `GET /api/config` | Settings (passwords and keys are never returned, only whether they are set) and the list of profiles |
| `GET /api/profile` | The vehicle profile in use, with inheritance resolved |
| `GET /api/events` | The activity list |
| `GET /api/devicelog` | The full activity log from the SD card, as text |
| `GET /api/support` | Every request in the profile and how the car has answered it |
| `GET /api/system` | Firmware, board, memory, SD card |
| `GET /api/wifi/scan` | Networks in range |

### `GET /api/status`

```json
{
  "device":  { "id": "canary-51m0a7", "name": "CANary", "fw": "0.1.0", "board": "wican-pro",
               "uptime_s": 2605, "time": 1795414541, "time_set": true, "tz_offset_min": -300 },
  "vehicle": { "state": "running", "state_for_s": 259, "vin": "3VV2B7AX5JM012345",
               "profile": { "id": "vw-tiguan-mk2", "name": "Volkswagen Tiguan (2018+, 2.0 TSI)", "hash": "9e697770" },
               "can_bitrate": 500000, "can_id_bits": 11, "learning": 0,
               "signals_live": 55, "signals_total": 64 },
  "battery_v": 14.25,
  "odometer_km": 165797.0, "odometer_time": 1795414521,
  "units_imperial": true,
  "trip":    { "active": true, "id": "20261123T061121Z-386f", "duration_s": 259, "distance_km": 4.03,
               "recording": true, "count": 2 },
  "health":  { "worst": "warning", "attention": 6, "assessed": 18, "monitors": 19,
               "mil": false, "dtc_count": 2 },
  "net":     { "connected": true, "ssid": "HomeNet", "ip": "10.0.10.50", "rssi": -52, "ap_active": false },
  "sync":    { "configured": true, "mode": "canary", "busy": false, "last_ok": 1795414147,
               "last_error": "", "pending_trips": 0, "odometer_sent_km": 165793.6 },
  "storage": { "present": true, "total": 31914983424, "free": 31902400512, "logging": true, "retention_days": 7 },
  "power":   { "sleep_enabled": true, "can_sleep": false, "why": "vehicle is awake" }
}
```

`vehicle.state` is one of `starting`, `detecting`, `identifying`, `ignition_on`, `running`,
`off`. `health.worst` is `ok`, `advisory`, `warning`, `critical` or `nodata`. `sync.mode` is
`canary` (Glovebox), `lubelogger` (odometer only) or `none`.

### A check, from `GET /api/findings`

```json
{ "id": "fuel_trim_idle", "title": "Fuel mixture at idle", "headline": "Running lean at idle",
  "category": "Fuel mixture", "status": "advisory", "value": 12.62, "unit": "%", "dec": 1,
  "text": "The engine is adding 12.6% fuel at idle to keep the mixture right.",
  "tpl": "The engine is adding {value} fuel at idle to keep the mixture right.",
  "stat": "mean", "seen_s": 69, "need_s": 30, "needs": "idling with a warm engine",
  "detail": "…", "causes": ["…"],
  "levels": [ { "severity": "warning", "op": "gt", "value": 15 }, { "severity": "advisory", "op": "gt", "value": 9 } ] }
```

`status` adds two values to the verdicts: `nodata` (waiting; `seen_s` of `need_s` gathered)
and `unavailable` (the car does not report something the check needs; `why` names it).

## Changing things

| Request | Does |
| --- | --- |
| `POST /api/config` | Changes settings. Send only what should change, in the shape `GET /api/config` returns. Passwords and keys are written as `ui_password`, `wifi.ap_pass`, `wifi.networks[].pass` and `glovebox.api_key`. |
| `POST /api/sync` | Syncs with Glovebox now |
| `POST /api/wake` | Looks for the car now instead of waiting for the battery voltage to rise |
| `POST /api/time` | `{"unix": 1795414541, "tz_offset_min": -300}`. Sets the time zone, and the clock if it was never set. |
| `POST /api/ap` | Turns the device's own Wi-Fi network on for 15 minutes |
| `POST /api/reboot` | Restarts |
| `POST /api/storage/format` | Erases the SD card. Refused while a drive is being recorded. |
| `POST /api/ota` | Installs a firmware image sent as the raw request body, then restarts |
| `POST /api/diag/request` | Sends one read request to a control unit: `{"tx": "7E0", "data": "22 F1 90"}`, optionally `"rx"`. Only read services are accepted; anything else is answered with status 403. |

### Settings

```json
{
  "device_name": "CANary", "units_imperial": true, "tz_offset_min": -300,
  "wifi":     { "ap_mode": 0, "networks": [ { "ssid": "HomeNet" } ] },
  "glovebox": { "url": "http://192.168.1.20:8080", "vehicle_id": 1, "upload_logs": true,
                "odometer_unit": "mi", "odometer_offset": 0 },
  "vehicle":  { "profile": "auto" },
  "power":    { "sleep_enabled": true, "sleep_voltage": 13.0, "wake_voltage": 13.3, "sleep_delay_s": 300 },
  "log":      { "enabled": true, "retention_days": 7 }
}
```

`wifi.ap_mode` is `0` to run the device's own network only when no known network is in range
and `1` to keep it on. `vehicle.profile` is `auto` or a profile id. `glovebox.odometer_unit`
and `odometer_offset` apply only to a plain LubeLogger server.

## Example: the odometer in Home Assistant

```yaml
rest:
  - resource: http://canary.local/api/status
    scan_interval: 300
    sensor:
      - name: Tiguan odometer
        value_template: "{{ value_json.odometer_km | round(0) }}"
        unit_of_measurement: km
        device_class: distance
      - name: Tiguan health
        value_template: "{{ value_json.health.worst }}"
```

The device sleeps when the car is parked, so expect it to be unreachable most of the day.
Glovebox, which the device reports to before sleeping, is the better source for anything that
should always be there.
