# OctoScale Features

NFC filament scale for OctoPrint and SpoolManagerExtended. This page summarizes the device capabilities and supported tag formats. See the [Assembly Guide](https://github.com/Ajimaru/OctoScale/wiki/Assembly-Guide) for hardware, wiring, enclosure, firmware, setup, and maintenance.

<img src="https://raw.githubusercontent.com/Ajimaru/OctoScale/main/assets/OctoScale.jpg" width="380" alt="Assembled OctoScale with a spool on the scale">

## Scale

- HX711 and 5 kg load cell with live readout on the TFT and web UI.
- One-point quick calibration and two-point linearity-checked calibration.
- Calibration factor stored in NVS and retained across reboots and OTA updates.

## NFC

The PN5180 reads and writes NFC-A/NTAG/Ultralight, NFC-V/ISO 15693, and Mifare Classic 1K tags. Supported payload formats are auto-detected:

| Format | Tag | Notes |
| --- | --- | --- |
| `octoscaleExtended` | Mifare Classic 1K | Custom binary layout with CRC-8 commit marker |
| `ntagExtended` | NTAG | Same layout with NTAG page addressing |
| `nfcvExtended` | NFC-V | Same layout with NFC-V blocks |
| `openSpool` / `nfcvOpenSpool` | NTAG / NFC-V | OpenSpool NDEF/JSON, readable by third-party apps |
| `nfcvOpenPrintTag` | NFC-V | OpenPrintTag CBOR, including drying data |
| `tigerTag` | NTAG | TigerTag Standard, unsigned, big-endian 80-byte payload |

Foreign tags such as Snapmaker U1 tags fall back to a UID lookup in SpoolManagerExtended. NFC writes report `droppedFields` when fields do not fit and `unsupportedFields` when the selected format has no representation. Unknown tags can be inspected with a raw sector/block dump.

## Interface and connectivity

- ST7789 320x240 TFT with EC11 encoder menu mirroring the spool workflow.
- Boot splash, locked OTA progress screen, and independent light/dark display theme.
- Three idle stages: dim, bouncing-logo screensaver, and display/backlight off.
- Hidden device test menu: hold PUSH for 3 seconds on the System info screen.
- Web UI themes: OctoScale and OctoPrint, each with light/dark mode.
- Web UI covers weighing, calibration, NFC, OctoPrint instances, system, WiFi, and test points.
- Passive buzzer with event tones and mirrored onboard/external WS2812 status LEDs.
- Optional browser debug console for NFC and HTTP activity.
- WiFiManager provisioning with AP fallback and captive portal.
- OTA through `espota` or web upload/URL update.
- Multiple OctoPrint instances with database failover when instances share an external database.
- AES-256-CBC encrypted JSON configuration backup; API keys are never exported in plaintext.
