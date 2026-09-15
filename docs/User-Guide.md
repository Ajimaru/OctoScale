<!-- markdownlint-disable MD033 -->

# OctoScale User Guide

This guide covers day-to-day operation: the first run after assembly, the TFT menu, the web UI, and what to do when something looks wrong.

If the device is not built and flashed yet, start with the [Assembly Guide](Assembly-Guide) and the [Setup Guide](Setup-Guide). The [Setup Guide](Setup-Guide) owns build, flash, and first-time configuration; this page picks up once the device is on the network and calibrated.

## Initial setup

The [Setup Guide](Setup-Guide) covers flashing and the configuration steps in the web UI. Once that is done, the first thing worth doing is a full pass on the device itself, because it confirms the hardware independently of the network.

### Confirm the hardware before trusting a reading

A freshly assembled unit can pass a web UI check and still have a cold solder joint on the encoder or a buzzer wired backwards. The device has a hidden test menu for exactly this.

**To open it:** from the idle screen press PUSH to reach the Device menu, turn to **System info**, press PUSH to open it, then **hold PUSH for about 3 seconds**. A confirmation beep means the test menu is open.

The gesture is deliberately awkward. System info is already two deliberate steps from idle, so a held PUSH there will not fire by accident, and holding a single button is reliable in a way that a two-button combination is not on this hardware.

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-list.svg" width="240" alt="">

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-nfc.svg" width="210" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-scale.svg" width="210" alt=""> |
| --- | --- |
| **NFC test** — reader initialises, tag detected, UID read. Reader must show `ready`; a tag on the platform shows `present` plus its UID. | **Scale test** — load cell and HX711 respond. Press the platform: the number must move and return. |

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-led.svg" width="210" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-buzzer.svg" width="210" alt=""> |
| --- | --- |
| **LED test** — turn to step Red/Green/Blue/White/Off. **Both** WS2812 must show the same colour; if they differ, they are not sharing the LED helper. | **Buzzer test** — PUSH plays the test tone. Silence here with the buzzer enabled means a wiring fault. |

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-screen.svg" width="210" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-button.svg" width="210" alt=""> |
| --- | --- |
| **Screen test** — three pages: full-screen colours, font sample, grayscale ramp. Catches dead pixels and colour-order faults. | **Button test** — guided: press PUSH, then KO. Confirms both buttons are wired and debounced. |

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/test-knob.svg" width="210" alt=""> | |
| --- | --- |
| **Knob test** — guided: turn CW, then CCW. If the directions are reversed, swap `A` and `B` on the encoder. | |

Inside a sub-test, hold PUSH again to return to the test list; from the list, hold PUSH to leave. Button and Knob tests need this because they consume normal presses as test input and so have no short-press way back.

### First spool

1. Put a tagged spool on the platform.
2. If the tag is blank, write it from the web UI: **NFC → Write ID to tag**.
3. The device reads the tag, looks the spool up, and offers **Load into printer** or **Save weight**.

