# Development

Almost everything can be built, run and tested on a desktop without the hardware. The engine
is plain C11 with no dependency on ESP-IDF, and the simulator puts the real web page in front
of it.

## Desktop: simulator and tests

Needs a C compiler, CMake and Ninja (or Make). Developed on Linux; macOS is built in CI as well.

```
cmake -S host -B build-host -G Ninja
cmake --build build-host
build-host/canary-tests
```

The tests drive the whole engine through simulated drives: bus detection, learning what a car
answers, every check with and without the fault it looks for, recording, interrupted uploads,
clock corrections, sleep decisions. They finish in a few seconds.

Run the simulator from the repository root, since it serves `firmware/web` and loads
`profiles/` from there:

```
build-host/canary-sim --scenario worn
```

Then open `http://localhost:8088`.

| Option | Meaning |
| --- | --- |
| `--scenario healthy\|worn\|generic` | A car with nothing wrong; one with a boost leak, a lean idle, one knocking cylinder and learned cam offsets; or a non-Volkswagen that answers only standard OBD-II on 29-bit identifiers |
| `--days N` | Replay N days of ordinary use flat out before serving, ending now: weekday commutes, errands, a motorway trip at the weekend |
| `--wear PERCENT` | With `--days`, the faults grow by this much a day, so trends have something to show |
| `--fast-forward SECONDS` | Replay that much of a looping commute instead |
| `--minutes N` | Length of the looping commute (default 12) |
| `--speed X` | Run the clock X times faster |
| `--tz MINUTES` | The device's time zone, as an offset from UTC |
| `--wifi home\|always\|never` | When the simulated Wi-Fi is connected: only while parked (default), always, never |
| `--glovebox URL --vehicle N [--key K]` | Sync with a real Glovebox |
| `--state DIR` | Where settings and the "SD card" live (default `/tmp/canary-sim`) |
| `--port`, `--profiles`, `--web` | Listening port and source folders |

To fill a Glovebox with two weeks of history for a car that is getting worse:

```
build-host/canary-sim --scenario worn --days 14 --wear 2.5 --tz -300 \
    --glovebox http://localhost:8080 --vehicle 1
```

## The web page

`firmware/web` is plain HTML, CSS and JavaScript with no build step and no dependencies. The
simulator serves the files straight from disk, so edit and reload. For the firmware,
`tools/gen_assets.py` compresses them into the image at build time.

Unit conversion and the limit gauge exist twice, in `firmware/web/app.js` and in Glovebox's
`CanaryFormat.cs`. Change them together.

## Firmware

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/).
CANary is developed and released with version 5.5.1.

```
cd firmware
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The first build creates `sdkconfig` from `sdkconfig.defaults` for the WiCAN Pro. The app image
is `build/canary.bin`, which is the file the update page takes.

```
firmware/main/app_main.c   start-up, recovery mode, the button, going to sleep
firmware/main/plat.c       the engine's platform interface on ESP-IDF: CAN, HTTP client, settings
firmware/main/net.c        Wi-Fi: known networks, own access point, captive portal, mDNS, network time
firmware/main/web.c        the HTTP server, firmware update
firmware/main/hal.c        voltage, light, clock, the board's other chips
firmware/main/sdcard.c     the SD card
firmware/main/board.h      pin maps
```

## Keeping Glovebox in step

Glovebox analyses recordings with its own C# implementation of the expression language and the
check engine. Three things keep the two from drifting:

- `profiles/tests/expr_vectors.json`: expression cases that both must pass.
- A recorded drive in Glovebox's `tools/CanarySelfTest/fixtures`, with the verdicts the device
  reached on it. Glovebox's build fails if the server would decide differently.
- This rule: a change to how checks are evaluated (`monitors.c`), to the log format
  (`trips.c`, `vehicle.c`) or to the expression language (`expr.c`) is not finished until the
  matching change is in Glovebox and the fixtures are refreshed.

To refresh the fixtures:

```
build-host/canary-sim --scenario worn --state /tmp/fx --fast-forward 1200 --wifi never
# stop it once it is serving, then from the newest pair in /tmp/fx/sd/logs:
#   <id>.json                     -> fixtures/device-summary.json
#   gzip -9 -n -c <id>.cnl        -> fixtures/trip.cnl.gz
#   curl localhost:8088/api/profile -> fixtures/profile.json   (with the simulator running)
#   profiles/tests/expr_vectors.json -> fixtures/expr_vectors.json
```

## Generated documentation

`docs/checks.md` is written from the profiles:

```
python3 tools/gen_checks_doc.py
```

CI fails if it is out of date.

## Releases

A release is cut by changing `firmware/version.txt` on `main`. The Release workflow then
builds the firmware, takes the notes from the matching `## <version>` section of
`CHANGELOG.md`, and publishes `v<version>` with:

- `canary-wican-pro-<version>.bin`, the image for the update page
- `canary-wican-pro-<version>-usb.bin`, bootloader, partition table and image in one file for
  flashing at address `0x0`
- `SHA256SUMS`

Write the changelog section before changing the version; the workflow fails without it.
Versions below 1.0 and versions with a suffix (`0.3.0-rc1`) are published as pre-releases.

## Conventions

- The engine (`canary_core`) must stay free of ESP-IDF, of globals and of anything that keeps
  it from running in the simulator. New hardware needs go through `cn_platform`.
- Everything sent to the car goes through `cn_isotp_request`, which enforces the read-only
  service list. Do not add a second way to transmit.
- Text shown to people is written for someone who is not a mechanic and not a programmer.
- A new behaviour comes with a test that runs it through the simulated car.
- Match the surrounding style: four spaces, a function's opening brace on its own line, lines up to about 140 characters. Comments say why, not what.
- Source files carry an SPDX line: `GPL-3.0-or-later`.
