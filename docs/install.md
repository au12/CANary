# Install and first setup

CANary replaces the firmware on a MeatPi WiCAN Pro. It uses the same flash layout as the stock
firmware, so it installs through the stock firmware's own update page, and the stock firmware
goes back on the same way. Settings of the two do not touch each other.

## Before you start

- **A WiCAN Pro** plugged into the car's OBD-II port. It is powered by the car and cannot be
  powered over USB-C alone.
- **A microSD card** in the device's slot, formatted FAT32. A card formatted exFAT (most cards
  larger than 32 GB come that way) is not readable until it has been formatted once from
  **Device > Storage**. Without a card the checks run but nothing is recorded.
- **The firmware file** `canary-wican-pro-<version>.bin` from the
  [releases page](https://github.com/au12/CANary/releases).

## Install from the stock firmware

1. Join the WiCAN's Wi-Fi network. Out of the box it is named `WiCAN_` followed by the device's
   ID, and the password is `@meatpi#`. If the device is already on your home network, use its
   address there instead.
2. Open its configuration page: `http://192.168.0.10` on a WiCAN Pro's own network.
3. Go to the **System** tab. Under **Firmware Update**, click **Choose File**, pick
   `canary-wican-pro-<version>.bin`, then **Update**.
4. Wait about a minute. The device restarts by itself.

The stock firmware writes the file to the spare firmware slot and starts from it. Its own
settings stay where they are, so going back later restores it as you left it.

## First setup

1. Join the Wi-Fi network `CANary-XXXX` (the last four characters differ per device). The
   password is `canary-setup`.
2. A phone usually opens the page by itself. Otherwise go to `http://192.168.4.1`.
3. Open **Device**:
   - **Wi-Fi.** Add your home network, or pick it from **Show networks in range**. You can add
     several; CANary joins whichever is in range, strongest first.
   - **Glovebox.** Optional. Enter the three values from the vehicle's CANary page in Glovebox.
   - **Vehicle.** Leave the profile on automatic. With the ignition on, CANary reads the VIN and
     picks the best match; this page shows which one it chose.
   - **System > Password for this page.** Optional. Anyone on the same network can open the
     page unless you set one.
4. Start the engine. The **Health** page fills in as the checks gather what they need. Some
   need a warm engine or a stretch of hard acceleration, so the first verdicts take a drive or
   two.

Once it is on your home network, the page is at `http://canary.local`, or at the address your
router gave it (shown under **Device > Wi-Fi**).

## The button and the light

| Light | Meaning |
| --- | --- |
| Green | Car awake, nothing to report |
| Amber | A warning finding |
| Red | A critical finding |
| Dim blue | Car off; CANary is finishing its work and will sleep |
| Purple | Recovery mode |
| Off | Asleep |

| Button | Does |
| --- | --- |
| Short press | Turns CANary's own Wi-Fi network on for 15 minutes. Also wakes it from sleep. |
| Hold 10 seconds | Erases all settings and restarts. Recorded drives stay on the card. |
| Hold while plugging in | Starts recovery mode |

## Updating

Download the new `canary-wican-pro-<version>.bin`, open **Device > System** and upload it. The
device ends the current drive's recording, installs the file to the spare slot and restarts.
Settings and recorded drives are kept.

## Going back to the stock firmware

Download MeatPi's firmware for the WiCAN Pro from the
[wican-fw releases](https://github.com/meatpiHQ/wican-fw/releases) and unzip it. Upload the
file named `wican-fw_obd_pro_v…p.bin` under **Device > System**, exactly like an update. The
device restarts into the stock firmware with the settings it had before.

## Recovery mode

Recovery mode runs nothing but Wi-Fi and a one-button page that installs a firmware file. It
starts when:

- the button is held while the device is plugged in, or
- the firmware failed to start properly five times in a row.

The light is purple. Join `CANary-XXXX`, open `http://192.168.4.1`, choose a file (CANary or
stock) and click **Install**.

## Flashing over USB

Only needed if neither firmware starts. This follows MeatPi's procedure for the WiCAN Pro:

1. Install the CH342 serial driver for your system if it does not have one.
2. Plug the device into the car for power, then connect USB. Two serial ports appear; use the
   first.
3. Open [ESPTool Web](https://espressif.github.io/esptool-js/) in Chrome or Edge, click
   **Connect** and choose that port.
4. Flash `canary-wican-pro-<version>-usb.bin` at address `0x0`.

That file contains the bootloader, the partition table and the firmware in one image. With
`esptool` installed, the same thing from a terminal:

```
esptool.py --chip esp32s3 write_flash 0x0 canary-wican-pro-<version>-usb.bin
```

Flashing this file resets CANary's settings. The image is one continuous block from `0x0` to the
end of the firmware, so it also covers the settings area at `0x9000` and erases it: Wi-Fi
networks, Glovebox connection and page password are gone, as after holding the button for ten
seconds, and you go through **First setup** again. Recorded drives on the SD card are kept.

It also puts CANary in the first firmware slot, which is usually where the stock firmware was.
To go back to stock afterwards, upload MeatPi's file under **Device > System** as described above.
Erasing the whole flash first removes everything, including the stock firmware's own storage.

## If something is not right

| What you see | What to try |
| --- | --- |
| No `CANary-XXXX` network | It is only on while no known network is in range. Press the button once to turn it on for 15 minutes. |
| "Looking for the car" that never ends | Turn the ignition on. CANary only talks when the car answers, and tries 500 and 250 kbit/s with both identifier lengths. A car without CAN on its OBD-II port cannot be read. |
| "No SD card found" | Reseat the card. A card formatted exFAT has to be formatted once under **Device > Storage**. |
| Glovebox: "the server asked for a login" | The server has a login and the API key is missing or wrong. Create a new one on the vehicle's CANary page in Glovebox. |
| Glovebox: "server not reachable" | The address must be reachable from the car's parking spot over Wi-Fi. Use the server's address on your network, not `localhost`. |
| Glovebox: "the server moved to https" | A reverse proxy redirects to https. Enter the address with `https://`. The certificate must be one a browser would accept without a warning. |
| A reading or a check says "not available" | The car does not report it. **Device > Advanced** lists every request and how the car answered. |
| Drive times are wrong | The clock and the time zone are set from Glovebox, from the network, or from your browser the first time you open the page. **Device > System** shows what the device thinks the time is. |
| The device seems stuck | **Device > System > Restart CANary**, or unplug it for a few seconds. |

The **Device > Activity** list says what CANary has been doing in plain words. Under
**Device > Advanced**, **What this car answers** lists every request and the car's reply, and
downloads as a file to attach to a bug report. It names the model (the first eleven
characters of the VIN) but not the car's serial number.
