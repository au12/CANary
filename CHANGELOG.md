# Changelog

## 0.1.1

Fixes from a full review before the first drive. Install it in place of 0.1.0
(`canary-wican-pro-0.1.1.bin` on the stock **System** tab, or **Device > System** on CANary).

- A drive cut short by a power loss or crash is finished on the next start and uploaded,
  instead of staying on the SD card for good.
- Recording, uploading and clean-up no longer step on each other when they happen at once.
- More than 240 drives on the card: the oldest are removed instead of piling up unseen.
- Wi-Fi: hidden networks, WPA3-only routers, and a captive portal that works with phones
  that add extras to their DNS questions. The rescue network only opens while your Wi-Fi
  settings have never worked, not on every drive.
- Local dates follow daylight saving time (Glovebox tells the device the current offset).
- A request that collides with another module on a busy bus is retried.
- Power page: the unused "sleep below" setting is gone; "wake above" is kept within 12.8–14.4 V.

## 0.1.0

The first public build. It runs and is tested against a simulated car and builds for the
WiCAN Pro, but has not yet completed a drive in a real one. Treat it as a preview: the
standard OBD-II readings follow the published standard, while the Volkswagen-specific ones
come from a community database and are unconfirmed.

**Install:** upload `canary-wican-pro-0.1.0.bin` on the stock firmware's **System** tab under
**Firmware Update**. The stock firmware goes back the same way from CANary's
**Device > System** page. `canary-wican-pro-0.1.0-usb.bin` is for flashing over USB at address
`0x0`. See [docs/install.md](docs/install.md).

### What is in it

- **Health checks while driving:** 13 from standard OBD-II readings (boost, fuel mixture,
  cooling, charging, oil and catalyst temperature) and 8 more for the Volkswagen EA888 Gen 3
  (knock, misfires, cam timing, fuel pressure). Findings by severity, each with its limits, an
  explanation and the usual causes.
- **Trouble codes** from the engine and, on Volkswagen MQB cars, eight more control units,
  with plain descriptions.
- **Live data** with a short history beside every reading.
- **Every drive recorded** to the SD card and charted on the device.
- **Glovebox:** the odometer and every drive are sent whenever the car is on Wi-Fi, and
  Glovebox builds a weekly report. Recordings are deleted from the card a week after they were
  uploaded. A plain LubeLogger server gets odometer records.
- **Vehicle profiles** in JSON that build on each other, chosen from the VIN. Each car is asked
  what it supports.
- **Read-only by construction,** quiet when the car is, asleep when it is parked.
- **A web page on the device** for phone and desktop, light and dark, metric and imperial. No
  app, no cloud.
- **Updates from the page,** a recovery mode, and a flash layout shared with the stock
  firmware so either can replace the other.
- **A simulator** that runs the same engine against a modelled car, and the tests built on it.

### Known gaps

- Not yet run on a car or on the device itself.
- Sleep current not measured.
- Cars without CAN on the diagnostic port are not supported.
