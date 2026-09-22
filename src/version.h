#pragma once

// Bump on release. Build timestamp is added automatically.
#define FW_VERSION_RELEASE "0.0.4"

// Dev builds get a running number appended ("0.0.3-dev7") so a device can say exactly
// which build is on it -- flashing the same release version twice is otherwise
// indistinguishable on the TFT, in /nfcprobe's sibling endpoints and in the web UI.
// tools/dev_build.py defines FW_DEV_BUILD on every local build and keeps the counter
// in artifacts/.devcounter, outside git: the number changes on every build, so having
// it in a tracked file would make every test flash look like a source change.
// A build without that script (a clean checkout) falls back to the plain release
// version. CI's release build runs the same script but with OCTOSCALE_RELEASE_BUILD=1
// (see .github/workflows/release.yml and tools/dev_build.py), which makes it skip the
// dev suffix so a published firmware reports the plain release version too.
#ifdef FW_DEV_BUILD
  #define FW_VERSION FW_DEV_BUILD
#else
  #define FW_VERSION FW_VERSION_RELEASE
#endif

#define FW_BUILD __DATE__ " " __TIME__
