# Vendored: PN5180 Library (patched)

Source: [atrappmann/PN5180 Library](https://github.com/ATrappmann/PN5180-Library),
v1.5 (matches the last version checked into `platformio.ini` before vendoring).
Checked upstream up to v1.8.1 (latest as of this patch) — the bug below is still present
there, so vendoring + patching was the only option instead of just bumping the version.

## Patch: timeout on the BUSY-pin wait (`PN5180.cpp`, `transceiveCommand`)

Upstream waits on the PN5180's BUSY pin with plain `while (digitalRead(...) != level);`
loops — no timeout. A non-conforming or edge-case NFC-A/NTAG tag that doesn't complete
the SPI handshake as expected (reproduced with a specific "simple" NFC-A tag while
writing) hangs this loop forever. On the ESP32 that starves the owning FreeRTOS task
(`pn5180Task`, core 0) indefinitely, which trips the Task Watchdog Timer -> hard reboot,
with no serial/crash log surviving (this project has no USB serial in normal operation —
it runs on external power).

Fix: each BUSY wait now bails out after 50ms and returns `false` instead of hanging.
NSS is always deasserted before returning on a timeout, so the SPI bus is never left
mid-transaction. Callers (`sendData`/`readData`/etc.) already treat a failed transceive
as "no valid data" via their existing RX_STATUS/length checks, so a timeout now surfaces
as a clean write/read failure instead of a silent reboot.

## Patch: `mifareAuthenticate()` (`PN5180.h`/`PN5180.cpp`) — new function, not a fix

Upstream never implements the PN5180's host command `0x0C` (`MIFARE_AUTHENTICATE`),
so the library can talk to NFC-A/NTAG/Ultralight and ISO15693/NFC-V tags but not
MIFARE Classic (which needs a Crypto1 sector authentication before any read/write).
Added `PN5180::mifareAuthenticate(blockAddr, key, authMode, uid, &status)` following
the exact SPI transaction pattern already used by `writeRegister`/`readEEprom`/etc.
(`transceiveCommand` under `SPI.beginTransaction`/`endTransaction`). Command frame is
13 bytes: `[0x0C, key[6], authMode(0x60=KeyA/0x61=KeyB), blockAddr, uid[4]]`, response
is 1 status byte (0 = authenticated). Used by `pn5180nfc.h`'s Mifare Classic
read/write functions (OctoScale-side, not part of this vendored copy).

**Third-party reference**: the command frame's byte ordering was initially guessed
wrong (`[0x0C, blockAddr, authMode, key[6], uid[4]]`, which authenticated silently
without ever running the RF handshake) and was corrected by cross-checking against
[jef-sure/esp32-component-pn5180](https://github.com/jef-sure/esp32-component-pn5180)
(`src/pn5180.c`, `pn5180_mifareAuthenticate()`), MIT License, Copyright (c) 2026 Anton
Petrusevich. No code from that project was copied verbatim — only the wire-format byte
ordering was verified against it.

## Keeping this up to date

If bumping the PN5180 library version, re-apply this patch (or check if upstream fixed
it) instead of just overwriting `lib/PN5180 Library/` with a fresh download.
`platformio.ini` deliberately excludes `PN5180 Library` from `lib_deps` so this local,
patched copy is always used (PlatformIO prefers `lib/` over `lib_deps` for the same
library name) — this applies to every checkout/clone build, not just this machine.
