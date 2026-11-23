# How it works

## The parts

```
profiles/            what to ask each car, how to decode it, what to check   (JSON)
firmware/components/canary_core/
                     the engine: bus protocol, polling, checks, drives, sync (portable C11)
firmware/main/       the WiCAN Pro around it: Wi-Fi, web server, SD card, clock, sleep
firmware/web/        the page the device serves
host/sim/            the same engine on a desktop, against a modelled car
host/tests/          automated checks that run the engine through whole drives
```

The engine knows nothing about ESP32s. It talks to the outside through one small interface
(send a CAN frame, read the clock, make an HTTP request, store a value), so exactly the same
code runs on the device, in the simulator and in the tests.

## Talking to the car

**Finding it.** CANary sends one standard request at 500 kbit/s and, failing that, 250 kbit/s,
with 11-bit and then 29-bit identifiers. Whatever the car answers is remembered for next time.
If nothing answers, the car is taken to be off and left alone.

**Identifying it.** It reads which standard readings the car supports and its VIN, then picks
a vehicle profile: the one whose VIN patterns match with the highest priority, or the one
chosen under **Device > Vehicle**.

**Learning what it answers.** A profile lists more requests than any one car will answer.
Each is tried; one the car refuses or ignores is dropped. The result is stored, so later
drives start at full speed. A reading can name several places to get the same value, most
specific first, and the first one that works is used.

**Polling.** Readings are sorted into rates: *fast* (as often as the bus allows, in turn),
*medium* (every second), *slow* (every 10 seconds), *rare* (every minute) and *once*. A
request the car has not answered within 160 ms is given up on and retried later. A reading
that has not been refreshed within its shelf life (5 seconds for fast and medium, 30 for slow,
3 minutes for rare) counts as unknown rather than as its last value.

**Trouble codes** are read when a drive starts and every ten minutes: stored, pending and
permanent codes from the engine, and the stored and pending codes of every other control unit
the profile lists.

## Read-only, by construction

Every request passes through one function that sends it only if its service is on this list:

| Service | What it reads |
| --- | --- |
| 01, 02 | Current readings and freeze frames |
| 03, 07, 0A | Stored, pending and permanent trouble codes |
| 06, 09 | Self-test results; VIN and calibration IDs |
| 19 | Trouble codes of a control unit (UDS) |
| 22 | A reading by identifier (UDS) |

Clearing codes, actuator tests, coding, adaptation and programming are different services and
cannot be sent, whatever a profile or the **Advanced** page asks for.

## Staying out of the way

| The car is | CANary |
| --- | --- |
| Running | Polls at full rate, runs the checks, records |
| Ignition on, engine off | Polls for 3 minutes, then one heartbeat request every 2 seconds, and nothing at all after 15 minutes. Opening the web page keeps full polling going while you look. |
| Stopped at a light with start-stop | Keeps the drive open for up to 5 minutes |
| Not answering | Stops asking. It looks again only when the battery voltage shows the alternator is charging. |

## Checks

A check is a few lines in a profile:

```json
{
  "id": "boost_under_target",
  "title": "Boost pressure",
  "alert": "Boost below target",
  "when": "accel_pedal > 60 && rpm > 2000 && boost_target > baro + 35",
  "needs": "hard acceleration above 2000 rpm",
  "value": "boost_target - boost_actual",
  "stat": "p90", "settle_s": 2, "min_time_s": 6, "unit": "kPa",
  "levels": [ { "severity": "critical", "gt": 35 }, { "severity": "warning", "gt": 20 }, { "severity": "advisory", "gt": 12 } ]
}
```

Twice a second the engine evaluates `when`. Once it has held for `settle_s`, `value` is added
to the check's statistic. When the conditions have held for `min_time_s` in total, the
statistic is compared with the levels. Until then the check says what it is waiting for and
how much it has seen. If a reading the check needs is one the car does not report, the check
is marked not available.

Using a statistic over the right conditions, rather than a momentary value, is what keeps the
findings calm: one gear change does not raise a boost warning, and an engine that is still
cold is not judged on its fuel trims.

Checks start afresh with every drive. The device's Health page shows the latest drive;
Glovebox combines the week.

## Drives

A drive begins when the engine starts and ends when the ignition goes off. Everything the car
reports during it is written to one text file on the SD card, each reading with its time. A
reading that has not changed is written again only now and then, which keeps a twelve-minute
commute around half a megabyte. The file is flushed every five seconds, so pulling the
device out loses at most that. Drives shorter than 20 seconds are discarded.

When a drive ends, a small summary is written next to it: distance, duration, fuel used,
every check's verdict and the trouble codes seen. The format of both files is in
[log-format.md](log-format.md).

## Syncing

When the device joins a known Wi-Fi network, and again whenever a drive ends in range:

1. It asks the server what it is. Glovebox answers that it speaks CANary. A plain LubeLogger
   server gets odometer records through its standard API and nothing more.
2. It checks in with the odometer reading and its state. Glovebox records the odometer.
3. It uploads the vehicle profile if the server does not have it.
4. It uploads each drive not yet on the server, 64 KB at a time. A lost connection resumes
   from where the server says it stopped.

A week after a drive was uploaded (configurable under **Device > Storage**), its raw file is
deleted from the card. If the card runs nearly full before that, the oldest recordings go
first, uploaded ones before the rest.

The clock comes from the WiCAN Pro's battery-backed clock, corrected by network time, by the
server's reply, or by your browser the first time the page is opened. A drive recorded before
the device ever knew the time is dated once it does.

Nothing is sent anywhere except the server you enter. There is no account and no telemetry.

## Sleep

Parked, CANary waits five minutes, finishes any upload, then turns off Wi-Fi, puts the board's
other chips to sleep and dozes, waking for a moment every two seconds to look at the battery
voltage. It restarts when the voltage rises the way it does when an alternator starts
charging (above 13.3 V after having been lower, or a jump of 0.6 V), or when the button is
pressed. If the battery falls below 11.6 V it goes to sleep straight away. The thresholds are
under **Device > Power**.

On a board that cannot measure the battery, CANary cannot tell when to wake and so does not
sleep; power such a board from a switched supply.

## In Glovebox

Glovebox keeps the raw file of every drive and runs the profile's checks over it again with
its own implementation of the same rules. A week's verdict comes from the week's drives taken
together, so checks that rarely get enough from one commute still reach a verdict. The two
implementations are held together by shared test vectors and by a recorded drive that both
must judge the same way. See [server-protocol.md](server-protocol.md) and Glovebox's
[docs/canary.md](https://github.com/au12/glovebox/blob/CANary/docs/canary.md).
