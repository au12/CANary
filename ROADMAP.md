# Roadmap

Where CANary and its Glovebox side are headed, most important first. Nothing here has a date.
Items move up when someone needs them; [say so in an issue](https://github.com/au12/CANary/issues).

## 1. The first real drive

Version 0.1 has only ever met a simulated car. Before anything else:

- Flash a WiCAN Pro and confirm the basics on the 2018 Tiguan: the car is found, the VIN picks
  the right profile, drives are recorded and uploaded, the device sleeps and wakes.
- Go through **What this car answers** and fix the Volkswagen identifiers that are wrong or
  missing. Compare every reading with a scan tool, at idle and on a drive, and correct the
  scalings.
- Settle what the learned cam offsets do on this engine generation. They are recorded and
  trended; whether they say anything about the timing chain on a Gen 3 engine is an open
  question that only data from real cars answers.
- Check the hardware drivers on the device itself: SD card, clock, light, button, recovery.
- Measure the current drawn while asleep, and move to a deeper sleep if it is not negligible.

## 2. Making it dependable

- **Verified readings.** Mark each reading in a profile with the cars it has been confirmed
  on, and show unconfirmed ones as such in the interface.
- **Install from the browser.** A web installer, so a first flash needs a USB cable and
  nothing else.
- **Updates without downloading files.** The device checks GitHub for a newer release and
  offers it, when asked to.
- **Recording without an SD card.** Keep the current drive in memory and upload it at the next
  Wi-Fi contact.
- **Faster polling.** Ask for several readings in one request where the control unit allows.
- **Profile updates without reflashing.** Fetch new and corrected vehicle profiles from the
  repository.

## 3. Seeing more

- **The car's own self-tests** (OBD-II service 06): catalyst efficiency, oxygen sensor
  response, evaporative system leak tests and the misfire monitor report how close they are to
  failing, long before a code is set.
- **Freeze frames:** the conditions under which a trouble code was stored, kept with the code.
- **Battery health:** resting voltage after the car has stood for hours, and how far the
  voltage dips while cranking, trended week to week.
- **Fill-ups:** recognise a refuel from the fuel level and offer it to Glovebox as a fuel
  record to complete.
- **Diesels:** soot load, regeneration frequency and duration, EGR.
- **Hybrids and electric cars:** drives that begin without an engine starting, and battery
  state-of-health readings.

## 4. More cars

- More of the Volkswagen group on the existing base: Golf GTI and R, Jetta, Atlas, Audi A3, A4
  and Q5, and the earlier EA888 generations, where the cam adaptation is an established
  indicator of chain stretch.
- **BMW F-series, starting with the N55.** Most of the engine is in place; what is missing is
  the profile and BMW's way of addressing control units.
- **A discovery tool** that asks a control unit, with the engine off and only when told to,
  which identifiers it answers. Writing a profile for a new car then starts from a list
  instead of from nothing.
- **Cars without CAN on the diagnostic port,** through the WiCAN Pro's interpreter chip.
- A place to share profiles and the cars they were confirmed on.

## 5. Glovebox

- **Merge the CANary section into the main line,** with a setting to hide it for vehicles that
  have no device.
- **Backups.** Include CANary's data in Glovebox's built-in backup and restore.
- **Notes on the timeline.** Mark a repair ("diverter valve replaced") so that the trend lines
  show what it changed.
- **From finding to task.** Turn a finding into a reminder or a planned repair in one click.
- **Export.** A drive or a signal as CSV.
- **Home Assistant.** Publish each car's odometer, verdict and findings, so they can be used in
  dashboards and automations without the device being awake.

## 6. Glovebox for people who do not run servers

Today Glovebox is a Docker container, which suits people who already run a home server and
nobody else. CANary is only as approachable as the thing it reports to, so the aim is a
Glovebox that installs like any other program.

**What it has to do**

- Install on Windows, macOS and Linux from an ordinary installer, with nothing else to set up.
  Glovebox needs no separate database by default, so there is nothing to install beside it.
- Run quietly in the background from the tray or menu bar, start with the computer, and open
  in the browser when clicked.
- Keep its data in the user's own folder, say where that is, and back it up on a schedule to a
  place the user picks.
- Update itself.

**What it needs from CANary, and the other way round**

- **No addresses to type.** Glovebox announces itself on the home network, and the device
  lists the servers it can see.
- **No keys to copy.** The device asks to join; Glovebox shows "A CANary on the Tiguan wants
  to connect" and one click approves it.
- **A server that is not always on.** A laptop is asleep when the car comes home. The device
  already keeps every drive until it has been delivered, and waits a week after that before
  freeing the space. What remains is to make "caught up after three days away" look normal on
  both sides rather than like a fault.

**In steps**

1. Self-contained builds for Windows, macOS (Apple silicon and Intel) and Linux, published
   with each release. Unzip, run, browser opens. Data in the user's application-data folder.
2. The tray and menu-bar shell: start at login, open, quit, show the data folder.
3. Installers: a signed installer for Windows, a signed and notarised disk image for macOS, an
   AppImage and a `.deb` for Linux. Then the package managers (winget, Homebrew).
4. Finding and pairing without typing, as above.
5. Scheduled backups and automatic updates.

**An alternative for the same people:** a Home Assistant add-on. Many WiCAN owners already
run Home Assistant, it is always on, and installing an add-on is a few clicks.

## Not planned

- **Anything that writes to the car.** Clearing codes, actuator tests, coding and adaptations
  are deliberately out of reach of this firmware and will stay there.
- **A cloud service.** Your car's data stays on your network.
- **A phone app.** The web page works on a phone and can be added to the home screen.
