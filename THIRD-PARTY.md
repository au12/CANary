# Third-party notices

CANary is licensed under the GNU General Public License v3.0 or later (see [LICENSE](LICENSE)).
It includes or builds on the following work by others.

## Included in this repository

| What | Where | License |
| --- | --- | --- |
| [cJSON](https://github.com/DaveGamble/cJSON), © Dave Gamble and cJSON contributors | `host/third_party/cJSON` (desktop builds) | MIT, see the `LICENSE` file beside it |
| [Barlow](https://github.com/jpt/barlow) typeface, © The Barlow Project Authors | `firmware/web/barlow-*.woff2` | SIL Open Font License 1.1, see [docs/licenses/OFL-Barlow.txt](docs/licenses/OFL-Barlow.txt) |

## Built into the firmware

| What | License |
| --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) and the components it bundles (FreeRTOS, lwIP, Mbed TLS, FatFs, cJSON and others) | Apache-2.0 for ESP-IDF; each bundled component under its own license, listed in ESP-IDF's documentation |
| [esp-protocols mDNS](https://github.com/espressif/esp-protocols) | Apache-2.0 |

## Information this project relies on

**MeatPi wican-fw.** Board support for the WiCAN Pro (pin assignments, flash layout, the
interpreter chip's handshake) follows MeatPi's open firmware and schematic,
<https://github.com/meatpiHQ/wican-fw>, licensed GPL-3.0. CANary is a separate firmware. It is
not affiliated with, endorsed by or supported by MeatPi; please do not ask them for help with
it.

**OBDb.** The Volkswagen-specific identifiers and scalings in `profiles/vw-*.json` are derived
from the OBDb community database, <https://github.com/OBDb>, licensed
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/). Those profile files are
adaptations of that work. They are offered here under GPL-3.0-or-later, which Creative Commons
lists as a compatible license for adaptations of BY-SA 4.0 material, and they remain available
under CC BY-SA 4.0 as well.

**Standards.** Standard readings and trouble-code numbering follow SAE J1979 / ISO 15031-5 and
SAE J2012 / ISO 15031-6. The descriptions of trouble codes in `firmware/web/dtc.json` were
written for this project.

## Trademarks

Volkswagen, Audi, Škoda, SEAT, BMW, WiCAN, MeatPi and other names are trademarks of their
owners and are used only to say which hardware and vehicles this software works with.
