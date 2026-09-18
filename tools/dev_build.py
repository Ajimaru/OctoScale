"""PlatformIO build hook: dev-build numbering + artifact archiving.

Two jobs, both aimed at one question that kept coming up during hardware testing:
"which build is actually on the device right now?"

1. Before compiling, bump a counter and hand the code a version string of the form
   "<FW_VERSION>-dev<N>" through -DFW_DEV_BUILD. src/version.h stays untouched, so
   flashing does not produce a git diff and the release version remains the single
   source of truth for what the firmware calls itself on a release build.

2. After linking, copy the images into artifacts/ named after that version, so an
   older build can still be flashed back and a crash backtrace can still be resolved
   against the .elf it came from.

The counter lives in artifacts/.devcounter (gitignored, like artifacts/ itself). It is
deliberately NOT in version.h or any tracked file: a number that changes on every
build would otherwise turn every test flash into a commit-worthy change.

Both the USB and the OTA environment run this. They share s3_base, so the same source
builds both, and the artifact names carry the environment to keep them apart.
"""

Import("env")

import re
import shutil
from pathlib import Path

PROJECT_DIR = Path(env["PROJECT_DIR"])
ARTIFACT_DIR = PROJECT_DIR / "artifacts"
COUNTER_FILE = ARTIFACT_DIR / ".devcounter"
VERSION_HEADER = PROJECT_DIR / "src" / "version.h"


def read_release_version() -> str:
    """FW_VERSION_RELEASE as written in src/version.h ("0.0.3"), or "0.0.0" if unreadable.

    Parsed rather than imported because version.h is C, and duplicating the number
    here would let the two drift apart silently.
    """
    try:
        text = VERSION_HEADER.read_text(encoding="utf-8")
    except OSError:
        return "0.0.0"
    match = re.search(r'#define\s+FW_VERSION_RELEASE\s+"([^"]+)"', text)
    return match.group(1) if match else "0.0.0"


def next_dev_number() -> int:
    """Increment and persist the per-build counter.

    A corrupt or missing counter restarts at 1 rather than failing the build: losing
    the numbering is an inconvenience, a build that refuses to start is not.
    """
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    current = 0
    try:
        current = int(COUNTER_FILE.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        current = 0
    nxt = current + 1
    try:
        COUNTER_FILE.write_text(f"{nxt}\n", encoding="utf-8")
    except OSError as exc:
        print(f"dev_build: WARNING could not persist counter ({exc}); using {nxt}")
    return nxt


RELEASE_VERSION = read_release_version()
DEV_NUMBER = next_dev_number()
DEV_VERSION = f"{RELEASE_VERSION}-dev{DEV_NUMBER}"

# The firmware reads this instead of FW_VERSION when it is defined (see version.h).
# Quoting via CPPDEFINES tuple so the value reaches the compiler as a string literal.
env.Append(CPPDEFINES=[("FW_DEV_BUILD", env.StringifyMacro(DEV_VERSION))])

print(f"dev_build: building {DEV_VERSION}")


def archive_artifacts(source, target, env):
    """Copy the linked images into artifacts/ under the dev version name.

    Runs after the .bin exists. firmware.bin is the application image -- the same file
    the web UI's OTA upload expects (see docs/Setup-Guide.md); it is copied under an
    -ota name as well so the OTA upload target is obvious without knowing that.
    """
    build_dir = Path(env.subst("$BUILD_DIR"))
    env_name = env.subst("$PIOENV")
    base = f"octoscale-{DEV_VERSION}-{env_name}"

    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    copies = [
        (build_dir / "firmware.bin", ARTIFACT_DIR / f"{base}.bin"),
        (build_dir / "firmware.bin", ARTIFACT_DIR / f"{base}-ota.bin"),
        (build_dir / "firmware.elf", ARTIFACT_DIR / f"{base}.elf"),
    ]
    for src, dst in copies:
        if not src.exists():
            print(f"dev_build: WARNING {src.name} missing, not archived")
            continue
        try:
            shutil.copy2(src, dst)
        except OSError as exc:
            # Never fail the build over archiving -- the firmware itself is fine.
            print(f"dev_build: WARNING could not archive {dst.name} ({exc})")
    print(f"dev_build: archived {base}.{{bin,-ota.bin,elf}} -> artifacts/")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", archive_artifacts)