If the lookup fails, see [Troubleshooting](#troubleshooting) below — the message on screen distinguishes the cases.

## TFT user guide

### Controls

The whole interface is three inputs.

| Input | Action |
| --- | --- |
| Turn knob | Move the selection |
| PUSH (knob press) | Confirm / open |
| KO (second button) | Back / cancel |

Every screen that takes input shows its own hint line at the bottom, so the current meaning of each control is always on screen.

### Idle screen

The resting state shows the live weight in large digits, with a status footer for **NFC**, **Scale**, **DB**, and **WiFi**. A tag on the reader adds its carrier type (`NFC-A`, `NFC-V`, …) under the weight.

| No tag | Tag present | Not yet calibrated |
| --- | --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/idle-normal.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/idle-tag.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/idle-uncal.svg" width="240" alt=""> |

If the scale has never been calibrated, the idle screen carries a `Calibrate scale in web UI` warning. The weight shown before calibration is not meaningful.

After a period with no input the display steps down through three stages, all configurable in the web UI under System → Display:

1. **Dim** — backlight drops to the dim level (default after `timeout` seconds).
2. **Screensaver** — a bouncing logo, default after 60 s.
3. **Off** — backlight fully off, default after 300 s, if enabled.

| Boot splash | Screensaver |
| --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/boot.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/screensaver.svg" width="240" alt=""> |

Any input or any state change (a tag arriving, for instance) returns to the normal display.

### Loading a spool

Placing a tagged spool on the platform starts the flow automatically.

1. **Checking DB…** — the spool is looked up by tag ID, or by UID for a foreign tag.
2. **Spool found** — vendor, material, colour swatch and remaining weight, then a choice:
   - **Load into printer** → choose printer → choose tool → **Loaded**
   - **Save weight** → **Saved**
3. Both paths end on a result screen that returns to idle.

| 1. Lookup | 2. Spool found | 3a. Choose printer |
| --- | --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/flow-dbcheck.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/ask-action.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/ask-printer.svg" width="240" alt=""> |

| 3b. Choose tool | 4a. Loaded | 4b. Saved |
| --- | --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/ask-tool.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/done-loaded.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/flow-saved.svg" width="240" alt=""> |

The **Save weight** path is also the unload path. It shows `Put on scale` with a live reading and the expected remaining weight, so you can confirm the spool is seated before PUSH commits the value.

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/weigh-confirm.svg" width="240" alt="">

### Device menu

PUSH or KO from idle opens the Device menu.

| Item | Effect |
| --- | --- |
| **Tare** | Zeroes the scale; confirms with `Tared` and returns automatically |
| **NFC debug: ON/OFF** | Replaces the idle screen with a live tag readout — carrier, UID, usable bytes, and whether a spool ID resolved |
| **Buzzer: ON/OFF** | Master buzzer switch |
| **Theme: Dark/Light** | Display theme, independent of the web UI theme |
| **System info** | WiFi signal, IP, NFC/Scale/DB status, printer count, firmware version |

| Device menu | System info |
| --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/device-menu.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/sysinfo.svg" width="240" alt=""> |

**NFC debug** is the fastest way to tell an unreadable tag from an unassigned one: it shows the carrier and UID even when no spool ID could be resolved.

| No tag | Spool found | No spool ID |
| --- | --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/idle-debug-empty.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/idle-debug-id.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/idle-debug-noid.svg" width="240" alt=""> |

### Writing and erasing tags

Writes are started from the web UI but the device takes over the screen while one runs, showing `Writing NFC tag` / `Erasing NFC tag` with **Do not remove tag**.

| Writing | Erasing |
| --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/nfc-write.svg" width="240" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/nfc-erase.svg" width="240" alt=""> |

Take that literally. A write is not instant — an NTAG Extended write can take around 11 seconds — and lifting the tag mid-write leaves it partially written. The status LED blinks blue for the whole operation, however long it takes.

Results are shown for a few seconds:

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/nfc-written.svg" width="200" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/nfc-idonly.svg" width="200" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/nfc-writefail.svg" width="200" alt=""> |
| --- | --- | --- |
| **Tag written** — complete success; the format used is named underneath | **ID only** — the tag was too small for the chosen format, so only the spool ID was stored | **Write failed** — the reason is shown (auth failure, tag removed, verify mismatch) |

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/nfc-erased.svg" width="200" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/nfc-erasefail.svg" width="200" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/flow-error.svg" width="200" alt=""> |
| --- | --- | --- |
| **Tag erased** — erase completed | **Erase failed** — no tag present, or the erase errored | **Error** — any other flow step failing; the cause is named |

**ID only** is a warning, not a failure. The tag will still resolve to the right spool, but the extra fields are not on it — the database supplies them instead.

### Status LED and buzzer

The status LED is two WS2812 driven as one: one on the ESP32-S3 board, one on the enclosure front. They always show the same thing — the front one is simply the one you can see.

The patterns below animate — the timings are the firmware's own, so what you see here is what the device does.

**Resting and connection states**

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-idle.svg" width="64" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-boot.svg" width="64" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-ap.svg" width="64" alt=""> |
| --- | --- | --- |
| **Breathing green** — connected and idle, the normal resting state. Deliberately faint so an event flash stands out against it. | **Solid blue** — booting, or still joining the saved WiFi network. | **Solid orange** — no network reachable; the `OctoScale-Setup` portal is open. |

**Busy — do not interrupt**

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-ota.svg" width="64" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-busy.svg" width="64" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-db.svg" width="64" alt=""> |
| --- | --- | --- |
| **Blinking blue, 1 s** — software update running. Do not power off. | **Blinking blue, 0.5 s** — tag write, erase, or raw dump. Do not remove the tag. | **Solid cyan** — database lookup in flight. |

**Event flashes**

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-ok.svg" width="64" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-warn.svg" width="64" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/led-err.svg" width="64" alt=""> |
| --- | --- | --- |
| **Green** — spool recognised, or write succeeded. | **Amber** — write succeeded but fields were dropped (the **ID only** case). | **Red** — write failed, no printer configured, or a missing reference weight. |

Blue always means *do not interrupt*. Brightness for both WS2812 is a single slider in the web UI (System → Status LED); it scales the level only, never the colours or patterns.

The buzzer tones are matched to the LED, which blinks in lockstep with each tone segment. Frequencies and durations below are exactly what the firmware plays.

| | |
| --- | --- |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/buz-read.svg" width="300" alt=""> | Tag detected — the lightest signal, just "something happened". |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/buz-ok.svg" width="300" alt=""> | OK, and the web UI's test tone. |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/buz-success.svg" width="300" alt=""> | Spool loaded or weight saved — two rising beeps. |
| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/signals/buz-error.svg" width="300" alt=""> | Unknown spool or database offline — a low double tone. |

The buzzer can be switched off entirely in the Device menu; the LED timing stays the same either way.

## Web UI user guide

Open `http://<device-ip>/`. The interface is six tabs; the device only polls for data belonging to the tab you are actually looking at, and stops entirely when the browser tab is hidden.

| Tab | Purpose |
| --- | --- |
| **Operate** | Live weight, tare, and the current spool |
| **NFC** | Read, write, and erase tags; raw dump for unknown tags |
| **Scale** | Calibration and HX711 diagnostics |
| **Setup** | OctoPrint instances, spool database selection, WiFi |
| **System** | Display, LED, buzzer, themes, firmware update, config backup |
| **Debug** | Live NFC and HTTP log |

### Operate

Live weight with a tare button, mirroring the device's idle screen. Use this to confirm a reading without walking to the device.

### NFC

**Write ID to tag** is the common case: it writes the spool ID in the format chosen for that tag type. Everything else on the tag is optional — the database remains the source of truth.

Two results are worth understanding:

- **`droppedFields`** — the data did not fit on this tag. Expected on small tags; the spool still resolves.
- **`unsupportedFields`** — the chosen format has no representation for that field at all. Changing tags will not help; changing format might.

**Raw dump** reads a tag sector by sector without interpreting it, which is what to reach for when a tag reads as unknown and you need to know whether it holds data at all.

### Scale

One-point calibration needs a single known reference weight. Two-point calibration uses a light and a heavy reference and checks linearity, which catches a load cell mounted under strain.

The calibration factor lives in NVS and survives reboots and OTA updates. The HX711 panel shows chip readiness, the raw ADC value, and noise; unstable noise here is a wiring or supply problem, not a calibration problem.

### Setup

Add each OctoPrint instance with name, host, port, and API key (OctoPrint → Settings → API), then select which instance holds the spool database. Instances sharing an external database can fail over for each other; instances on local SQLite cannot, since each has its own separate records.

### System

Display brightness and the three idle timeouts, LED brightness, buzzer mode, and both themes. Firmware updates can be uploaded directly or pulled from a URL.

Configuration backup exports as AES-256-CBC encrypted JSON. API keys are never written in plaintext, so a backup file is safe to keep off the device — but it is also unreadable without the passphrase, so store that somewhere durable.

### Debug

A live log of NFC reads and HTTP calls. This polls at a high rate and only runs while the Debug tab is open and the browser tab visible, so leaving it open in a background window costs nothing.

## Troubleshooting

### The device reboots on its own

Almost always the power supply path, not the firmware. Check in this order:

1. **Are all three capacitors fitted?** 470 µF at the PN5180, 1500 µF at the ESP32-S3 `5V` pin, 100 µF at the Mini-360 output. The 470 µF one is what lets the PN5180 raise an RF field at all; the other two absorb WiFi transmit bursts. See the [Assembly Guide](Assembly-Guide).
2. **Is anything chained into the 5 V path?** USB switches, power meters, and extension cables each add contact resistance. A supply that measures a healthy 5 V at idle can still collapse under a current burst. Short, thick, and direct.
3. **Check the reset reason** in the Debug tab. A brownout and a firmware fault look identical from the outside but report differently — the [Development Guide](Development-Guide) explains how to read it.

### A tag is not recognised

The message on screen distinguishes the cases, and they have different fixes:

| <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/flow-unknown.svg" width="210" alt=""> | <img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/screens/ask-printer-empty.svg" width="210" alt=""> |
| --- | --- |
| **Unknown spool** — the tag was read fine, but its ID is not in the database. Assign the spool in SpoolManagerExtended, or write a fresh ID. | **No printer** — a spool resolved, but no OctoPrint instance is configured. Add one in the web UI under Setup. |

| Message | Meaning | Fix |
| --- | --- | --- |
| **Unknown spool** | The tag was read fine, but its ID is not in the database | Assign the spool in SpoolManagerExtended, or write a fresh ID |
| **Unreadable tag** | Data was found but no supported format could parse it | Check the raw dump; the tag may be foreign or partially written |
| No reaction at all | The tag was never detected | Reposition it, then check **NFC debug** or the NFC test |

A tag that reads intermittently by position is usually the 470 µF capacitor missing or too far from the PN5180.

### Weight is wrong or negative

- **Negative when loaded** — the load cell wires are swapped. Exchange white and green at the HX711.
- **Drifting or noisy** — check the HX711 noise value in the Scale tab. Steady noise with a stable supply points at a mechanical problem: the load cell must be able to flex freely, bolted at one end only.
- **Consistently off by a factor** — recalibrate, preferably two-point.

### Nothing on the database side works

Weighing, taring, calibration, and reading, writing, and erasing tags all work without OctoPrint. Lookup, printer and tool selection, and weight updates need [OctoPrint-SpoolManagerExtended](https://github.com/OctoPrint/OctoPrint-SpoolManagerExtended) reachable and configured with a valid API key. Check the DB indicator in the idle footer and the spool-ID lookup test in Setup.

### The encoder turns the wrong way

Swap `A` and `B` on the encoder. Confirm with the Knob test.

### The web UI is unreachable

Check the device's System info screen for its IP; the router may have issued a new one. Use the IP rather than `octoscale.local` — mDNS is unreliable here, and it is also why OTA updates should be addressed by IP.

---

For implementation details, the HTTP API, and the tag format internals, continue with the [Development Guide](Development-Guide).

<!-- markdownlint-enable MD033 -->
