# Hardware

## MeatPi WiCAN Pro

The board CANary is written for. An ESP32-S3 (16 MB flash, 8 MB PSRAM) with a CAN transceiver
on the OBD-II connector's high-speed CAN pins, a microSD slot, a battery-backed clock, an RGB
status light and a button. Pin assignments follow MeatPi's open
[firmware and schematic](https://github.com/meatpiHQ/wican-fw).

| Part | Pins | Used by CANary |
| --- | --- | --- |
| CAN transceiver | TX GPIO 2, RX GPIO 1, standby GPIO 38 | Yes: all communication with the car. Put in standby when asleep. |
| Supply voltage sense | GPIO 4 (ADC1 channel 3) through a divider | Yes: tells a running engine from a parked car, shown as battery voltage |
| microSD, 4-bit | CLK 21, CMD 47, D0 14, D1 13, D2 12, D3 48, detect 40 | Yes: recordings and the activity log |
| I2C bus | SDA 5, SCL 6 | Yes: status light and clock |
| RGB light driver | on I2C | Yes |
| Real-time clock | on I2C | Yes: keeps time while the car is off |
| Button | GPIO 8 | Yes |
| OBD interpreter chip | UART TX 16, RX 15, reset 41, ready 7, sleep 9 | No. Identified at start, left idle, and put to sleep with the rest of the board. |
| Motion sensor | on I2C, interrupt GPIO 3 | No |
| USB-C | | Serial console only. The board cannot be powered from USB. |

**The interpreter chip.** The WiCAN Pro carries a second chip that speaks the older OBD-II
protocols (K-line, J1850) as well as CAN, and the stock firmware talks to the car through it.
CANary uses the ESP32's own CAN controller instead: it is faster, and it lets the read-only
rule be enforced in one place. The cost is that cars without CAN on the diagnostic port
cannot be read. Using the chip for those is on the [roadmap](../ROADMAP.md).

**Flash layout.** Identical to the stock firmware's: two firmware slots of 5048 KB and a 6 MB
data partition. CANary keeps its settings in its own namespace of the shared settings area
and does not use the data partition, so each firmware finds its own settings untouched after
a switch.

**The SD card** is mounted as FAT. Recordings go to `canary/logs/`, the activity log to
`canary/device.log`, and optional extra vehicle profiles are read from `canary/profiles/`.
The card is never formatted without being asked.

## Other ESP32-S3 boards

Any ESP32-S3 board with a 3.3 V CAN transceiver (SN65HVD230, TJA1051 and the like) wired to
two GPIOs can run the engine and the web page:

```
cd firmware
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.generic-s3" build
```

Set the two pins under **CANary** in `idf.py menuconfig` if they are not TX 5 and RX 4. This
configuration builds in CI but has not been run on hardware. It has no SD card, so nothing is
recorded and there is nothing to upload beyond the odometer. It cannot measure the supply
voltage, so it never sleeps: power it from a supply that switches off with the ignition. The
defaults assume 16 MB of flash; a smaller module needs its own partition table.

Connect to the OBD-II connector's pin 6 (CAN high), pin 14 (CAN low) and pin 5 (ground). Pin
16 is battery voltage at all times and needs a regulator rated for automotive transients, not
a bare 12 V to 5 V module.

## Adding a board

A board is a block of pin definitions in [`firmware/main/board.h`](../firmware/main/board.h)
plus an entry in `Kconfig.projbuild`. Everything a board may lack (SD card, voltage sensing,
I2C parts, the interpreter chip) is behind a `BOARD_HAS_…` flag, and the code compiles without
it.
