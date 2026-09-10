# Vendored: PN5180 Library (patched)

Source: [atrappmann/PN5180 Library](https://github.com/ATrappmann/PN5180-Library),
v1.5 (matches the last version checked into `platformio.ini` before vendoring, and the
only version the PlatformIO registry ever published).

Upstream is **archived** (read-only, last commit August 2021) and its final release,
v1.8.1, fixes none of the hangs patched here — so vendoring + patching was the only
option rather than bumping the version. See "Why we stay on 1.5" below.

Patches applied here, in order: three of them are bug fixes for hangs that reboot the
device, one adds a command upstream never implemented, one extends a function's outputs.

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

## Patch: timeouts on the IRQ wait loops (`PN5180.cpp`) — bug fix

Same class of bug as the BUSY-pin patch above, at three more places that the original
patch missed. `setRF_on()`, `setRF_off()` and `reset()` each waited on an IRQ flag with
an unguarded spin:

```cpp
while (0 == (TX_RFON_IRQ_STAT & getIRQStatus()));   // no timeout, no way out
```

If the chip never raises the flag — RF disturbance, brown-out, a tag loading the field,
or the chip left in an unexpected state — the loop never returns. As with the BUSY wait,
that starves `pn5180Task` on core 0, trips the Task Watchdog and reboots the device with
no log. `reset()` is the most exposed of the three: it is by far the most-called entry
point of this library from `pn5180nfc.h` (~60 call sites), and `setRF_on()` runs on every
single NFC probe via `setupRF()`.

Fix: one shared private helper `PN5180::waitForIRQ(mask, timeoutMs)` polls `IRQ_STATUS`
and returns `false` after `PN5180_IRQ_TIMEOUT_MS` (500 ms) instead of spinning forever.
`setRF_on()`/`setRF_off()` now return `false` on timeout — `pn5180ProbeNfcA()` already
checked that return value, so its error path finally works. `reset()` stays `void` (all
~60 call sites treat it as fire-and-forget) and retries once with longer reset timings
before giving up.

`delay(1)` inside the loop is deliberate: it yields the CPU so the task keeps feeding the
watchdog. That is also why 500 ms is acceptable here, while the BUSY-pin wait — a tight
spin that does *not* yield — deliberately stays at 50 ms. Do not unify those two values.

## Patch: wait for `RX_IRQ_STAT` instead of `delay(10)` (`PN5180ISO15693.cpp`) — bug fix

`issueISO15693Command()` waited a flat 10 ms after `sendData()` and then checked the SOF
flag exactly once. A slower NFC-V tag has not finished answering by then, so its response
was thrown away as `EC_NO_CARD` — sporadic read failures that surfaced as "no tag" with
no error logged anywhere. Now the code waits for two things in order, each bounded:
first for the tag to *start* answering (SOF, `ISO15693_SOF_TIMEOUT_MS` = 20 ms), then for
the reception to *complete* (`RX_IRQ_STAT`, `ISO15693_RX_TIMEOUT_MS` = 200 ms).

**Both waits are load-bearing — do not collapse the first one back into a single check.**
The first version of this patch did exactly that (one SOF check after `delay(1)`, return
`EC_NO_CARD` if clear) and it broke every write: `WRITE_SINGLE_BLOCK` is only acknowledged
*after* the tag's internal programming cycle (~4–6 ms on ICODE parts), so SOF is
legitimately still clear at that point. Reads kept working (a tag answers those in well
under a millisecond), which is what made it look fine at first — every write failed with
`write failed (block 0)`. Confirmed by A/B-flashing the unpatched library against the
patched one on the same tag, then fixed and re-verified with a real write + read-back.

The SOF timeout is deliberately kept tight (20 ms — the old flat delay was 10 ms) because
it is paid in full on every poll that has no tag on the reader. A timeout in either loop
returns `EC_NO_CARD`, which every caller already treats as "nothing read".

## Attribution for the two patches above

Both fixes are adopted from the actively maintained fork
[tueddy/PN5180-Library](https://github.com/tueddy/PN5180-Library) v2.3.7 — a fork of this
same library, published under the same **LGPL-2.1**, so reusing it here raises no
licensing question. The `RX_IRQ_STAT` fix is also present in upstream v1.8.1.

The timeout/retry *approach* was taken from that fork; the code here was re-expressed as
one shared `waitForIRQ()` helper rather than copied loop-by-loop, to stay consistent with
the existing `pn5180WaitBusy()` patch and to keep the three call sites readable. The
`reset()` retry sequence (RST low 10 ms, high 50 ms, one more attempt) follows tueddy's
behaviour directly. Timeout values: 500 ms matches tueddy's `commandTimeout` default;
the 200 ms ISO15693 bound is our own choice.

## Why we stay on 1.5 instead of switching library

Checked when these patches were written:

- **Upstream `ATrappmann/PN5180-Library` is archived** (read-only on GitHub, last commit
  August 2021). Nothing will ever be fixed there.
- **v1.8.1 fixes none of the hangs** — all the unguarded loops above are still present in
  it. Its only relevant improvement is the `RX_IRQ_STAT` fix, which we took.
- **The PlatformIO registry only ever published 1.5**, which is why that is the base here.
- **tueddy's fork is the de-facto successor** (v2.3.7, active into 2025) and contains
  everything we patch by hand. Switching to it wholesale was considered and rejected for
  now: it would break three call sites (`getSystemInfo()` has no extended overload there,
  `mifareAuthenticate()` returns `int16_t` instead of our `bool` + status-out, and our
  `mifareAuthDebugCb` hook does not exist), and its `PN5180ISO14443` is actually weaker
  than our own NFC-A code — it uses a flat `delay(5)` and has no equivalent of
  `pn5180SendDataRetry()` (`src/pn5180nfc.h`), which recovers the real "chip stuck in
  transceive state 3" failure. If a future bump is wanted, tueddy is the target, and
  those are the three breakages to expect.

## Keeping this up to date

If bumping the PN5180 library version, re-apply **all** the patches above (or check
which of them the new source already contains) instead of just overwriting
`lib/PN5180 Library/` with a fresh download. The quickest completeness check for the
hang fixes: `grep -n "while" "lib/PN5180 Library/"*.cpp` — every remaining loop must have
a timeout, and there must be no `while (...);` one-liner left at all.
`platformio.ini` deliberately excludes `PN5180 Library` from `lib_deps` so this local,
patched copy is always used (PlatformIO prefers `lib/` over `lib_deps` for the same
library name) — this applies to every checkout/clone build, not just this machine.
