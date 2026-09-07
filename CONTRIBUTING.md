# Contributing

Thanks for taking a look. OctoScale is a hardware project, so a few things are worth
knowing before you open a PR.

By participating you agree to abide by the [Code of Conduct](CODE_OF_CONDUCT.md).

## Before you start

- **Hardware-dependent changes need hardware.** Anything touching the PN5180, HX711,
  TFT or encoder can only really be reviewed if it has been run on a device. Say in the
  PR whether you tested on real hardware, and which tag types if it is NFC-related.
- **Open an issue first for larger changes.** Especially for new on-tag payload formats
  or changes to the flow state machine — those have knock-on effects on the web UI, the
  TFT menu and the database side at once.

## Building

```bash
pio run -e esp32s3                                    # build
pio run -e esp32s3 -t upload                          # flash over USB
pio run -e esp32s3_ota -t upload --upload-port <ip>   # flash over the network
```

CI builds `esp32s3` on every push and PR. The OTA environment inherits everything from
`[s3_base]` and differs only in upload transport, so a green build covers both.

## Code style

Match the surrounding code rather than applying a formatter — the source is
deliberately comment-heavy, and the comments explain *why* something is the way it is
(hardware quirks, timing constraints, library bugs worked around). Those comments are
the main documentation of the hardware's behaviour, so please keep that up in new code.

A few conventions that are load-bearing rather than cosmetic:

- **Core split.** HX711 and PN5180 run on core 0, WiFi/HTTP/OTA on core 1. HTTP handlers
  must never touch `g_tft`, `pixel` or the scale directly — set a `volatile` flag and let
  the core-0 task act on it (see `g_menuPreviewReq` for the pattern).
- **Compiler warnings.** The project builds clean under `-Wall -Wextra`; please keep it
  that way.
- **NFC changes** should state which tag types were tested (NTAG, NFC-V, Mifare Classic
  are three genuinely different code paths).

## Reporting bugs

Include the firmware version (footer of the web UI), the tag type if relevant, and a
debug log: the web UI's Debug tab has a console that records NFC reads/writes and HTTP
calls once switched on.
