# Server protocol

What the device sends to a server, for anyone who wants to receive it with something other
than Glovebox. Glovebox's side is described in its
[docs/canary.md](https://github.com/au12/glovebox/blob/CANary/docs/canary.md).

The device is always the client. It talks to exactly one server, the one entered under
**Device > Glovebox**, over HTTP or HTTPS. With HTTPS the server's certificate must chain to a
public certificate authority.

## Authentication

If an API key is set, every request carries it in an `x-api-key` header. A server may answer a
missing or wrong key with `401`, `403`, or a redirect to its login page; the device reports
all three to the user as a key problem and does not follow the redirect.

## A sync

A sync runs when the device joins Wi-Fi, when a drive ends, on **Sync now**, and every
fifteen minutes while the device stays connected. After a failure it tries again a minute
later.

```
GET  /api/canary/hello
POST /api/canary/checkin?vehicleId=N
PUT  /api/canary/profile?hash=H                  only if the server lacks the profile
GET  /api/canary/trips/{id}/status?vehicleId=N   for each drive not yet uploaded
POST /api/canary/trips/{id}/chunk?…              repeated until the drive is complete
```

### `GET /api/canary/hello`

A reply with status 200 whose JSON contains the key `canary_api` means the server implements
this protocol:

```json
{ "canary_api": 1, "server": "Glovebox", "version": "0.2.0" }
```

Anything else and the device checks whether the server is a LubeLogger: it asks
`GET /api/vehicle/odometerrecords/latest?vehicleId=N` and, if that works, adds an odometer
record through `POST /api/vehicle/odometerrecords/add` whenever the reading has gone up. No
drives are uploaded in that mode.

### `POST /api/canary/checkin?vehicleId=N`

The body describes the device and the car as they are now. A server uses what it understands.

```json
{
  "device": "canary-51m0a7", "name": "CANary", "fw": "0.1.0", "board": "wican-pro",
  "vin": "3VV2B7AX5JM012345",
  "profile": { "id": "vw-tiguan-mk2", "name": "Volkswagen Tiguan (2018+, 2.0 TSI)", "hash": "9e697770" },
  "odometer_km": 165766.4, "odometer_time": 1795409950,
  "battery_v": 12.6, "state": "off",
  "time": 1795412906, "tz_offset_min": -300,
  "pending_trips": 1,
  "storage": { "total": 31914983424, "free": 31902400512 },
  "worst": "warning", "mil": false,
  "findings": [ … ], "dtcs": [ … ]
}
```

`odometer_km` is always kilometres; `odometer_time` is when it was read. `findings` and
`dtcs` have the same shape as in a drive's [summary](log-format.md#the-summary-json).

Reply:

```json
{ "ok": true, "have_profile": false, "time": 1795412906, "tz_offset_min": -300,
  "odometer": { "recorded": true, "value": 103002, "unit": "mi" } }
```

| Field | Meaning |
| --- | --- |
| `have_profile` | `false` asks the device to upload the profile named in the check-in |
| `time` | The server's clock in Unix seconds. The device also reads the `Date` header. Either sets the device's clock if it has none. |
| `odometer` | What the server did with the reading, shown to the user |
| `tz_offset_min` | Optional. The server's offset from UTC today, in minutes. Sent only by a server that knows its time zone; the device adopts it, which keeps local dates right across daylight saving changes. |

### `PUT /api/canary/profile?hash=H`

The body is the resolved vehicle profile as JSON. `H` is the FNV-1a 32-bit hash of the body,
as eight hex digits; it is the hash named in check-ins and in every recording's header, so a
server can keep several versions side by side.

### `GET /api/canary/trips/{id}/status?vehicleId=N`

```json
{ "ok": true, "received": 65536, "complete": false }
```

`received` is how many bytes of this drive's recording the server holds. The device continues
from there.

### `POST /api/canary/trips/{id}/chunk?vehicleId=N&offset=O&total=T&crc=C&device=D&start=S`

The body is the next piece of the recording, raw (`application/octet-stream`).

| Parameter | Meaning |
| --- | --- |
| `offset` | Position of this piece in the recording |
| `total` | Size of the whole recording |
| `crc` | CRC-32 of this piece (polynomial 0xEDB88320, as in zlib), in hex |
| `device` | The device's id |
| `start` | The drive's start time in Unix seconds, `0` if unknown |

Reply, as for the status request. If `offset` is not where the server is, it changes nothing
and says where it is; the device carries on from there. The piece that makes `received` reach
`total` completes the drive. The device sends pieces of 64 KB.

Uploading the same drive again is harmless: a server that already has it answers
`"complete": true` straight away.

## Errors

Any reply that is not a 200 with the expected JSON ends the sync. The device shows the reason
under **Device > Glovebox**, keeps everything, and tries again at the next opportunity.

## After the upload

The device notes when each drive was fully uploaded. After the number of days set under
**Device > Storage** (seven by default) it deletes the recording from the card and keeps the
summary.
