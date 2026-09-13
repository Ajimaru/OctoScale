# OctoScale Setup Guide

This guide covers the first firmware flash, OTA updates, and first-time device setup. Start with the [Assembly Guide](https://github.com/Ajimaru/OctoScale/wiki/Assembly-Guide) if the hardware is not assembled yet.

## Build & flash

Install [PlatformIO](https://platformio.org/). The patched PN5180 library is vendored in `lib/`; other dependencies are downloaded from `lib_deps` on the first build.

For the first flash, disconnect the external 5 V supply and connect USB:

```bash
pio run -e esp32s3 -t upload
```

On first boot, connect to the `OctoScale-Setup` access point, enter WiFi credentials, and note the assigned IP from the router or serial monitor:

```bash
pio device monitor -e esp32s3
```

The web UI is at `http://<device-ip>/`. Later firmware updates use OTA and do not need USB:

```bash
pio run -e esp32s3_ota -t upload --upload-port <device-ip>
```

Use the IP address for OTA rather than `octoscale.local` because mDNS can make `espota` time out.

### Prebuilt firmware files

Tagged releases provide ready-to-use firmware files under [GitHub Releases](https://github.com/Ajimaru/OctoScale/releases):

- `octoscale-usb.bin` is the merged image for a first USB flash. Flash it at address `0x0` with an ESP32-S3 tool such as `esptool`.
- `octoscale-ota.bin` is the application image for an OTA update through the web UI or the `/updateurl` endpoint. Do not use it for the first flash; it does not contain the bootloader and partition table.

Release artifacts are created when a tag such as `v0.0.1` matches `FW_VERSION` in `src/version.h`. The GitHub Actions firmware artifact is intended for CI inspection; the tagged GitHub Release is the stable download location.

## First-time setup

Open `http://<device-ip>/` and work through these steps in order.

1. **Calibrate.** Empty the scale and tare it. Use a known reference weight with either one-point calibration or two-point calibration using light and heavy references. Verify the result. The HX711 diagnostics panel shows chip readiness, raw ADC value, and noise; unstable noise usually indicates wiring or supply trouble.
2. **Add printers.** In Setup, add each OctoPrint instance with name, host, port, and API key from OctoPrint Settings > API.
3. **Choose the spool database.** Select the OctoPrint instance used for spool records. Instances sharing an external database can fail over; local SQLite instances cannot. Use the spool-ID lookup test.
4. **Review optional settings.** Configure selection timeout, display brightness and idle stages, LED brightness, buzzer mode/tones, and web/display themes.
5. **Test a spool.** Put a tagged spool on the scale. For a new tag, use NFC > Write ID to tag. The Debug tab can log reads and lookups.

### Software requirement

Database features require [OctoPrint-SpoolManagerExtended](https://github.com/OctoPrint/OctoPrint-SpoolManagerExtended). OctoScale uses its API endpoints for spool lookup by ID or UID, measured-weight updates, printer/tool loading, and database identification for failover. All calls use API keys; OctoScale never connects to MySQL directly.

Without the plugin, weighing, taring, calibration, reading, writing, and erasing NFC tags still work. Automatic lookup, printer/tool selection, and database weight updates do not.

Continue with the [User Guide](https://github.com/Ajimaru/OctoScale/wiki/User-Guide) to operate the device, or consult the [Development Guide](https://github.com/Ajimaru/OctoScale/wiki/Development-Guide) for implementation details.
