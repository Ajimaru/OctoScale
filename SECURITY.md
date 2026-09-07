# Security Policy

## Reporting a vulnerability

Please report security issues through GitHub's private vulnerability reporting
("Report a vulnerability" on the Security tab) rather than a public issue.

## Scope and expectations

OctoScale is a hobbyist device intended for a **trusted home network**. Its security
model is worth stating plainly so you know what to expect:

- **The web UI and HTTP API are unauthenticated.** Anyone who can reach the device on
  the network can weigh, tare, read and write tags, and change configuration. There is
  no login, and adding one is not currently planned.
- **OTA firmware updates are unauthenticated.** `espota` runs without a password, and
  `POST /updateurl` makes the device download and flash a `.bin` over plain HTTP — the
  download is integrity-checked (MD5/length) but the source is not authenticated. Use
  trusted URLs on your own LAN only, and do not expose the device to the internet.
- **All traffic is unencrypted HTTP** — the web UI, the OctoPrint calls and the backup
  transfer alike.
- **OctoPrint API keys are the one thing that is protected**: they are stored in NVS and
  AES-256-CBC encrypted in config backups, never exported in plaintext.

Given that, the issues most worth reporting are ones that break the assumptions above —
for example an API key leaking in plaintext through an endpoint or a backup, a buffer
overflow reachable from tag data (a malicious tag is a real attack surface, since tags
travel between devices), or a way to persist code across a firmware update.

Reports that amount to "the web UI has no password" are known and documented here.

## Supported versions

The latest release on `main` is the only supported version.
