#pragma once
#include <Arduino.h>

// pn5180nfc.h — PN5180 NFC reader (SPI) via the atrappmann/PN5180 library.
//
// FSPI bus (global SPI, SCK=12/MOSI=11/MISO=13), dedicated control pins
// (NSS=10/BUSY=14/RST=21). The chip answers over MISO, so the EEPROM version read
// confirms bus + chip + wiring all work.

// --- Tag types (protocol-neutral) ---
enum PN5180TagType {
  PN5180_TAG_NONE = 0,   // no tag in the field
  PN5180_TAG_NFCV,       // ISO15693 / NFC-V
  PN5180_TAG_NFCA,       // ISO14443-A / NFC-A
};

// Result of a debug probe (latest state, readable via HTTP).
struct PN5180ProbeResult {
  PN5180TagType type = PN5180_TAG_NONE;
  String uid = "";       // big-endian hex (usual display order)
  uint16_t atqa = 0;     // NFC-A only: ATQA (2 bytes)
  uint8_t sak = 0;       // NFC-A only: SAK (1 byte, type hint)
  uint8_t blockSize = 0; // NFC-V only: bytes/block (from getSystemInfo, 0 = unknown)
  uint8_t numBlocks = 0; // NFC-V only: block count (from getSystemInfo, 0 = unknown)
  uint8_t dsfid = 0;      // NFC-V only: Data Storage Format Identifier, if the tag reports one
  bool dsfidPresent = false;
  uint8_t afi = 0;        // NFC-V only: Application Family Identifier, if the tag reports one
  bool afiPresent = false;
  // NFC-V only: manufacturer-specific IC reference byte. Shown as a raw hex value, not
  // decoded to a chip name -- no verified public mapping of this byte to NXP ICODE
  // variants (SLI/SLIX/SLIX2/SLIX-L/...) was found; guessing wrong would be worse than
  // not showing a product name at all (see HARDWARE.md's "Tags evaluated for NFC-V").
  uint8_t icRef = 0;
  bool icRefPresent = false;
  int numPages = 0;       // NFC-A only: page/block count (Mifare Classic: fixed 64; NTAG: from GET_VERSION)
  bool numPagesPresent = false;
  long idParsed = -1;    // databaseId parsed off the tag's data area (-1 = none/unparsable)
  String idText = "";    // raw characters read where the databaseId would be (debug only)
  // How much the idParsed above can be trusted -- the legacy ID layout carries no magic
  // and no checksum, so "there are digits at the right offset" is NOT proof the tag is
  // ours. Consumers (the load flow, the SpoolManager plugin) need to tell the two apart:
  //   "extended" -- id came from a self-describing format (ntagExtended's "OX" magic,
  //                 octoscaleExtended's "OS", nfcvExtended, OpenSpool NDEF, TigerTag).
  //                 A magic matched, so the id is as trustworthy as the format itself.
  //   "legacy"   -- id came from the header-less ASCII field written by /nfcwriteid
  //                 (pn5180WriteNtagId & friends). Shape-validated only. A foreign tag
  //                 with digits in the same place is indistinguishable from ours.
  //   "unverified" -- as "legacy", AND the tag's occupancy says "foreign": the CC/data
  //                 area belongs to a format we did not write. Almost certainly not our
  //                 id. Callers should refuse to act on it without asking the user.
  //   "extendedNoId" -- a verified format that carries no OctoScale databaseId at all
  //                 (TigerTag, OpenPrintTag). Structured data IS present, so this is
  //                 NOT the same as "": a caller must not treat the tag as blank.
  //   ""         -- no id parsed at all (idParsed == -1).
  // Set by pn5180ApplyIdTrust() once occupancy is known; the raw read paths only ever
  // produce "extended" or "legacy", since they cannot see the CC.
  String idSource = "";
  // Capability Container state (NFC-A/NTAG + NFC-V only -- Mifare Classic has no CC
  // concept, stays ""). "virgin" = all-zero, never written; "ndef" = valid NDEF CC
  // (0xE1/0xE2 magic) -- the OTP bit is set and can NEVER be unset again, even by an
  // erase (see pn5180NtagCcIsVirgin/pn5180NfcvCcIsVirgin, and pn5180EraseTag's
  // comment); "other" = neither (foreign/non-NDEF content, erase/write would refuse
  // to touch it); "" = not read (Mifare, or read failed).
  String ccState = "";
  // False while a tag has been detected but its Extended read / occupancy probes have
  // not finished yet. The poll loop publishes type and uid as soon as the tag answers,
  // because the TFT needs them immediately, but the id and the format fields are only
  // filled a few hundred milliseconds later. Without this flag that window is
  // indistinguishable from a finished read of a tag that genuinely has no id, so a
  // caller polling once could see an Extended tag as "empty, no id" and offer to
  // overwrite it. A caller that only acts on complete data should require this.
  bool complete = false;
};

// databaseId storage on the tag (uniformly ASCII decimal):
//  - NFC-A/NTAG: user pages starting at page 4 (READ 0x30 always returns 4 pages/16 B).
//  - NFC-V/ISO15693: blocks starting at block 0 (readSingleBlock, usually 4 B/block).
// 12 bytes covers the full uint32 range (max 10 digits) + terminator.
static const uint8_t PN5180_ID_START_PAGE = 4;   // NFC-A: first user page
static const uint8_t PN5180_ID_START_BLOCK = 0;  // NFC-V: first data block
static const uint8_t PN5180_ID_BYTES = 12;       // 12 bytes ASCII decimal

// --- Mifare Classic 1K (blank/factory tags only -- see pn5180nfc.h's mifare section) --
// Block 4 = sector 1's first data block: sector 0 (blocks 0-3) holds the manufacturer
// block + often factory data, so sector 1 is the first "clean" sector on every blank
// 1K tag. One 16-byte block comfortably fits PN5180_ID_BYTES (12) -> no multi-block
// split needed, unlike NTAG's 4-byte pages.
static const uint8_t MIFARE_ID_BLOCK = 4;
static const uint8_t MIFARE_KEY_A[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // factory default
static const uint8_t MIFARE_AUTH_KEY_A = 0x60;
static const uint8_t MIFARE_AUTH_KEY_B = 0x61;  // foreign tags may guard data with B

// --- Mifare Classic Extended layout (plan C.1) --------------------------------
// Sector 1 (block 4, unchanged) stays the legacy ID anchor. Sector 2 (blocks 8-9) adds
// numeric fields + a CRC-8 "commit marker" (block 8's magic). Sector 3 (blocks 12-14)
// adds strings. Block 8 is written LAST -- an aborted write leaves a tag with no magic,
// which reads back as "legacy ID only", never as a corrupt Extended payload.
static const uint8_t MIFARE_EXT_MAGIC0 = 'O';
static const uint8_t MIFARE_EXT_MAGIC1 = 'S';
// v3: sector 4 block 16 adds 7 more numeric fields (remainingWeight/totalLength/
// usedLength/firstUse/lastUse/purchasedOn/cost), sectors 5-8 (12 blocks) add a
// second, longer string buffer (code/batchNumber/purchasedFrom/finish/displayName),
// sector 9 block 36 is a v3-only commit marker (own sub-magic, checked independently
// of block 8's) so a v3 reader can tell "v3 fields genuinely present and intact" apart
// from "v3 version byte was set but the write was cut off before the new blocks
// landed" -- block 8 is still written LAST overall, same discipline as v1/v2.
static const uint8_t MIFARE_EXT_VERSION = 0x05;  // v5: drying/td in block 17[6..11] (v4: multi-colour in block 10[2..8])
static const uint8_t MIFARE_EXT_BLOCK8  = 8;
static const uint8_t MIFARE_EXT_BLOCK9  = 9;
static const uint8_t MIFARE_EXT_BLOCK10 = 10;  // v2+: [0] bed temp min [1] bed temp max
static const uint8_t MIFARE_EXT_BLOCK12 = 12;
static const uint8_t MIFARE_EXT_BLOCK13 = 13;
static const uint8_t MIFARE_EXT_BLOCK14 = 14;
static const int MIFARE_EXT_STRING_BYTES = 48;  // blocks 12-14, 16 bytes each
static const uint8_t MIFARE_EXT_BLOCK16 = 16;   // v3: sector 4, 7 packed numeric fields
// Block 16 is packed full (all 16 bytes), so the minute-of-day companions for
// firstUse/lastUse/purchasedOn get block 17 -- same sector, so it rides along on the
// auth block 16 already did, no extra Crypto1 round.
static const uint8_t MIFARE_EXT_BLOCK17 = 17;   // v3: 3x minute-of-day (uint16 LE);
                                               // v5: +6 dryTemp u8, +7 pad, +8 dryTime u16 LE,
                                               // +10 td x100 u16 LE, +12..15 reserved 0x00
// v3 string buffer: sectors 5-8, 12 contiguous data blocks treated as one flat
// 192-byte length-prefixed buffer (same [1-byte len][UTF-8 bytes] convention as the
// existing 3-string block above, just longer/more records). Block numbers skip each
// sector's trailer (23,27,31,35 are Crypto1 key blocks, not data).
static const uint8_t MIFARE_EXT_BLOCK20 = 20;
static const int MIFARE_EXT_STRING2_BLOCKS[12] = {20,21,22, 24,25,26, 28,29,30, 32,33,34};
static const int MIFARE_EXT_STRING2_BYTES = 192;  // 12 blocks x 16 B
static const uint8_t MIFARE_EXT_BLOCK36 = 36;   // v3: sector 9, commit marker

// --- NFC-V/ISO15693 Extended layout (blank/user-purchased tags only, same caveat as
// Mifare above -- this is NOT written to a tag whose blocks might already hold
// factory/vendor data; pn5180WriteNfcvSpoolTag only ever gets called for tags the
// user chose to write a spool ID to via the web UI in the first place). --------------
// Fixed 4-byte blocks (pn5180WriteNfcvId's own reasoning: getSystemInfo() has a known
// OOB-read bug on short/non-conforming replies, so it's never queried for this format
// either -- 4 B/block is the documented fallback and covers the ICODE-family tags this
// project targets). Blocks 0-2 stay the legacy 12-byte ID anchor (unchanged). Same
// commit-marker discipline as Mifare: the magic block is written LAST.
//   Block 3-4  (8 B):  magic(2) + version(1) + flags(1) + databaseId(4, uint32 LE)
//   Block 5-6  (8 B):  totalWeight/spoolWeight/usedWeight (uint16 LE each) + density (uint16 LE)
//   Block 7-10 (16 B): diameter+diameterTolerance (uint16 LE each) + 3 temps (uint8) +
//                      3 signed offsets (int8) + RGB (3 B) + hotend temp min/max (uint8 each,
//                      v2+) + CRC-8 (1 B, over blocks 3-10 minus the CRC byte itself)
//   Block 11-22 (48 B): strings (vendor/material/colorName), same length-prefixed layout as Mifare
//   Block 23 (4 B, v2+): [0] bed temp min (uint8) [1] bed temp max (uint8) [2-3] reserved
static const uint8_t NFCV_EXT_MAGIC0 = 'O';
static const uint8_t NFCV_EXT_MAGIC1 = 'S';
static const uint8_t NFCV_EXT_VERSION = 0x04;  // v4: drying/td in blocks 26-27 (v3: multi-colour in blocks 24-25)
static const uint8_t NFCV_EXT_BLOCK_START = 3;   // magic/version/flags/dbId: blocks 3-4
static const uint8_t NFCV_EXT_BLOCK_NUM   = 5;   // weights/density: blocks 5-6
static const uint8_t NFCV_EXT_BLOCK_PHYS  = 7;   // diameter/temps/rgb/crc: blocks 7-10
static const uint8_t NFCV_EXT_BLOCK_STR   = 11;  // strings: blocks 11-22
static const int NFCV_EXT_STRING_BYTES = 48;
static const uint8_t NFCV_EXT_BLOCK_TEMPRANGE = 23;  // v2+: bed temp min/max (4 B block)
// v3: multi-colour. tempRangeBuf has only [2..3] free -- not enough for 2 RGB trios plus
// flags -- so this takes two fresh blocks from the tag's unused tail (24-27 are free on
// the smallest ICODE this project targets, 28 blocks / 112 B). Colour 1 stays in
// physBuf where it always was.
static const uint8_t NFCV_EXT_BLOCK_COLOR = 24;   // [0..2] colour 2, [3] flags
                                                  // block 25: [0..2] colour 3, [3] reserved
// v4: drying/td. Strings live in blocks 11-22, i.e. BEFORE these -- unlike NTAG, adding
// them shifts nothing, so no existing field can be misread if a writer gets it wrong.
static const uint8_t NFCV_EXT_BLOCK_DRY = 26;    // [0] dryTemp u8, [1] pad, [2..3] dryTime u16 LE
                                                  // block 27: [0..1] td x100 u16 LE, [2..3] reserved
static const uint8_t NFCV_BLOCK_SIZE = 4;

// --- NTAG Extended layout (NTAG215/216 only -- 213's ~144 B usable can't fit the v3
// field set, see the capacity comment on pn5180WriteNtagExtended; offered as a
// SpoolManagerExtended write-format preference alongside openSpool, mirroring NFC-V's existing
// nfcvExtended/nfcvOpenSpool choice). No NDEF wrapper (Extended isn't meant to be read
// by a generic phone NFC app, same reasoning as Mifare's own Extended format) -- pages
// are written directly starting at page 4 (the first user page; pages 0-3 are
// UID/lock/CC/internal and never touched). Single version, full v3 field set from the
// start (no v1/v2 predecessor to stay compatible with, unlike Mifare/NFC-V). All 20
// fixed-layout bytes below are 4-byte-page-aligned (5 pages, 4-19 is NOT used -- pages
// 4-8, 20 B total) -------------------------------------------------------------------
//   Pages 4-5   (8 B):  magic(2) + version(1) + flags(1) + databaseId(4, uint32 LE)
//   Pages 6-7   (8 B):  totalWeight/spoolWeight/usedWeight/remainingWeight (uint16 LE each)
//   Page 8      (4 B):  density(uint16 LE) + diameter(uint16 LE)
//   Pages 9-11  (12 B): diameterTolerance(uint16 LE) + 3 temps(uint8) + 3 signed
//                       offsets(int8) + RGB(3 B) + temperatureMax(uint8)
//   Page 12     (4 B):  temperatureMin/bedTemperatureMin/bedTemperatureMax(uint8 each)
//                       + reserved(1 B)
//   Page 13     (4 B):  CRC-8(1 B, over pages 4-12 + this page's first byte, i.e. all
//                       fixed numeric bytes before it) + reserved(3 B)
//   Pages 14-16 (12 B): totalLength/usedLength (uint24 LE each, 6 B) + cost (uint16 LE,
//                       fixed-point x100, 2 B) + firstUse(uint16 LE, 2 B) + reserved(2 B)
//   Pages 17-18 (8 B):  lastUse/purchasedOn (uint16 LE each, days since 1970-01-01)
//   Pages 19+   :       8 length-prefixed strings (vendor/material/colorName/code/
//                       batchNumber/purchasedFrom/finish/displayName), same
//                       [1-byte len][UTF-8 bytes] convention as Mifare/NFC-V, flat
//                       across as many pages as the tag's capacity allows
//   Last page used: commit marker, own sub-magic ('N','X') + flags2 bit0 ("strings
//                       present") -- written LAST, same discipline as every other
//                       Extended format: an aborted write never reads back as corrupt,
//                       only as "no Extended data" (magic missing) or "numeric fields
//                       only, no strings" (commit marker missing/incomplete).
static const uint8_t NTAG_EXT_MAGIC0 = 'O';
static const uint8_t NTAG_EXT_MAGIC1 = 'X';
static const uint8_t NTAG_EXT_VERSION = 0x03;  // v3: drying/td on pages 21-22 (v2: multi-colour on pages 19-20)
static const uint8_t NTAG_EXT_PAGE_START = 4;    // magic/version/flags/dbId: pages 4-5
static const uint8_t NTAG_EXT_PAGE_NUM   = 6;    // totalWeight/spoolWeight/usedWeight/remainingWeight: pages 6-7
static const uint8_t NTAG_EXT_PAGE_DENSITY = 8;  // density+diameter: page 8
static const uint8_t NTAG_EXT_PAGE_PHYS  = 9;    // diameterTolerance/temps/offsets/rgb/temperatureMax: pages 9-11
static const uint8_t NTAG_EXT_PAGE_TEMPMIN = 12; // temperatureMin/bedTemperatureMin/Max: page 12
static const uint8_t NTAG_EXT_PAGE_CRC   = 13;   // CRC-8: page 13
static const uint8_t NTAG_EXT_PAGE_LEN   = 14;   // totalLength/usedLength/cost/firstUse: pages 14-16
static const uint8_t NTAG_EXT_PAGE_DATES = 17;   // lastUse/purchasedOn: pages 17-18
static const uint8_t NTAG_EXT_PAGE_COLOR = 19;  // v2: [0..2] colour 2, [3] flags
                                                // page 20: [0..2] colour 3, [3] reserved
// v3: pages 21-22 carry drying/td, so the string buffer moves 21 -> 23. This constant is
// the single source of truth for that boundary -- fixedBuf's size, the string budget, the
// pad loop and the commit-marker scan are all derived from it, so moving it moves them
// all together. Leaving strings at 21 while writing v3 would produce a complete,
// valid-LOOKING tag with the drying bytes read back as vendor/material.
static const uint8_t NTAG_EXT_PAGE_STR   = 23;  // strings start here (19 pre-v2, 21 in v2)
static const uint8_t NTAG_EXT_PAGE_DRY   = 21;  // v3: [0] dryTemp u8, [1] pad, [2..3] dryTime u16 LE
                                                //     page 22: [0..1] td x100 u16 LE, [2..3] reserved
static const uint8_t NTAG_EXT_MAGIC2_0 = 'N';    // commit marker sub-magic (last page)
static const uint8_t NTAG_EXT_MAGIC2_1 = 'X';
// CRC coverage: pages 4-12 (36 B) + page 13's own first byte position is the CRC
// itself, so coverage is exactly pages 4-12 -- 9 pages x 4 B = 36 B.
static const int NTAG_EXT_CRC_LEN = (NTAG_EXT_PAGE_CRC - NTAG_EXT_PAGE_START) * 4;

// --- TigerTag Standard layout (foreign format, NOT our own -- byte-for-byte per the
// TigerTag-SDK-Python, Apache-2.0, tigertag/tag.py, as relayed by the SpoolManagerExtended
// peer session and cross-checked against their own tested read-parser). All
// multi-byte fields BIG-ENDIAN, unlike every OctoScale-native NTAG/Mifare layout
// above (little-endian) -- do not reuse pn5180ScaleU16/pn5180ScaleU24 here, their
// byte order is wrong for this format.
//
// Payload is exactly 80 bytes (pages 4-23, NTAG213-sized, unsigned/"Standard"
// variant) -- offsets below are relative to page 4 = byte 0. Bytes 32+ (Twin-Tag-ID,
// Color 2/3, TD, custom message, measure-available) are NOT written: no verified
// SDK source for their layout was available at implementation time (see the
// TigerTag write-support conversation) -- leaving them zeroed is the deliberate
// choice over guessing.
static const uint8_t TIGERTAG_PAGE_START = 4;
static const uint32_t TIGERTAG_MAGIC_STANDARD = 0x5BF59264;  // unsigned, what we write
// static const uint32_t TIGERTAG_MAGIC_PLUS = 0xBC0FCB97;  // signed variant -- never
// written here, would need a private key we don't have.
static const uint32_t TIGERTAG_PRODUCT_ID_GENERIC = 0xFFFFFFFF;  // "offline"/no catalog link
static const int TIGERTAG_PAYLOAD_LEN = 80;                      // bytes, pages 4-23

// Data carried between the HTTP handler (POST /nfcwritespool) and the write functions.
// Optional fields use hasX flags (JSON may omit some) -- unset numeric fields are
// written as their "not set" sentinel (0xFFFF for uint16 scaled fields, matching the
// existing octoprint.h convention of -1 meaning "not set").
struct SpoolTagData {
  long databaseId = -1;
  String material = "", vendor = "", colorName = "";
  String color = "";       // "#rrggbb" or "#rrggbb;#rrggbb..." or "rainbow"/"transparent..."
  // v4 multi-color: SpoolManagerExtended's `color` field is a small grammar, not a hex
  // string -- "[transparent:]#hex[;#hex[;#hex]]" or the bare sentinel "rainbow" (see
  // its SPOOLMANAGER_UTILS.composeSpoolColor/parseSpoolColor). Up to THREE colours (the
  // UI has three pickers), plus two independent flags. Earlier formats stored only the
  // first colour and dropped the rest silently; these fields carry the whole value.
  // colorCount 0 with isTransparent set is legitimate ("transparent", untinted).
  uint8_t colorRgb[3][3] = {{0,0,0},{0,0,0},{0,0,0}};  // up to 3 colours, RGB each
  uint8_t colorCount = 0;      // how many of colorRgb[] are valid (0-3)
  bool isTransparent = false;  // "transparent:" prefix
  bool isRainbow = false;      // bare "rainbow" sentinel
  float diameter = -1;     // mm
  float diameterTolerance = -1;
  float density = -1;      // g/cm^3
  float totalWeight = -1, spoolWeight = -1, usedWeight = -1;  // g
  int temperature = -1, bedTemperature = -1, enclosureTemperature = -1;      // deg C
  int offsetTemperature = 0, offsetBedTemperature = 0, offsetEnclosureTemperature = 0;  // signed, deg C
  // Optional range around temperature/bedTemperature (SpoolManagerExtended's minTemperature/
  // maxTemperature/minBedTemperature/maxBedTemperature) -- temperature/bedTemperature
  // themselves stay the independent "target" value, not the range's min.
  int temperatureMin = -1, temperatureMax = -1;
  int bedTemperatureMin = -1, bedTemperatureMax = -1;
  // octoscaleExtended v3 fields (Mifare Classic 1K only so far -- see MIFARE_EXT_BLOCK16
  // and the v3 string buffer). -1/"" = not set, same convention as every other field.
  float remainingWeight = -1;              // g
  long totalLength = -1, usedLength = -1;  // mm
  String code = "", batchNumber = "", purchasedFrom = "", finish = "", displayName = "";
  long firstUse = -1, lastUse = -1, purchasedOn = -1;  // days since 1970-01-01
  // Time of day for the three dates above, 0..1439 (-1 = not set), stored SEPARATELY
  // from the day count rather than folded into it. SpoolManagerExtended's dates carry a real
  // time (13 of 13 used spools in their test DB had one), which used to be truncated on
  // write -- so every rewrite of an unchanged spool showed a date diff. Minutes since
  // the epoch would be the obvious encoding and is the one to avoid: it is ~29.8M today,
  // and these fields go over the wire as uint16, so pn5180ScaleU16 would clamp every
  // single value to 0xFFFF ("not set") and silently drop the date along with the time.
  // Keeping day and minute apart also means an older reader still sees exactly the day
  // it always saw, instead of a half-understood combined value.
  int firstUseMinuteOfDay = -1, lastUseMinuteOfDay = -1, purchasedOnMinuteOfDay = -1;
  float cost = -1;                          // 2 decimal places

  // Drying fields (OpenPrintTag spec keys 57/58) plus td (key 27). No octoscaleExtended
  // layout carries any of them. OPT carries all three; TigerTag carries the two drying
  // fields (bytes 28/29) but not td.
  //
  // dryingTime is in MINUTES everywhere on THIS side of the boundary -- the OPT spec
  // unit, and what /nfcprobe reports and /nfcwritespool accepts. Per-layout conversion
  // is the individual writer's job: OPT stores minutes verbatim, TigerTag's byte 29 is
  // HOURS and divides (see pn5180WriteNtagTigerTag). Do not "simplify" that division
  // away -- without it every TigerTag drying time is off by a factor of 60, which is
  // exactly the bug that was measured on hardware (3h written -> 180 in byte 29).
  int dryingTemperature = -1;   // deg C
  int dryingTime = -1;          // minutes (spec unit; 8 h arrives here as 480)
  // Transmission distance: a DIMENSIONLESS opacity number (0.1 = most opaque,
  // 100 = most transparent), not a length. Never label it mm.
  float td = -1;

  // TigerTag-only: pre-resolved IDs into TigerTag's own Material/Brand/Aspect/Type/
  // Diameter/MeasureUnit registries. The firmware does NOT own or look up this
  // registry -- the caller (SpoolManagerExtended) resolves text values to these IDs before
  // sending, same "plugin resolves, firmware just packs bytes" split already used
  // for every other format. -1 = not resolved/not set.
  long tigerTagMaterialId = -1, tigerTagBrandId = -1, tigerTagAspectId = -1;
  long tigerTagTypeId = -1, tigerTagDiameterId = -1, tigerTagMeasureUnitId = -1;
};

// CRC-8, polynomial 0x07 (the plan's chosen poly), no reflection, init 0x00 -- a small,
// dependency-free implementation since none of this project's existing libraries
// expose a CRC-8.
inline uint8_t pn5180Crc8(const uint8_t *data, int len) {
  uint8_t crc = 0x00;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

// Parses the first color out of SpoolManagerExtended's `color` field: "#rrggbb", optionally
// followed by more colors separated by ';' (multi-color spools -- only the first is
// storable in the 3-byte RGB slot), or a sentinel like "rainbow"/"transparent[:#hex]"
// (neither has a single RGB value -- returns false, caller leaves the RGB bytes zeroed).
// Parses SpoolManagerExtended's full `color` grammar into the v4 fields. The single-
// colour pn5180ParseColorHex below stays as-is: every pre-v4 write path still uses it,
// and the primary colour must keep landing in the same 3-byte slot it always has.
//
//   "rainbow"                       -> rainbow flag, no colours
//   "transparent"                   -> transparent flag, no colours (untinted)
//   "transparent:#rgb[;#rgb[;#rgb]]"-> transparent flag + 1-3 colours
//   "#rgb[;#rgb[;#rgb]]"            -> 1-3 colours, opaque
//
// Anything unparseable leaves the outputs at "nothing set" rather than guessing --
// a wrong colour on a tag is worse than no colour, same reasoning as everywhere else.
// Packs the v4 colour flags into one byte, shared by all three carriers so the wire
// encoding cannot drift apart between them: bit0 transparent, bits1-2 colour count
// (0-3), bit3 rainbow. 0x00 therefore means "no colour information", which is exactly
// what a pre-v4 tag's zeroed reserve bytes read back as -- no separate presence flag
// needed.
inline bool pn5180ParseColorHex(const String &color, uint8_t rgbOut[3]);  // defined below

inline uint8_t pn5180PackColorFlags(const SpoolTagData &d) {
  uint8_t f = 0;
  if (d.isTransparent) f |= 0x01;
  f |= (uint8_t)((d.colorCount & 0x03) << 1);
  if (d.isRainbow) f |= 0x08;
  return f;
}
inline void pn5180UnpackColorFlags(uint8_t f, SpoolTagData &d) {
  d.isTransparent = (f & 0x01) != 0;
  d.colorCount = (uint8_t)((f >> 1) & 0x03);
  d.isRainbow = (f & 0x08) != 0;
}

inline void pn5180ParseColorGrammar(const String &color, uint8_t rgbOut[3][3],
                                    uint8_t &countOut, bool &transparentOut,
                                    bool &rainbowOut) {
  countOut = 0; transparentOut = false; rainbowOut = false;
  for (int i = 0; i < 3; i++) rgbOut[i][0] = rgbOut[i][1] = rgbOut[i][2] = 0;

  String c = color;
  c.trim();
  if (c.length() == 0) return;

  String lower = c; lower.toLowerCase();
  if (lower == "rainbow") { rainbowOut = true; return; }
  if (lower.startsWith("transparent")) {
    transparentOut = true;
    int colon = c.indexOf(':');
    if (colon < 0) return;          // bare "transparent", no base colours
    c = c.substring(colon + 1);
    c.trim();
  }

  // Split on ';' -- at most 3 colours are storable, extras are ignored (the UI only
  // offers three pickers, so a longer list would be malformed input rather than data).
  int start = 0;
  while (start < (int)c.length() && countOut < 3) {
    int sep = c.indexOf(';', start);
    String part = (sep < 0) ? c.substring(start) : c.substring(start, sep);
    part.trim();
    uint8_t rgb[3];
    if (pn5180ParseColorHex(part, rgb)) {
      rgbOut[countOut][0] = rgb[0]; rgbOut[countOut][1] = rgb[1]; rgbOut[countOut][2] = rgb[2];
      countOut++;
    }
    if (sep < 0) break;
    start = sep + 1;
  }
}

// Inverse of pn5180ParseColorGrammar: rebuilds the grammar string from the parsed
// parts. Must stay an exact mirror of the parser above, or a /nfcprobe result fed back
// into /nfcwritespool would not round-trip. "transparent" with zero colours is a
// legitimate state (untinted), see the colorCount comment on SpoolTagData.
inline String pn5180ComposeColorGrammar(const uint8_t rgb[3][3], uint8_t count,
                                        bool transparent, bool rainbow) {
  if (rainbow) return String("rainbow");   // parser returns early on it too, count ignored
  String out;
  if (transparent) out = "transparent";
  if (count == 0) return out;              // "" or bare "transparent"
  char hx[10];
  for (uint8_t i = 0; i < count && i < 3; i++) {
    snprintf(hx, sizeof(hx), "#%02X%02X%02X", rgb[i][0], rgb[i][1], rgb[i][2]);
    out += (i == 0) ? (transparent ? ":" : "") : ";";
    out += hx;
  }
  return out;
}

// Single decode point for the primary colour of the three Extended carriers.
//
// Why this exists rather than a condition at each read site: the primary colour's three
// bytes carry no "unset" encoding of their own -- black is 0,0,0, and so is an untouched
// buffer. The only authority on whether a colour was stored is the flag byte's
// colorCount (see pn5180PackColorFlags), and that byte lives in a different block/page
// that is itself version-gated and read further down. Deciding at the byte site is
// therefore guessing; this runs once the flags are actually in hand.
//
// haveFlags=false means the tag predates its carrier's flag byte (Mifare < v4,
// NFC-V < v3, NTAG < v2 -- the counters run per carrier and do NOT line up). There the
// information was never written, so black and unset are indistinguishable: fall back to
// the historic "non-zero means set" reading and leave 0,0,0 empty rather than inventing
// a black that could overwrite a real spool colour on import.
inline void pn5180ApplyExtendedColor(SpoolTagData &out, const uint8_t primary[3],
                                     bool haveFlags, uint8_t flagByte) {
  if (haveFlags) {
    pn5180UnpackColorFlags(flagByte, out);
    // colorCount==0 with isTransparent is valid ("transparent", untinted) -- must not
    // be overwritten with the zeroed primary bytes.
    if (out.colorCount >= 1)
      for (int i = 0; i < 3; i++) out.colorRgb[0][i] = primary[i];
  } else if (primary[0] || primary[1] || primary[2]) {
    for (int i = 0; i < 3; i++) out.colorRgb[0][i] = primary[i];
    out.colorCount = 1;
  }
  // out.color stays the PRIMARY colour as plain "#RRGGBB" -- the full grammar is what
  // colorFull carries (see /nfcprobe). Widening this field would change a published
  // value the web UI displays.
  if (out.colorCount >= 1) {
    char hx[10];
    snprintf(hx, sizeof(hx), "#%02X%02X%02X",
             out.colorRgb[0][0], out.colorRgb[0][1], out.colorRgb[0][2]);
    out.color = hx;
  }
}

// Primary colour for the single-RGB slot every carrier has had since v1. Prefers the
// already-parsed grammar result (d.colorRgb[0]) over re-parsing d.color, because
// pn5180ParseColorHex alone returns false for "transparent:#ff0000;..." -- the string
// starts with 't', not '#'. That is exactly right for a plain hex parser and exactly
// wrong here: it silently dropped the primary colour of every transparent multi-colour
// spool, writing 0,0,0 while colours 2 and 3 came through fine.
inline bool pn5180PrimaryColorRgb(const SpoolTagData &d, uint8_t rgbOut[3]) {
  if (d.colorCount > 0) {
    for (int i = 0; i < 3; i++) rgbOut[i] = d.colorRgb[0][i];
    return true;
  }
  return pn5180ParseColorHex(d.color, rgbOut);  // pre-grammar callers / plain "#rrggbb"
}

inline bool pn5180ParseColorHex(const String &color, uint8_t rgbOut[3]) {
  if (color.length() < 7 || color[0] != '#') return false;  // sentinels don't start with '#'
  char buf[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    buf[0] = color[1 + i*2]; buf[1] = color[2 + i*2];
    for (int c = 0; c < 2; c++) if (!isxdigit((unsigned char)buf[c])) return false;
    rgbOut[i] = (uint8_t)strtoul(buf, nullptr, 16);
  }
  return true;
}

// Truncates a UTF-8 string to at most maxBytes bytes WITHOUT cutting a multi-byte
// sequence in half (a continuation byte has the top two bits 10xxxxxx -- back off until
// the cut point isn't one).
inline String pn5180Utf8SafeTruncate(const String &s, int maxBytes) {
  if ((int)s.length() <= maxBytes) return s;
  int cut = maxBytes;
  while (cut > 0 && (((uint8_t)s[cut]) & 0xC0) == 0x80) cut--;
  return s.substring(0, cut);
}

inline const char *pn5180TagTypeName(PN5180TagType t) {
  switch (t) {
    case PN5180_TAG_NFCV: return "NFC-V (ISO15693)";
    case PN5180_TAG_NFCA: return "NFC-A (ISO14443-A)";
    default:              return "no tag";
  }
}

// Rough SAK interpretation (a hint only; a full read would need ATS). Good enough for
// the debug view. atqa/sak are the final values after the cascade completes.
inline const char *pn5180NfcaProduct(uint16_t atqa, uint8_t sak) {
  if (sak & 0x20) return "ISO14443-4 (smartcard/DESFire?)";
  // SAK 0x00 = type-2 tag (not ISO14443-4). ATQA bits 6-7 of the LSB = UID size.
  if ((sak & ~0x04) == 0x00) {
    uint8_t uidSize = (atqa >> 6) & 0x03;  // 0=4B (single), 1=7B (double), 2=10B (triple)
    if (uidSize == 1) return "NTAG / Ultralight (7-byte)";
    return "Type 2 (Ultralight-like)";
  }
  if (sak == 0x08) return "Mifare Classic 1K";
  if (sak == 0x18) return "Mifare Classic 4K";
  if (sak == 0x09) return "Mifare Mini";
  if (sak == 0x28) return "Mifare Classic (ISO14443-4)";
  return "Mifare/NFC-A";
}

#include <SPI.h>
#include <PN5180.h>
#include <PN5180ISO15693.h>

// Pins defined in main.cpp, referenced here as extern.
extern const int PN5180_NSS;
extern const int PN5180_BUSY;
extern const int PN5180_RST;
extern const int SPI_SCK;
extern const int SPI_MOSI;
extern const int SPI_MISO;

static PN5180ISO15693 *g_pn5180 = nullptr;
static bool g_pn5180Ready = false;
static int g_pn5180LastRc = 999;      // last getInventory code (HTTP diagnostics)
static uint32_t g_pn5180OkCount = 0;  // total tags successfully read

// Initializes the PN5180 (ISO15693/NFC-V). Reads the chip version to verify it's alive
// and sets up the RF field. Returns true = the chip answered plausibly.
// Debug hook for PN5180::mifareAuthenticate (see PN5180.h) -- logs the raw send/recv
// frame bytes via our own debug console (fully inert while it's off, like all dbgLog*).
// Off by default even when the debug log is on: with a Mifare tag resting on the
// reader, the 500 ms poll authenticates every single time, so this callback alone
// produced TWO lines every 500 ms -- enough to churn through the whole ring buffer in
// minutes and bury everything else in it. Flip g_pn5180AuthTrace to true (via
// /nfcauthtrace?on=1) only while actually chasing an auth/Crypto1 problem.
volatile bool g_pn5180AuthTrace = false;
static void pn5180MifareAuthDebugCb(const uint8_t *sendBuf, size_t sendLen,
                                    const uint8_t *recvBuf, size_t recvLen, bool ok) {
  if (!g_dbgLogEnabled || !g_pn5180AuthTrace) return;
  String hex;
  char b[4];
  for (size_t i = 0; i < sendLen; i++) { snprintf(b, sizeof(b), "%02X ", sendBuf[i]); hex += b; }
  if (!recvBuf) {
    dbgLogf("Mifare auth SPI: TX %s", hex.c_str());
  } else {
    String rhex;
    for (size_t i = 0; i < recvLen; i++) { snprintf(b, sizeof(b), "%02X ", recvBuf[i]); rhex += b; }
    dbgLogf("Mifare auth SPI: TX %s RX %s ok=%d", hex.c_str(), rhex.c_str(), ok);
  }
}

inline bool pn5180Init() {
  // Start SPI with our pins (the library later calls SPI.begin() without pins, so we
  // set them beforehand; SS=-1, the library drives CS itself via NSS).
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, -1);

  PN5180::mifareAuthDebugCb = pn5180MifareAuthDebugCb;

  g_pn5180 = new PN5180ISO15693((uint8_t)PN5180_NSS, (uint8_t)PN5180_BUSY, (uint8_t)PN5180_RST);
  g_pn5180->begin();
  g_pn5180->reset();

  uint8_t product[2] = {0, 0}, firmware[2] = {0, 0};
  g_pn5180->readEEprom(PRODUCT_VERSION, product, 2);
  g_pn5180->readEEprom(FIRMWARE_VERSION, firmware, 2);
  Serial.printf("PN5180: product %d.%d, firmware %d.%d\n",
                product[1], product[0], firmware[1], firmware[0]);

  uint16_t fw = (firmware[1] << 8) | firmware[0];
  if (fw == 0x0000 || fw == 0xFFFF) {
    Serial.println("PN5180: no plausible response (check SPI/wiring)");
    g_pn5180Ready = false;
    return false;
  }
  g_pn5180->setupRF();  // set up the RF field for ISO15693
  Serial.println("PN5180: ready (ISO15693/NFC-V, RF active)");
  g_pn5180Ready = true;
  return true;
}

inline bool pn5180IsReady() { return g_pn5180Ready; }
inline int pn5180LastRc() { return g_pn5180LastRc; }
inline uint32_t pn5180OkCount() { return g_pn5180OkCount; }

// --- Tag-type detection for debug mode --------------------------------------
// The PN5180 chip supports many protocols; the atrappmann library only covers
// ISO15693. For the debug menu, type detection is enough: try both common NFC
// families per poll and report whichever answered.
//   - NFC-V  (ISO15693): tags like ICODE SLIX, common spool tags.
//   - NFC-A  (ISO14443-A): NTAG21x, Mifare Ultralight/Classic, most phone tags.
// NFC-A is hand-built on the base class's low-level primitives (REQA->ATQA,
// anticollision->UID, SELECT->SAK). loadRFConfig indices for 106 kbps type-A: 0x00/0x80.

// Tries to read an ISO15693 tag. On success, writes the 8-byte UID as a hex string
// (big-endian, usual display order) to uidHexOut. Returns true = a tag is present.
// Non-blocking: no tag -> false, no delay.
inline bool pn5180ReadTag(String &uidHexOut) {
  uidHexOut = "";
  if (!g_pn5180Ready || !g_pn5180) return false;
  uint8_t uid[8] = {0};
  ISO15693ErrorCode rc = g_pn5180->getInventory(uid);
  g_pn5180LastRc = (int)rc;  // for HTTP diagnostics (/nfc5180)
  if (rc != ISO15693_EC_OK) {
    // A failed inventory leaves the RF state stuck -> reset + setupRF, otherwise no
    // tag is ever detected afterward (per the library's own example).
    g_pn5180->reset();
    g_pn5180->setupRF();
    return false;
  }
  g_pn5180OkCount++;
  char buf[3];  // UID arrives LSB-first -> reverse for the usual display order
  for (int i = 7; i >= 0; i--) {
    snprintf(buf, sizeof(buf), "%02X", uid[i]);
    uidHexOut += buf;
  }
  return true;
}

// Re-arms the RF field (if the reader got stuck after an error).
inline void pn5180Recover() {
  if (!g_pn5180) return;
  g_pn5180->reset();
  g_pn5180->setupRF();
}

// --- NFC-A (ISO14443-A) low-level probe -------------------------------------
// Implemented only as far as the debug menu needs: wake the card (REQA/WUPA), read
// ATQA, cascade-1 anticollision for the UID, SELECT for the SAK. No full ISO14443-4
// stack, no memory read — just type + UID.
//
// Uses the PN5180 base class's public low-level methods: loadRFConfig / setRF_on /
// setRF_off / sendData(with validBits) / readData / getIRQStatus / clearIRQStatus /
// readRegister. Returns true = an NFC-A card answered (out filled in).
//
// CRC registers: for ISO14443-A REQA/ATQA/anticollision, CRC MUST be OFF on both TX
// and RX (these frames carry no CRC, only parity). Without this the PN5180 discards
// the ATQA and the card is never detected. Bit 0 of each register is the CRC enable.
#ifndef CRC_TX_CONFIG
#define CRC_TX_CONFIG (0x19)
#endif

// Waits for the RX IRQ or a timeout (ms). true = data received.
static inline bool pn5180WaitRx(PN5180 *p, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (p->getIRQStatus() & RX_IRQ_STAT) return true;
    delay(1);
  }
  return false;
}

// sendData() rejects the command if the chip isn't in WaitTransmit state yet (checked
// once, no retry inside the library). Right after the AndMask/OrMask register writes
// that request Idle->Transceive, the chip needs a moment to actually get there -> retry
// a few times with a short delay instead of failing the whole write on the first miss.
static inline bool pn5180SendDataRetry(PN5180 *p, uint8_t *data, int len, uint8_t validBits = 0) {
  for (int attempt = 0; attempt < 20; attempt++) {
    if (p->sendData(data, len, validBits)) return true;
    int state = (int)p->getTransceiveState();
    dbgLogf("  sendData retry %d: transceiveState=%d", attempt, state);
    // state 3/4/5 = still wait-receive/wait-for-data/receiving from the PREVIOUS
    // transceive cycle. Just re-arming Idle/Transceive doesn't clear it (confirmed:
    // stays stuck at state=3 for 20 straight retries) -> also clear IRQ status each
    // time, matching the full reset sequence used elsewhere (clearIRQStatus first).
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);  // Idle/StopCom
    delay(2);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);   // Transceive
    delay(2);
  }
  return false;
}

inline bool pn5180ProbeNfcA(PN5180ProbeResult &out) {
  if (!g_pn5180) return false;
  PN5180 *p = g_pn5180;  // base-class pointer (public inheritance)

  // Reload RF for ISO14443-A/106k (switches out of the active ISO15693 mode).
  p->reset();
  if (!p->loadRFConfig(0x00, 0x80)) { return false; }  // 0x00 TX, 0x80 RX = type-A 106k
  if (!p->setRF_on()) { return false; }
  p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);  // arm transceive state
  p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
  delay(2);

  p->writeRegisterWithAndMask(CRC_RX_CONFIG, 0xfffffffe);  // CRC off TX+RX
  p->writeRegisterWithAndMask(CRC_TX_CONFIG, 0xfffffffe);

  // --- REQA (0x26), 7 valid bits -> ATQA (2 bytes) ---
  p->clearIRQStatus(0xffffffff);
  p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);  // reset to idle->transceive
  p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
  uint8_t reqa = 0x26;
  if (!p->sendData(&reqa, 1, 7)) { return false; }  // validBits=7 (short frame)
  if (!pn5180WaitRx(p, 15)) { return false; }       // no card
  uint32_t rxStatus = 0;
  p->readRegister(RX_STATUS, &rxStatus);
  uint16_t atqaLen = (uint16_t)(rxStatus & 0x1ff);
  if (atqaLen < 2) { return false; }
  uint8_t *atqa = p->readData(2);
  if (!atqa) { return false; }
  out.atqa = (uint16_t)atqa[0] | ((uint16_t)atqa[1] << 8);

  // --- Cascade loop (CL1=0x93, CL2=0x95) for the full UID + final SAK ---
  // 4-byte UID: CL1 only. 7-byte UID: CL1's response starts with CT=0x88 + UID[0..2],
  // CL1's SAK has bit2 set ("UID incomplete") -> CL2 returns UID[3..6]. Per cascade:
  // anticollision (CRC off) -> UID part, then SELECT (CRC on) -> SAK.
  const uint8_t selCode[2] = { 0x93, 0x95 };  // CL1, CL2
  uint8_t uidBytes[7];
  int uidCount = 0;
  char buf[3];
  out.uid = "";

  for (int cl = 0; cl < 2; cl++) {
    // -- Anticollision: CRC off, SEL + NVB=0x20 -> 5 bytes (4 UID/CT + BCC) --
    p->writeRegisterWithAndMask(CRC_RX_CONFIG, 0xfffffffe);
    p->writeRegisterWithAndMask(CRC_TX_CONFIG, 0xfffffffe);
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
    uint8_t anticoll[2] = { selCode[cl], 0x20 };
    if (!p->sendData(anticoll, 2, 0)) break;
    if (!pn5180WaitRx(p, 15)) break;
    p->readRegister(RX_STATUS, &rxStatus);
    uint16_t uidLen = (uint16_t)(rxStatus & 0x1ff);
    if (uidLen < 4) break;
    uint8_t *r = p->readData(5);   // 4 UID/CT bytes + BCC
    if (!r) break;

    // r[0..3] = UID part (a 7-byte UID's CL1 starts with CT=0x88).
    bool ct = (cl == 0 && r[0] == 0x88);  // cascade tag only in CL1 of a 7-byte UID
    for (int i = (ct ? 1 : 0); i < 4 && uidCount < 7; i++) {
      uidBytes[uidCount++] = r[i];
    }

    // -- SELECT: CRC on, SEL + 0x70 + 4 UID/CT bytes + BCC -> SAK --
    p->writeRegisterWithOrMask(CRC_RX_CONFIG, 0x00000001);
    p->writeRegisterWithOrMask(CRC_TX_CONFIG, 0x00000001);
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
    uint8_t sel[7] = { selCode[cl], 0x70, r[0], r[1], r[2], r[3], r[4] };
    if (!p->sendData(sel, 7, 0)) break;
    if (!pn5180WaitRx(p, 15)) break;
    p->readRegister(RX_STATUS, &rxStatus);
    if ((uint16_t)(rxStatus & 0x1ff) < 1) break;
    uint8_t *sakResp = p->readData(1);
    if (!sakResp) break;
    out.sak = sakResp[0];

    if (!(out.sak & 0x04)) break;  // SAK bit2 = UID incomplete -> next cascade
  }

  // Every failure above is a `break` out of the cascade loop, which leaves whatever
  // CL1 delivered in uidBytes -- typically 3 bytes (4 minus the 0x88 cascade tag).
  // Without this check that fragment was returned as a successful read: NFC-A UIDs are
  // 4 or 7 bytes, never 3, so it is structurally impossible yet looked valid to every
  // caller. Consequences seen in the field: it differed from g_lastUid, so the poll
  // treated it as a new tag and refilled the extended-read cache with unreadable data
  // (hasExtended 1 -> 0, id 103 -> -1 on a tag that never moved, plus a pointless
  // byCode HTTP lookup and a "spool gone" flicker on the TFT); and via g_pn5180Uid it
  // reached /nfcwritestatus, where SpoolManagerExtended derives rfidTagKey from it -- a
  // truncated UID yields a well-formed but WRONG key, which their teach-in wrote
  // straight to the database, permanently unlinking the tag. Report it as "no tag"
  // instead: a partial anticollision run is a failed read, not a new tag.
  if (uidCount != 4 && uidCount != 7) return false;

  for (int i = 0; i < uidCount; i++) {
    snprintf(buf, sizeof(buf), "%02X", uidBytes[i]);
    out.uid += buf;
  }

  out.type = PN5180_TAG_NFCA;
  return true;
}

// Forward declarations: the ID readers are defined further below (needed by both
// pn5180Probe, for the debug view, and pn5180ReadSpool, for the real read path).
inline bool pn5180ReadNtagId(long &idOut, String &textOut);
inline bool pn5180ReadNfcvId(const uint8_t *uidLsb, long &idOut, String &textOut);
inline bool pn5180ReadMifareId(const String &uidHex, long &idOut, String &textOut);
inline bool pn5180ReadMifareExtended(const String &uidHex, SpoolTagData &out);
inline bool pn5180ReadNtagOpenSpool(SpoolTagData &out);
inline bool pn5180ReadNtagExtended(SpoolTagData &out);
inline bool pn5180ReadNtagTigerTag(SpoolTagData &out);
inline bool pn5180ReadNfcvExtended(const uint8_t uid[8], SpoolTagData &out);
inline bool pn5180ReadNfcvOpenSpool(const uint8_t uid[8], SpoolTagData &out);
// Needed by pn5180ReadSpoolEx's optional capacity output (nfcvOpenSpool only).
inline int pn5180NfcvUserBytes(const uint8_t uid[8], uint8_t *bsOut = nullptr, uint8_t *nbOut = nullptr);
// Needed by pn5180Probe's optional CC read (debug view only, see readCc param below).
inline bool pn5180NtagReadPages(uint8_t startPage, int count, uint8_t *out);
inline bool pn5180NtagCcIsVirgin(const uint8_t cc[4], bool &alreadyNdefOut);
inline bool pn5180NfcvReadBlock(const uint8_t uid[8], uint8_t block, uint8_t out[4]);
inline bool pn5180NfcvCcIsVirgin(const uint8_t cc[4], bool &alreadyNdefOut);
// Needed by pn5180Probe's page-count read (debug view only, NFC-A/NTAG branch).
enum NtagVariant { NTAG_UNKNOWN = 0, NTAG_213, NTAG_215, NTAG_216 };
inline int ntagUserBytes(NtagVariant v);
inline NtagVariant pn5180NtagGetVersion();

// --- Combined debug probe: NFC-V first, then NFC-A --------------------------
// Always leaves the reader back in ISO15693 mode, so the normal (debug-off) poll
// keeps working without a restart.
// Also attempts the databaseId parse (idParsed/idText) exactly like the real read path
// (pn5180ReadSpool) would -- so the WebUI/TFT debug view shows precisely what the
// normal flow would see (e.g. "no valid ID -> would fall back to a UID lookup").
//
// readCc (default false, only the debug-mode caller in main.cpp passes true, and only
// on a UID change): also reads the Capability Container (out.ccState) for NFC-A/NFC-V.
// Done INLINE here, while the card is already selected, rather than as a separate
// pn5180ProbeCcState() re-select afterwards -- an earlier version did exactly that and
// caused a real bug: the extra reset()+setupRF()+re-select cycle right after this
// function's own reset()+setupRF() was enough RF churn to occasionally corrupt the
// NEXT poll's anticollision read (observed live: a truncated 3-byte UID instead of the
// full 4, logged as a spurious extra "new tag" event). Reading the CC before the first
// reset() avoids the extra cycle entirely.
inline bool pn5180Probe(PN5180ProbeResult &out, bool readCc = false) {
  out = PN5180ProbeResult();
  if (!g_pn5180Ready || !g_pn5180) return false;

  // 1) NFC-V (ISO15693) — the reader's normal resting mode.
  uint8_t uidRaw[8] = {0};
  ISO15693ErrorCode rcV = g_pn5180->getInventory(uidRaw);
  g_pn5180LastRc = (int)rcV;
  if (rcV == ISO15693_EC_OK) {
    g_pn5180OkCount++;
    out.type = PN5180_TAG_NFCV;
    char buf[3];
    for (int i = 7; i >= 0; i--) { snprintf(buf, sizeof(buf), "%02X", uidRaw[i]); out.uid += buf; }
    uint8_t bs = 0, nb = 0, uidTmp[8];
    memcpy(uidTmp, uidRaw, 8);
    if (g_pn5180->getSystemInfo(uidTmp, &bs, &nb, &out.dsfid, &out.dsfidPresent,
                                 &out.afi, &out.afiPresent,
                                 &out.icRef, &out.icRefPresent) == ISO15693_EC_OK) {
      out.blockSize = bs; out.numBlocks = nb;
    }
    pn5180ReadNfcvId(uidRaw, out.idParsed, out.idText);  // best effort, debug view only
    if (readCc) {
      uint8_t cc[4];
      memcpy(uidTmp, uidRaw, 8);
      if (pn5180NfcvReadBlock(uidTmp, /*NFCV_OS_CC_BLOCK=*/0, cc)) {
        bool alreadyNdef;
        bool virgin = pn5180NfcvCcIsVirgin(cc, alreadyNdef);
        // pn5180NfcvCcIsVirgin only recognizes the 4-byte-CC magic (0xE1); a foreign
        // tag could carry the 8-byte form (0xE2, see HARDWARE.md) -- also "ndef" here
        // even though we only ever write the 4-byte form ourselves.
        out.ccState = virgin ? "virgin" : (alreadyNdef || cc[0] == 0xE2) ? "ndef" : "other";
      }
    }
    return true;
  }
  g_pn5180->reset();
  g_pn5180->setupRF();

  // 2) NFC-A (ISO14443-A) — temporarily switch the reader to type-A.
  bool foundA = pn5180ProbeNfcA(out);
  if (foundA) {
    bool isClassic = (out.sak == 0x08 || out.sak == 0x18 || out.sak == 0x09 || out.sak == 0x28);
    if (isClassic) {
      pn5180ReadMifareId(out.uid, out.idParsed, out.idText);
      // Mifare Classic has no CC concept -- out.ccState stays "".
      // Block count is fixed per SAK (sector layout is fully determined by the chip
      // variant, no on-tag query needed): 1K = 16 sectors x 4 blocks, 4K = 32x4 + 8x16,
      // Mini = 5 sectors x 4 blocks. The ISO14443-4 case (SAK 0x28) isn't a fixed Mifare
      // Classic layout, so it's left unset rather than guessed.
      if (out.sak == 0x08) { out.numPages = 64; out.numPagesPresent = true; }
      else if (out.sak == 0x18) { out.numPages = 256; out.numPagesPresent = true; }
      else if (out.sak == 0x09) { out.numPages = 20; out.numPagesPresent = true; }
    } else {
      // GET_VERSION FIRST, before any READ. Order matters and cost a while to pin down:
      // this used to sit after pn5180ReadNtagId() below and timed out every single time
      // (both attempts, back to back), so the debug view kept reporting the conservative
      // NTAG213 fallback for an actual NTAG215. A plain READ (0x30) ends the window in
      // which the tag still answers GET_VERSION (0x60), so it has to go directly after
      // the SELECT -- which is exactly what pn5180WriteNtagOpenSpool does, and why the
      // write path always got the variant right while this one never did.
      NtagVariant variant = pn5180NtagGetVersion();
      if (variant != NTAG_UNKNOWN) {
        out.numPages = ntagUserBytes(variant) / 4 + 4;  // user pages + pages 0-3 (UID/lock/CC)
        out.numPagesPresent = true;
      }
      pn5180ReadNtagId(out.idParsed, out.idText);  // card still selected
      if (readCc) {
        uint8_t cc[4];
        if (pn5180NtagReadPages(3, 1, cc)) {
          bool alreadyNdef;
          bool virgin = pn5180NtagCcIsVirgin(cc, alreadyNdef);
          out.ccState = virgin ? "virgin" : alreadyNdef ? "ndef" : "other";
        }
      }
    }
  }

  g_pn5180->reset();  // back to ISO15693 mode (for the next V poll / normal operation)
  g_pn5180->setupRF();

  return foundA;
}

// --- Read the databaseId off a tag (both types) ------------------------------
// Parses the legacy ID layout: ASCII decimal digits followed by space/NUL padding to
// the full field width -- exactly what pn5180WriteNtagId/pn5180WriteMifareId/
// pn5180WriteNfcvId produce (digits, then ' ' padding out to PN5180_ID_BYTES).
// Returns the parsed ID (>=0) or -1. textOut = the visible raw characters (for the UI).
//
// The trailing bytes are VALIDATED, not just used as a terminator. They used to be
// treated as "stop here, whatever follows" -- which made any foreign tag carrying
// leading ASCII digits parse as one of our IDs. A real case: an NTAG with an inventory
// number "81" + spaces on page 4 and a foreign NDEF CC reported idParsed=81, i.e. a
// third-party tag claiming to be spool 81 (see also the occupancy cross-check in
// pn5180ApplyIdTrust, which this function deliberately does NOT do itself -- it has no
// access to the CC and only judges the field's own shape).
//
// This cannot make the legacy layout self-describing: it carries no magic, so digits
// with correct padding stay indistinguishable from a foreign tag that happens to look
// the same. It only rejects the shapes our writers can never produce. Callers that need
// the stronger judgement must combine this with occupancy (see PN5180ProbeResult::
// idSource / pn5180ApplyIdTrust).
static long pn5180ParseIdBytes(const uint8_t *bytes, int len, String &textOut) {
  textOut = "";
  String digits = "";
  int i = 0;
  for (; i < len; i++) {
    char c = (char)bytes[i];
    if (c >= '0' && c <= '9') { digits += c; textOut += c; }
    else break;
  }
  if (digits.length() == 0) return -1;
  // The digit run must END inside the buffer, i.e. at least one padding byte must be
  // visible. Digits running up to the last byte mean the field was truncated and the
  // value is a PREFIX of the real id -- "1234567890" read as 8 bytes yields 12345678,
  // a different, plausible-looking, entirely wrong spool. Callers pass a variable
  // length here (pn5180ReadNfcvId passes however many bytes it managed to read), so
  // this is reachable in practice, not just in theory.
  if (i >= len) { textOut = ""; return -1; }
  // Everything after the digits must be padding (space or NUL). Our writers pad with
  // ' '; an erased/short field reads back NUL. Anything else -- another digit run, a
  // letter, binary -- is not our layout.
  for (; i < len; i++) {
    char c = (char)bytes[i];
    if (c != ' ' && c != 0) { textOut = ""; return -1; }
  }
  // A leading zero is never produced by String(long) for a value we wrote, so "0081"
  // is somebody else's fixed-width field, not our ID. Plain "0" stays valid.
  if (digits.length() > 1 && digits[0] == '0') { textOut = ""; return -1; }
  // 10 digits is the widest uint32; more cannot be one of our IDs and would overflow
  // toInt() silently.
  if (digits.length() > 10) { textOut = ""; return -1; }
  return digits.toInt();
}

// Cross-checks a legacy-parsed id against the tag's occupancy and downgrades it when
// the two disagree. This is the one place the two judgements meet: the ID parser reads
// a fixed offset and knows nothing about the CC, while occupancy reads the CC and knows
// nothing about the ID field. Until this existed they could contradict each other in a
// single /nfcprobe response -- occupancy "foreign" (this is somebody else's format)
// next to idParsed 81 (this is our spool 81), which is what the SpoolManager plugin ran
// into on a third-party NTAG carrying an inventory number.
//
// Only "legacy" ids are downgraded. An "extended" id matched a real magic, so a
// "foreign" occupancy next to it means the occupancy heuristic was wrong, not the id --
// occupancy only inspects the CC and the first user page, which an Extended tag does
// not use the way an NDEF tag does.
//
// The id is NOT cleared: /nfcprobe is a diagnostic endpoint and "there are digits here,
// but they are not credible" is more useful than a bare -1, which cannot be told apart
// from an empty field. Callers decide what to do with an "unverified" id; the load flow
// refuses it (see main.cpp's flowWouldUse).
inline void pn5180ApplyIdTrust(long idParsed, const String &occupancy,
                               bool hasExtended, String &idSourceOut) {
  // hasExtended is tested BEFORE idParsed, deliberately. On an Extended tag the legacy
  // area holds the format's own header, not digits -- an ntagExtended page 4 reads
  // "OX\x03\x00", which pn5180ParseIdBytes correctly refuses, leaving idParsed at -1.
  // The id is still on the tag, in the Extended payload. Testing idParsed first made
  // "extended" unreachable on exactly the path that reports the raw legacy parse (the
  // NFC-debug poll branch assigns the probe result wholesale, while the normal branch
  // substitutes the Extended id first), so turning NFC debug on silently downgraded a
  // verified tag to "". A trust field that depends on a debug toggle is worse than no
  // trust field, since a consumer cannot tell the two apart.
  // Both conditions, not hasExtended alone: a format can be verified AND carry no
  // OctoScale id. TigerTag is the live case (pn5180ReadSpoolExOpt's TigerTag branch
  // sets hasExtended without touching idOut, deliberately -- the format has no
  // databaseId field), and OpenPrintTag behaves the same way. Reporting "extended"
  // there would claim a trustworthy id next to idParsed == -1, which is a promise the
  // tag cannot keep.
  if (hasExtended && idParsed >= 0) { idSourceOut = "extended"; return; }
  // Verified format, no id of ours on it. Distinct from "" (nothing readable at all):
  // a consumer must not fall back to a UID lookup here and then offer to write the
  // tag, because there IS structured foreign data on it.
  if (hasExtended) { idSourceOut = "extendedNoId"; return; }
  if (idParsed < 0) { idSourceOut = ""; return; }
  idSourceOut = (occupancy == "foreign") ? "unverified" : "legacy";
}

// NFC-A/NTAG: read the ID pages via READ (0x30). Requires the card to already be
// selected (call right after pn5180ProbeNfcA, which leaves CRC on). READ always
// returns 4 pages (16 bytes) from the given page; 12 ID bytes fit starting at page 4.
inline bool pn5180ReadNtagId(long &idOut, String &textOut) {
  PN5180 *p = g_pn5180;
  idOut = -1; textOut = "";
  // CRC must be on (NTAG READ carries CRC) — left that way by ProbeNfcA's SELECT.
  p->clearIRQStatus(0xffffffff);
  p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
  p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
  uint8_t rd[2] = { 0x30, PN5180_ID_START_PAGE };  // READ from the user page
  if (!p->sendData(rd, 2, 0)) return false;
  if (!pn5180WaitRx(p, 15)) return false;
  uint32_t rxStatus = 0;
  p->readRegister(RX_STATUS, &rxStatus);
  uint16_t n = (uint16_t)(rxStatus & 0x1ff);
  if (n < PN5180_ID_BYTES) return false;   // NTAG READ returns 16 bytes
  uint8_t *data = p->readData(n > 16 ? 16 : n);
  if (!data) return false;
  idOut = pn5180ParseIdBytes(data, PN5180_ID_BYTES, textOut);
  return true;
}

// NFC-V/ISO15693: read the ID from the first blocks (the library's readSingleBlock).
// Block size comes from getSystemInfo (usually 4). Requires the tag to be in the field.
inline bool pn5180ReadNfcvId(const uint8_t *uidLsb, long &idOut, String &textOut) {
  idOut = -1; textOut = "";
  uint8_t blockSize = 4, numBlocks = 0;
  uint8_t uidTmp[8];
  memcpy(uidTmp, uidLsb, 8);
  if (g_pn5180->getSystemInfo(uidTmp, &blockSize, &numBlocks) != ISO15693_EC_OK) {
    blockSize = 4;  // fallback: most ICODE tags use 4-byte blocks
  }
  if (blockSize == 0 || blockSize > 32) blockSize = 4;
  uint8_t idBytes[PN5180_ID_BYTES];
  int got = 0;
  uint8_t blk = PN5180_ID_START_BLOCK;
  while (got < PN5180_ID_BYTES) {
    uint8_t block[32];
    memcpy(uidTmp, uidLsb, 8);
    if (g_pn5180->readSingleBlock(uidTmp, blk, block, blockSize) != ISO15693_EC_OK)
      break;
    for (int i = 0; i < blockSize && got < PN5180_ID_BYTES; i++)
      idBytes[got++] = block[i];
    blk++;
  }
  if (got == 0) return false;
  // `got`, not PN5180_ID_BYTES: only the bytes actually read are passed. A short read
  // (a block failed mid-field) must not be padded out with stale stack bytes, and
  // pn5180ParseIdBytes rejects a digit run that reaches the end of what it was given,
  // so a truncated field yields -1 rather than a prefix of the real id.
  idOut = pn5180ParseIdBytes(idBytes, got, textOut);
  return true;
}

// Combined: detects the tag type and reads the databaseId, then leaves the reader back
// in ISO15693 mode. Returns true = a tag was present (even without a valid ID).
//   typeOut = detected type, idOut = ID (>=0) or -1, uidHexOut/textOut for the UI.
inline bool pn5180ReadSpool(PN5180TagType &typeOut, long &idOut,
                            String &uidHexOut, String &textOut) {
  typeOut = PN5180_TAG_NONE; idOut = -1; uidHexOut = ""; textOut = "";
  if (!g_pn5180Ready || !g_pn5180) return false;

  // 1) NFC-V first (normal mode). getInventory returns the UID (LSB-first).
  uint8_t uid[8] = {0};
  ISO15693ErrorCode rc = g_pn5180->getInventory(uid);
  g_pn5180LastRc = (int)rc;
  if (rc == ISO15693_EC_OK) {
    g_pn5180OkCount++;
    typeOut = PN5180_TAG_NFCV;
    char b[3];
    for (int i = 7; i >= 0; i--) { snprintf(b, sizeof(b), "%02X", uid[i]); uidHexOut += b; }
    pn5180ReadNfcvId(uid, idOut, textOut);  // ID from blocks (best effort)
    g_pn5180->reset(); g_pn5180->setupRF();
    return true;
  }
  g_pn5180->reset(); g_pn5180->setupRF();

  // 2) NFC-A: probe (selects the card), then read the ID -- NTAG/Ultralight (plain
  // READ) or, for Mifare Classic, an authenticated block read (Crypto1, factory key).
  PN5180ProbeResult pr;
  if (pn5180ProbeNfcA(pr)) {
    typeOut = PN5180_TAG_NFCA;
    uidHexOut = pr.uid;
    bool isClassic = (pr.sak == 0x08 || pr.sak == 0x18 || pr.sak == 0x09 || pr.sak == 0x28);
    if (isClassic) pn5180ReadMifareId(pr.uid, idOut, textOut);  // best effort, wrong/no key -> idOut stays -1
    else pn5180ReadNtagId(idOut, textOut);                      // card is still selected
    g_pn5180->reset(); g_pn5180->setupRF();
    return true;
  }
  g_pn5180->reset(); g_pn5180->setupRF();
  return false;
}

// Extended entry point (plan C.3): tries the richer per-type formats first (Mifare
// Extended, NTAG OpenSpool), falling back to the legacy ID-only read when a tag has no
// Extended payload (no magic / no valid NDEF) or is NFC-V (never had an Extended
// format planned for it). Deliberately NOT called from the 500ms poll loop directly --
// it does 1-3 extra authenticated block reads or NDEF page reads on top of the
// existing probe, which is wasted work on every poll for a tag that isn't changing.
// Callers should only invoke this once per UID change and cache the result (see
// main.cpp's poll loop). data.databaseId mirrors idOut for callers that only care
// about the legacy field.
// formatOut (optional): the Extended format actually found on the tag, when
// hasExtended -- only meaningful for NFC-V, where either nfcvExtended or nfcvOpenSpool
// could be present (Mifare/NTAG have exactly one Extended format each, so the caller
// can infer it from typeOut/SAK without this). Left untouched when hasExtended is false.
// capacityBytesOut (optional): for an nfcvOpenSpool tag, the tag's real capacity (see
// pn5180NfcvUserBytes) -- lets the UI show the actual NDEF budget instead of
// nfcClassifyTag()'s nfcvExtended-sized default. Left untouched otherwise.
inline bool pn5180ReadSpoolEx(PN5180TagType &typeOut, long &idOut, String &uidHexOut,
                              String &textOut, SpoolTagData &data, bool &hasExtended,
                              String *formatOut = nullptr, int *capacityBytesOut = nullptr) {
  data = SpoolTagData();
  hasExtended = false;
  if (!pn5180ReadSpool(typeOut, idOut, uidHexOut, textOut)) return false;

  if (typeOut == PN5180_TAG_NFCA) {
    PN5180ProbeResult pr;
    if (pn5180ProbeNfcA(pr)) {
      bool isClassic = (pr.sak == 0x08 || pr.sak == 0x18 || pr.sak == 0x09 || pr.sak == 0x28);
      if (isClassic) {
        if (pn5180ReadMifareExtended(pr.uid, data)) { hasExtended = true; idOut = data.databaseId; }
      } else {
        // Try both NTAG formats -- disjoint page ranges (Extended's magic sits at
        // pages 4-5, OpenSpool's NDEF TLV starts at page 4 too but with a different
        // first-byte check: 0x03 for the TLV tag vs. 'O' for Extended's magic), same
        // "each format's own check rejects the other's data" pattern as NFC-V below.
        if (pn5180ReadNtagExtended(data)) {
          hasExtended = true; idOut = data.databaseId;
          if (formatOut) *formatOut = "ntagExtended";
        } else if (pn5180ReadNtagOpenSpool(data)) {
          hasExtended = true; idOut = data.databaseId;
          if (formatOut) *formatOut = "openSpool";
        } else if (pn5180ReadNtagTigerTag(data)) {
          // Foreign format, carries no OctoScale databaseId -- idOut stays whatever
          // pn5180ReadSpool() found (typically -1/unparsable on a real TigerTag tag,
          // since it has no ASCII-decimal ID at page 4). hasExtended still flips true
          // so the caller knows structured data (material/brand/... IDs) is present.
          hasExtended = true;
          if (formatOut) *formatOut = "tigerTag";
        }
      }
      g_pn5180->reset(); g_pn5180->setupRF();
    }
  } else if (typeOut == PN5180_TAG_NFCV) {
    // Extended blocks are UID-addressed, no session/selection needed -- but the raw
    // (LSB-first) UID is required, not uidHexOut's display-order hex string, so re-run
    // getInventory rather than parsing the hex back (order would have to be reversed
    // and is easy to get wrong).
    uint8_t rawUid[8] = {0};
    if (g_pn5180->getInventory(rawUid) == ISO15693_EC_OK) {
      // Try both formats -- they occupy disjoint blocks (nfcvExtended's magic sits at
      // blocks 3-4, nfcvOpenSpool's NDEF TLV starts at block 1) and each format's own
      // magic-byte/TLV-tag check rejects the other's data, so at most one of these two
      // calls succeeds for any real tag.
      if (pn5180ReadNfcvExtended(rawUid, data)) {
        hasExtended = true; idOut = data.databaseId;
        if (formatOut) *formatOut = "nfcvExtended";
      } else if (pn5180ReadNfcvOpenSpool(rawUid, data)) {
        hasExtended = true; idOut = data.databaseId;
        if (formatOut) *formatOut = "nfcvOpenSpool";
        if (capacityBytesOut) *capacityBytesOut = pn5180NfcvUserBytes(rawUid);
      }
      g_pn5180->reset(); g_pn5180->setupRF();
    }
  }
  if (!hasExtended) data.databaseId = idOut;  // keep the two id fields consistent either way
  return true;
}

// --- Write the databaseId to an NFC-A/NTAG tag -------------------------------
// NFC-A/NTAG only — NFC-V is deliberately never written (those tags could be
// factory-provisioned). Writes the ID as ASCII decimal, space-padded, across 3 pages
// (4-6) via NTAG WRITE (0xA2 <page> <4 bytes>), each page verified.
// Returns true = written + verified. errOut carries the reason on failure.
inline bool pn5180WriteNtagId(long id, String &errOut) {
  errOut = "";
  dbgLogf("NTAG write: start id=%ld", id);
  if (!g_pn5180Ready || !g_pn5180) { errOut = "reader not ready"; return false; }
  if (id < 0) { errOut = "invalid ID"; return false; }

  String s = String(id);
  // 10, not PN5180_ID_BYTES: the field is 12 bytes wide, but pn5180ParseIdBytes only
  // accepts up to 10 digits (the widest uint32) AND requires at least one padding byte
  // after them. A wider value would be written and verified here, then read back as -1
  // for the rest of the tag's life. Keep this bound and the parser's in step.
  if ((int)s.length() > 10) { errOut = "ID too long"; return false; }
  uint8_t idbuf[PN5180_ID_BYTES];
  for (int i = 0; i < PN5180_ID_BYTES; i++)
    idbuf[i] = (i < (int)s.length()) ? (uint8_t)s[i] : (uint8_t)' ';

  // Select the NFC-A card (CRC left on from SELECT).
  dbgLog("NTAG write: probing NFC-A...");
  PN5180ProbeResult pr;
  if (!pn5180ProbeNfcA(pr)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = "no NFC-A tag";
    dbgLog("NTAG write: no NFC-A tag found, aborting");
    return false;
  }
  dbgLogf("NTAG write: probe OK, uid=%s atqa=0x%04X sak=0x%02X", pr.uid.c_str(), pr.atqa, pr.sak);
  // NTAG WRITE (0xA2) only exists on Type-2 tags (NTAG/Ultralight). Mifare Classic
  // uses sector auth + a different command set -> reject early with a clear reason
  // instead of a cryptic low-level write/verify failure.
  if ((pr.sak & 0x20) || (pr.sak & ~0x04) != 0x00) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = String("unsupported tag (") + pn5180NfcaProduct(pr.atqa, pr.sak) +
             "): only NTAG/Ultralight can be written";
    dbgLogf("NTAG write: rejected, %s", errOut.c_str());
    return false;
  }
  PN5180 *p = g_pn5180;
  uint32_t rxStatus = 0;
  bool ok = true;

  // Write 3 pages of 4 bytes each, verifying by reading back. Each page starts with a
  // full reset+re-select of the tag: reusing the transceiver across consecutive
  // WRITE/READ frames left it stuck reporting transceiveState=3 ("wait receive") no
  // matter how often Idle/Transceive was re-armed -> a full RF reset is what reliably
  // clears it (the initial ProbeNfcA + this reset+re-select pattern are the only two
  // reset paths that have been confirmed to work).
  for (int page = 0; page < PN5180_ID_BYTES / 4; page++) {
    if (page > 0) {
      dbgLogf("NTAG write: page %d -> re-select tag", page);
      g_pn5180->reset(); g_pn5180->setupRF();
      PN5180ProbeResult pr2;
      if (!pn5180ProbeNfcA(pr2)) { ok = false; errOut = "lost tag between pages"; dbgLog("NTAG write: re-select failed"); break; }
    }
    dbgLogf("NTAG write: page %d -> write", page);
    uint8_t wr[6] = { 0xA2, (uint8_t)(PN5180_ID_START_PAGE + page),
                      idbuf[page*4+0], idbuf[page*4+1], idbuf[page*4+2], idbuf[page*4+3] };
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
    if (!pn5180SendDataRetry(p, wr, 6)) { ok = false; errOut = "write send error"; dbgLog("NTAG write: sendData(write) failed"); break; }
    dbgLog("NTAG write: sendData(write) OK, waiting for ACK");
    pn5180WaitRx(p, 15);  // NTAG ACKs WRITE with a 4-bit ACK (0x0A); a short wait suffices

    // Verify: read back the page just written.
    dbgLogf("NTAG write: page %d -> verify read", page);
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
    uint8_t rd[2] = { 0x30, (uint8_t)(PN5180_ID_START_PAGE + page) };
    if (!pn5180SendDataRetry(p, rd, 2)) { ok = false; errOut = "verify send error"; dbgLog("NTAG write: sendData(verify) failed"); break; }
    dbgLog("NTAG write: sendData(verify) OK, waiting for RX");
    if (!pn5180WaitRx(p, 15)) { ok = false; errOut = "verify timeout"; dbgLog("NTAG write: verify RX timeout"); break; }
    p->readRegister(RX_STATUS, &rxStatus);
    dbgLogf("NTAG write: RX_STATUS=0x%08X (len=%u)", rxStatus, (unsigned)(rxStatus & 0x1ff));
    if ((uint16_t)(rxStatus & 0x1ff) < 4) { ok = false; errOut = "verify too short"; dbgLog("NTAG write: verify reply too short"); break; }
    dbgLog("NTAG write: calling readData(4)...");
    uint8_t *rb = p->readData(4);
    dbgLogf("NTAG write: readData returned %s", rb ? "ptr" : "NULL");
    if (!rb) { ok = false; errOut = "verify read error"; break; }
    for (int i = 0; i < 4; i++)
      if (rb[i] != idbuf[page*4+i]) { ok = false; errOut = "verify mismatch"; break; }
    dbgLogf("NTAG write: page %d %s", page, ok ? "verified OK" : "MISMATCH");
    if (!ok) break;
  }

  dbgLogf("NTAG write: done, ok=%d err=%s", ok, errOut.c_str());
  g_pn5180->reset(); g_pn5180->setupRF();  // back to normal ISO15693 operation
  return ok;
}

// --- NTAG variant detection + generic page read/write (C.2) -------------------
// NTAG21x variants differ in user memory size, which OpenSpool/NDEF writing needs to
// know up front (how many pages are available for the NDEF message). Enum + these two
// functions are forward-declared above pn5180Probe, which also needs them (debug view's
// page-count read) but runs earlier in the file than this section.
inline int ntagUserBytes(NtagVariant v) {
  switch (v) { case NTAG_213: return 144; case NTAG_215: return 504; case NTAG_216: return 888; default: return 144; }
}
inline const char *ntagVariantName(NtagVariant v) {
  switch (v) { case NTAG_213: return "ntag213"; case NTAG_215: return "ntag215"; case NTAG_216: return "ntag216"; default: return "unknown"; }
}

// GET_VERSION (0x60): an 8-byte response, byte 6 = storage size code. Requires the
// card to already be selected. On timeout/NAK (some Ultralight variants don't support
// this command, or a marginal RF link drops it) falls back conservatively to NTAG213
// (144 B) rather than guessing bigger and overrunning the tag's actual capacity.
//
// Observed live on a real NTAG215 (debug log) that the FIRST GET_VERSION right after a
// card selection consistently timed out while later calls in the same session usually
// succeeded -- looks like the card needs a brief settle time post-select. A bare retry
// helped in the debug-poll path (which already runs pn5180ReadNtagId first, giving the
// card a few ms before this function's first attempt) but NOT reliably in the write
// path (pn5180WriteNtagOpenSpool calls this immediately after pn5180ProbeNfcA's SELECT,
// with nothing in between -- both retry attempts landed inside that same too-early
// window and both timed out). Fixed at the root instead of papering over it with more
// retries: a short settle delay before the FIRST attempt only (a failed attempt's own
// ~15 ms wait already provides that gap for any later attempts).
// Without this, a first-call timeout silently downgraded a 504 B NTAG215 to a 144 B
// budget for the rest of that write, which showed up as extra dropped fields
// (density/weights/temperatures) on a tag that had plenty of room for all of them.
inline NtagVariant pn5180NtagGetVersion() {
  PN5180 *p = g_pn5180;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt == 0) delay(2);
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
    uint8_t cmd[1] = { 0x60 };
    if (!p->sendData(cmd, 1, 0)) { dbgLogf("NTAG GET_VERSION: send failed (attempt %d)", attempt); continue; }
    if (!pn5180WaitRx(p, 15)) { dbgLogf("NTAG GET_VERSION: timeout (attempt %d)", attempt); continue; }
    uint32_t rxStatus = 0;
    p->readRegister(RX_STATUS, &rxStatus);
    uint16_t n = (uint16_t)(rxStatus & 0x1ff);
    if (n < 8) { dbgLogf("NTAG GET_VERSION: short reply (%u B, attempt %d)", n, attempt); continue; }
    uint8_t *data = p->readData(8);
    if (!data) { dbgLogf("NTAG GET_VERSION: readData failed (attempt %d)", attempt); continue; }
    dbgLogf("NTAG GET_VERSION: %02X %02X %02X %02X %02X %02X %02X %02X",
            data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
    switch (data[6]) {
      case 0x0F: return NTAG_213;
      case 0x11: return NTAG_215;
      case 0x13: return NTAG_216;
      default:   return NTAG_213;  // unrecognized code -> conservative default
    }
  }
  dbgLog("NTAG GET_VERSION: failed after retry, defaulting to 213");
  return NTAG_213;
}

// Reads `count` consecutive 4-byte pages starting at `startPage` into out (4*count
// bytes). One READ (0x30) returns 4 pages (16 B) regardless of how many were asked
// for, so this only issues ceil(count/4) transceives -- cheap compared to writes
// (which each need a full reset+re-select, see pn5180WriteNtagId's comment).
inline bool pn5180NtagReadPages(uint8_t startPage, int count, uint8_t *out) {
  PN5180 *p = g_pn5180;
  int got = 0;
  while (got < count) {
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
    uint8_t rd[2] = { 0x30, (uint8_t)(startPage + got) };
    if (!p->sendData(rd, 2, 0)) return false;
    if (!pn5180WaitRx(p, 15)) return false;
    uint32_t rxStatus = 0;
    p->readRegister(RX_STATUS, &rxStatus);
    uint16_t n = (uint16_t)(rxStatus & 0x1ff);
    if (n < 4) return false;
    uint8_t *data = p->readData(n > 16 ? 16 : n);
    if (!data) return false;
    int pagesGot = min(4, count - got);
    memcpy(out + got * 4, data, pagesGot * 4);
    got += pagesGot;
  }
  return true;
}

// Writes ONE 4-byte page (WRITE 0xA2), with a full reset+re-select first (except for
// the very first page of a run, where the caller has already just selected the tag) --
// same pattern pn5180WriteNtagId uses, required because reusing the transceiver across
// consecutive WRITE/READ frames gets stuck at transceiveState=3. No verify-read here
// (callers that need verification do their own read-back); kept minimal since this is
// called many times in a row for NDEF writes.
inline bool pn5180NtagWritePage(const String &uidHex, uint8_t page, const uint8_t data[4],
                                bool reselect, String &errOut) {
  PN5180 *p = g_pn5180;
  if (reselect) {
    g_pn5180->reset(); g_pn5180->setupRF();
    PN5180ProbeResult pr2;
    if (!pn5180ProbeNfcA(pr2) || pr2.uid != uidHex) { errOut = "lost tag while writing"; return false; }
  }
  uint8_t wr[6] = { 0xA2, page, data[0], data[1], data[2], data[3] };
  p->clearIRQStatus(0xffffffff);
  p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
  p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
  if (!pn5180SendDataRetry(p, wr, 6)) { errOut = "write send error"; return false; }
  pn5180WaitRx(p, 15);  // NTAG ACKs WRITE with a 4-bit ACK (0x0A); a short wait suffices
  return true;
}

// Parses a big-endian hex UID string (as produced by pn5180ProbeNfcA's out.uid) back
// into raw bytes. Mifare Classic 1K almost always has a 4-byte single-size UID (SAK
// 0x08 already implies this) -> only that case is supported here.
inline bool pn5180UidHexToBytes4(const String &hex, uint8_t out[4]) {
  if (hex.length() != 8) return false;
  for (int i = 0; i < 4; i++) {
    char b[3] = { hex[i*2], hex[i*2+1], 0 };
    out[i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  return true;
}

// NFC-V counterpart of the 4-byte helper above. pn5180Probe builds the ISO15693 UID
// string big-endian (byte 7 first, the usual display order), so decoding it back into
// the little-endian byte array the PN5180 library expects means reversing as we go --
// feeding a straight-order array to readSingleBlock addresses a different (nonexistent)
// tag and every block read fails.
inline bool pn5180UidHexToBytes8Reversed(const String &hex, uint8_t out[8]) {
  if (hex.length() != 16) return false;
  for (int i = 0; i < 8; i++) {
    char b[3] = { hex[i*2], hex[i*2+1], 0 };
    out[7 - i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  return true;
}

// Authenticates the sector containing `block` against the currently-selected tag's UID.
// Must be called with the tag freshly selected (right after pn5180ProbeNfcA) --
// authentication is bound to the current RF session and is lost on reset()/setupRF().
//
// key/authMode default to the factory Key A (FFFFFFFFFFFF), which is what OctoScale's
// own tags use and what every existing caller wants. Pass a different key to read a tag
// keyed by somebody else -- foreign vendor tags derive theirs from the UID, so the key
// is supplied per call rather than stored anywhere on the device.
inline bool pn5180MifareAuth(const String &uidHex, uint8_t block, String &errOut,
                             const uint8_t *key = MIFARE_KEY_A,
                             uint8_t authMode = MIFARE_AUTH_KEY_A) {
  uint8_t uid4[4];
  if (!pn5180UidHexToBytes4(uidHex, uid4)) { errOut = "unsupported UID length for Mifare auth"; return false; }
  uint8_t status = 0xFF;
  bool ok = g_pn5180->mifareAuthenticate(block, key, authMode, uid4, &status);
  // Only log FAILED auths unconditionally. A tag resting on the reader re-authenticates
  // on every 500 ms poll, so logging every success buried everything else in the ring
  // buffer within a couple of minutes -- successes are only traced when explicitly
  // asked for via /nfcauthtrace (same switch the raw SPI frame dump uses).
  if (!ok || status != 0 || g_pn5180AuthTrace)
    dbgLogf("Mifare auth: block=%u status=0x%02X ok=%d", block, status, ok);
  if (!ok || status != 0) { errOut = "auth failed (wrong key or tag rejected)"; return false; }
  return true;
}

// Reads one already-authenticated Mifare Classic block (16 bytes) into out[16].
// Card must be selected AND the sector containing `block` already authenticated in
// this RF session (see pn5180MifareAuth). Returns false on any transport/length error.
inline bool pn5180MifareReadBlock(uint8_t block, uint8_t out[16]) {
  PN5180 *p = g_pn5180;
  p->clearIRQStatus(0xffffffff);
  p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
  p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
  uint8_t rd[2] = { 0x30, block };
  if (!p->sendData(rd, 2, 0)) return false;
  if (!pn5180WaitRx(p, 15)) return false;
  uint32_t rxStatus = 0;
  p->readRegister(RX_STATUS, &rxStatus);
  uint16_t n = (uint16_t)(rxStatus & 0x1ff);
  if (n < 16) return false;
  uint8_t *data = p->readData(16);
  if (!data) return false;
  memcpy(out, data, 16);
  return true;
}

// --- Raw sector/block dump (unknown-tag fallback for the NFC debug view) -----
// Mifare Classic 1K only. Authenticates each sector in turn with the factory-default
// Key A (FFFFFFFFFFFF -- the only key OctoScale ever writes, and what most foreign
// tags/vendor tools still ship with) and reads every data block. Sector trailer
// blocks (3, 7, 11, ... every 4th block) hold the Key A/B + access bits -- never
// dumped as hex (no reason to expose key material even read-back, and it's not
// content anyway), only whether that sector authenticated.
struct MifareBlockDump {
  uint8_t block = 0;
  bool sectorAuthOk = false;
  bool readOk = false;
  uint8_t data[16] = {0};
};

// Full-image variant: 64 entries (every block including sector trailers) rather than
// the 48 data blocks above. Parameters cover what the debug dump and a foreign-tag read
// each need, so there is only one auth+read loop to maintain:
//
//   keyA/keyB   16 keys of 6 bytes each, indexed by sector; nullptr = factory Key A
//               everywhere. Key A is tried first, Key B only if Key A is rejected --
//               vendor tags vary in which one guards the data.
//   sectorMask  bit N set = dump sector N. 0xFFFF = all 16. Skipping sectors a parser
//               doesn't need is the single biggest saving available: block reads, not
//               auth, dominate the runtime (measured 765 ms with zero successful reads
//               vs. ~2150 ms reading all blocks).
//   withTrailers  keep sector-trailer blocks in the output. They hold key material, so
//               the debug view passes false; a raw consumer that computes over the full
//               1K image needs them present (zero-filled if unreadable) and passes true.
//
// Returns the number of entries written. Never aborts early: a sector whose key is
// rejected yields entries with sectorAuthOk=false and zeroed data, so a partial result
// is still usable -- some parsers only need one sector.
inline int pn5180DumpMifareClassic1kEx(const String &uidHex, MifareBlockDump *out,
                                       const uint8_t (*keyA)[6] = nullptr,
                                       const uint8_t (*keyB)[6] = nullptr,
                                       uint16_t sectorMask = 0xFFFF,
                                       bool withTrailers = false) {
  int n = 0;
  for (uint8_t sector = 0; sector < 16; sector++) {
    if (!(sectorMask & (1u << sector))) continue;
    uint8_t trailerBlock = sector * 4 + 3;
    // Authenticate against the sector's first DATA block, not its trailer. Crypto1 auth
    // is sector-wide, so either address unlocks the same sector -- but measured on a
    // real 1K tag (UID 0B06703B, factory key), a trailer-addressed auth never gets a
    // response back: every sector returned the uninitialised status 0xFF, including the
    // two that reported ok=1, so even those were transport artefacts rather than
    // successful authentications. The same key and mode against block 4 answers
    // status=0x00 on every single poll, which is why the normal read path
    // (pn5180ReadMifareExtended, block 4/8/12/16/36) always worked on the very tag whose
    // dump failed completely. That contradiction stood unexplained since 2026-08-29.
    uint8_t authBlock = sector * 4;
    String authErr;
    // Key A first, then Key B -- a rejected key costs one failed auth, and the tag
    // stays selected only as long as auth succeeds, so the retry re-selects below.
    bool authOk = pn5180MifareAuth(uidHex, authBlock, authErr,
                                   keyA ? keyA[sector] : MIFARE_KEY_A, MIFARE_AUTH_KEY_A);
    if (!authOk && keyB) {
      // A failed auth drops the card out of the selected state -- without re-selecting,
      // the Key B attempt would fail for the wrong reason and look like a bad key.
      PN5180ProbeResult reselect;
      if (pn5180ProbeNfcA(reselect) && reselect.uid == uidHex)
        authOk = pn5180MifareAuth(uidHex, authBlock, authErr,
                                  keyB[sector], MIFARE_AUTH_KEY_B);
    }
    uint8_t lastBlock = withTrailers ? trailerBlock : (uint8_t)(trailerBlock - 1);
    for (uint8_t b = sector * 4; b <= lastBlock; b++) {
      out[n].block = b;
      out[n].sectorAuthOk = authOk;
      out[n].readOk = authOk && pn5180MifareReadBlock(b, out[n].data);
      n++;
    }
  }
  return n;
}

// Debug-view dump: 48 data blocks, factory key, no trailers (they hold key material and
// aren't content). Thin wrapper so there is one auth+read implementation, not two.
inline int pn5180DumpMifareClassic1k(const String &uidHex, MifareBlockDump out[47]) {
  return pn5180DumpMifareClassic1kEx(uidHex, out);
}

// NTAG / Ultralight raw dump -- the keyless counterpart to the Mifare dump above.
// No authentication exists on these tags, so this is a plain page walk from page 0.
//
// Starting at page 0 (not at the user area, page 4) is deliberate and must not be
// "optimised" away: consumers index absolute page offsets, and a shifted dump corrupts
// their parsing silently rather than failing loudly. Pages 0-3 are UID/lock bytes and
// the Capability Container.
//
// Reads stop at the first failing page rather than erroring out -- Ultralight variants
// differ in size and the honest answer is "this is how far the tag goes". Returns the
// number of PAGES read (4 bytes each); 0 means even page 0 was unreadable.
// Requires the card to already be selected (call right after pn5180ProbeNfcA).
inline int pn5180DumpNtagPages(uint8_t *out, int maxPages) {
  int got = 0;
  // One READ returns 4 pages, so walk in blocks of 4 and keep whatever came back.
  while (got < maxPages) {
    int want = min(4, maxPages - got);
    if (!pn5180NtagReadPages((uint8_t)got, want, out + got * 4)) break;
    got += want;
  }
  return got;
}

// NFC-V (ISO15693) raw dump -- the third dump path alongside Mifare Classic and NTAG.
// No authentication exists here either (ISO15693 has no Crypto1 concept), so this is a
// plain UID-addressed block walk from block 0.
//
// Starting at block 0 is deliberate, same reasoning as the NTAG walk above: consumers
// index absolute block offsets, and a shifted dump corrupts their parsing silently.
// Block 0 is the Capability Container on an NDEF-formatted tag.
//
// Reads stop at the first failing block rather than erroring out -- ICODE variants
// differ in size and getSystemInfo's numBlocks is not always trustworthy, so the honest
// answer is "this is how far the tag actually goes". Returns the number of BLOCKS read
// (blockSize bytes each, almost always 4); 0 means even block 0 was unreadable.
// Requires a prior successful getInventory (the UID is passed in explicitly).
inline int pn5180DumpNfcvBlocks(const uint8_t uid[8], uint8_t *out, int maxBlocks) {
  int got = 0;
  while (got < maxBlocks) {
    if (!pn5180NfcvReadBlock(uid, (uint8_t)got, out + got * NFCV_BLOCK_SIZE)) break;
    got++;
  }
  return got;
}

// NTAG/Ultralight counterpart to pn5180MifareOccupancy below. These tags have no keys,
// so the Capability Container (page 3) plus the first user page carry the answer:
// an all-zero CC means the tag was never written, an NDEF CC (0xE1) with an empty
// message area likewise, and anything else is somebody's data.
//
// This exists because the normal poll path uses pn5180ReadSpool(), which never reads
// the CC -- so without this, occupancy stayed blank for every NTAG outside debug mode
// and the overwrite protection silently did not apply to them.
//
// Returns "empty", "foreign", or "" when it could not be determined (transport error).
// Requires the card to already be selected (call right after pn5180ProbeNfcA).
inline String pn5180NtagOccupancy() {
  // 8 PAGES, not 8 bytes: pages 0-7 = UID/lock (0-2), CC (3), first user pages (4-7).
  // This was uint8_t[8] and pn5180NtagReadPages wrote 32 bytes into it -- a 24-byte
  // stack overflow that tripped the canary ("Stack smashing protect failure") and
  // rebooted the device, but only sometimes: whether it crashed or silently corrupted
  // the neighbouring frame depended on what happened to sit there. Sized in bytes here
  // (pages * 4) so the unit is explicit and the two numbers cannot drift apart again.
  uint8_t pages[8 * 4];
  if (!pn5180NtagReadPages(0, 8, pages)) return "";
  const uint8_t *cc = pages + 3 * 4;
  bool alreadyNdef = false;
  if (pn5180NtagCcIsVirgin(cc, alreadyNdef)) return "empty";  // never written
  // A formatted-but-unused NDEF tag reads as an empty TLV: 0x03 0x00 (message of
  // length zero) or a plain 0x00/0xFE terminator right at the start of the user area.
  if (alreadyNdef) {
    const uint8_t *u = pages + 4 * 4;
    bool emptyMsg = (u[0] == 0x00) || (u[0] == 0xFE) || (u[0] == 0x03 && u[1] == 0x00);
    return emptyMsg ? "empty" : "foreign";
  }
  return "foreign";  // non-virgin, non-NDEF CC -> somebody else formatted this
}

// NFC-V (ISO15693) counterpart to pn5180NtagOccupancy/pn5180MifareOccupancy. Fills the
// third carrier's occupancy, which used to stay "" unconditionally: the poll path had no
// block reader when this was written, so a blank ISO15693 tag was indistinguishable from
// a vendor-written one. That gap surfaced as a real user-visible bug -- SpoolManager-
// Extended falls back to a heuristic when occupancy is empty ("no Extended data + no
// spool id + a recognised tag type" -> assume vendor tag), which fires on every blank
// NFC-V tag and warned the user about overwriting a vendor tag that was their own blank.
//
// Three-way, NOT a plain all-zero scan. An all-zero test looks correct and is wrong:
// a factory-NDEF-formatted-but-empty tag carries a CC plus a terminator TLV, so it is
// not all-zero and would be reported "foreign" -- exactly the case that was being
// misreported. Measured on a real blank ICODE (UID E00401532560D3EA): all blocks zero
// EXCEPT block 23 (283C0000) and block 27 (000000FE, the NDEF terminator). Same
// CC-driven reasoning pn5180NtagOccupancy already uses.
//
// Returns "empty", "foreign", or "" when it could not be determined (transport error).
// Requires a prior successful getInventory (the UID is passed in explicitly).
inline String pn5180NfcvOccupancy(const uint8_t uid[8]) {
  uint8_t cc[4];
  if (!pn5180NfcvReadBlock(uid, 0, cc)) return "";  // transport error -> unknown, stay silent
  bool alreadyNdef = false;
  if (pn5180NfcvCcIsVirgin(cc, alreadyNdef)) return "empty";  // all-zero CC -> never written
  // NDEF-formatted: 4-byte CC magic 0xE1, or the 8-byte form 0xE2 a foreign tag may use
  // (we only ever write the 4-byte one). Empty message = a terminator or a zero-length
  // TLV right at the start of the data area, same shapes pn5180NtagOccupancy tests for.
  if (alreadyNdef || cc[0] == 0xE2) {
    uint8_t first[4];
    if (!pn5180NfcvReadBlock(uid, 1, first)) return "";
    bool emptyMsg = (first[0] == 0x00) || (first[0] == 0xFE) ||
                    (first[0] == 0x03 && first[1] == 0x00);
    return emptyMsg ? "empty" : "foreign";
  }
  return "foreign";  // non-virgin, non-NDEF CC -> somebody else formatted this
}

// Tells a blank Mifare Classic tag apart from one carrying a foreign vendor's data,
// for the case where no format we know could parse it. Mifare has no Capability
// Container to inspect (unlike NTAG/NFC-V), so this leans on the key instead: every
// tag OctoScale writes keeps the factory-default Key A, so a sector that REJECTS that
// key was re-keyed by somebody else and must never be treated as free space. If the
// key still works, the tag is readable and genuinely empty only when its data blocks
// are all-zero -- a re-used tag with leftover bytes counts as foreign too.
//
// Returns "empty", "foreign", or "" when it could not be determined (transport error).
// Requires the card to already be selected (call right after pn5180ProbeNfcA).
inline String pn5180MifareOccupancy(const String &uidHex) {
  String authErr;
  // Sector 1 (trailer block 7) rather than sector 0: block 0 is the read-only
  // manufacturer block and sector 0 often keeps the default key even on vendor tags,
  // so it is the least informative sector to ask.
  if (!pn5180MifareAuth(uidHex, 7, authErr)) return "foreign";  // re-keyed -> not ours
  for (uint8_t b = 4; b < 7; b++) {
    uint8_t data[16];
    if (!pn5180MifareReadBlock(b, data)) return "";  // transport error -> unknown, stay silent
    for (int i = 0; i < 16; i++) if (data[i] != 0x00) return "foreign";
  }
  return "empty";
}

// --- Chip-level health/diagnostic status (NOT tag-related -- readable even with no
// tag on the reader). Pulls the handful of PN5180-internal EEPROM/register values the
// project reads at boot (product/firmware version) but never otherwise exposes, plus a
// few more that are useful for telling "reader hardware is unhappy" apart from "no tag
// present": die ID (a per-chip serial, handy for a multi-unit fleet), EEPROM layout
// version, RF field/transceiver state, and the two coarse status registers.
//
// SYSTEM_STATUS/TEMP_CONTROL are shown as raw hex + the one bit each that NXP's PN5180
// datasheet documents with certainty (system mode in bits 0-2 of SYSTEM_STATUS;
// overtemperature warning in bit 0 of TEMP_CONTROL) -- deliberately not decoding
// further undocumented bits into named flags we can't verify against real hardware.
struct PN5180ChipStatus {
  bool ok = false;                 // false = the reader wasn't ready, nothing below is valid
  uint8_t dieId[16] = {0};         // EEPROM 0x00, 16 bytes, unique per chip
  uint8_t eepromVersion[2] = {0};  // EEPROM 0x14 (major, minor)
  uint32_t rfStatus = 0;           // register 0x1d, raw
  bool rfFieldOn = false;          // RF_STATUS bit 0
  PN5180TransceiveStat transceiveState = PN5180_TS_Idle;  // RF_STATUS bits 24-26
  uint32_t systemStatus = 0;       // register 0x24, raw
  uint8_t systemMode = 0;          // SYSTEM_STATUS bits 0-2 (0=idle,1=standby,2=active/RW,3=LPCD,7=Autocoll)
  uint32_t tempControl = 0;        // register 0x25, raw
  bool overtempWarning = false;    // TEMP_CONTROL bit 0
  uint32_t irqStatus = 0;          // current IRQ_STATUS snapshot (register 0x02), raw
};

inline PN5180ChipStatus pn5180ReadChipStatus() {
  PN5180ChipStatus s;
  if (!g_pn5180Ready || !g_pn5180) return s;
  PN5180 *p = g_pn5180;
  s.ok = p->readEEprom(DIE_IDENTIFIER, s.dieId, sizeof(s.dieId))
      && p->readEEprom(EEPROM_VERSION, s.eepromVersion, sizeof(s.eepromVersion))
      && p->readRegister(RF_STATUS, &s.rfStatus)
      && p->readRegister(SYSTEM_STATUS, &s.systemStatus)
      && p->readRegister(TEMP_CONTROL, &s.tempControl);
  s.rfFieldOn = s.rfStatus & 0x01;
  s.transceiveState = p->getTransceiveState();
  s.systemMode = s.systemStatus & 0x07;
  s.overtempWarning = s.tempControl & 0x01;
  s.irqStatus = p->getIRQStatus();
  return s;
}

// --- Read the databaseId off a Mifare Classic 1K tag -------------------------
// Requires the card to already be selected (call right after pn5180ProbeNfcA).
// Authenticates sector 1 (Key A) then plain-reads block 4 (16 bytes, ID left-padded
// with spaces same as the NTAG/NFC-V layout).
inline bool pn5180ReadMifareId(const String &uidHex, long &idOut, String &textOut) {
  PN5180 *p = g_pn5180;
  idOut = -1; textOut = "";
  String authErr;
  if (!pn5180MifareAuth(uidHex, MIFARE_ID_BLOCK, authErr)) return false;

  p->clearIRQStatus(0xffffffff);
  p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
  p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
  uint8_t rd[2] = { 0x30, MIFARE_ID_BLOCK };  // READ (encrypted -- chip decrypts transparently)
  if (!p->sendData(rd, 2, 0)) return false;
  if (!pn5180WaitRx(p, 15)) return false;
  uint32_t rxStatus = 0;
  p->readRegister(RX_STATUS, &rxStatus);
  uint16_t n = (uint16_t)(rxStatus & 0x1ff);
  if (n < PN5180_ID_BYTES) return false;
  uint8_t *data = p->readData(n > 16 ? 16 : n);
  if (!data) return false;
  idOut = pn5180ParseIdBytes(data, PN5180_ID_BYTES, textOut);
  return true;
}

// Writes one 16-byte Mifare Classic block, with verify-read, and reports the reason on
// failure. The sector containing `block` must already be authenticated in this RF
// session (see pn5180MifareAuth) -- this helper does NOT authenticate itself, so
// callers writing multiple blocks in the same sector only need to authenticate once.
//
// Mifare Classic WRITE is a TWO-step protocol (unlike NTAG's single write frame):
//  1. send [0xA0, block] -> tag ACKs with a 4-bit 0x0A if it accepts the command
//  2. send the 16 data bytes -> tag ACKs again once they're actually written
// Sending command+data in one frame (the NTAG pattern) never gets step 1's ACK, so the
// tag silently ignores the write -> the read-back below then times out.
inline bool pn5180MifareWriteBlock(uint8_t block, const uint8_t data[16], String &errOut) {
  PN5180 *p = g_pn5180;
  bool ok = true;
  dbgLog("Mifare write: writing block " + String(block));
  uint8_t wr[2] = { 0xA0, block };
  p->clearIRQStatus(0xffffffff);
  p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
  p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
  uint8_t dataCopy[16];
  memcpy(dataCopy, data, 16);  // pn5180SendDataRetry wants non-const
  if (!pn5180SendDataRetry(p, wr, 2)) { ok = false; errOut = "write cmd send error"; dbgLog("Mifare write: sendData(cmd) failed"); }
  if (ok && !pn5180WaitRx(p, 15)) { ok = false; errOut = "write cmd ACK timeout"; dbgLog("Mifare write: no ACK after write command"); }
  if (ok) {
    dbgLog("Mifare write: cmd ACKed, sending 16 data bytes");
    p->clearIRQStatus(0xffffffff);
    p->writeRegisterWithAndMask(SYSTEM_CONFIG, 0xfffffff8);
    p->writeRegisterWithOrMask(SYSTEM_CONFIG, 0x00000003);
    if (!pn5180SendDataRetry(p, dataCopy, 16)) { ok = false; errOut = "write data send error"; dbgLog("Mifare write: sendData(data) failed"); }
    else if (!pn5180WaitRx(p, 15)) { ok = false; errOut = "write data ACK timeout"; dbgLog("Mifare write: no ACK after data"); }
  }

  if (ok) {
    dbgLog("Mifare write: verify read");
    uint8_t rb[16];
    if (!pn5180MifareReadBlock(block, rb)) { ok = false; errOut = "verify read error"; }
    else if (memcmp(rb, data, 16) != 0) { ok = false; errOut = "verify mismatch"; }
  }
  dbgLogf("Mifare write: block %u done, ok=%d err=%s", block, ok, errOut.c_str());
  return ok;
}

// --- Write the databaseId to a Mifare Classic 1K tag --------------------------
// One 16-byte block fits PN5180_ID_BYTES (12) with 4 bytes to spare (space-padded) --
// unlike NTAG, no multi-page split is needed. Writes+verifies block 4 (sector 1),
// factory Key A. Rejects tags whose SAK doesn't say "Mifare Classic" (see
// pn5180NfcaProduct) so a stray NTAG never takes this path by accident.
inline bool pn5180WriteMifareId(long id, String &errOut) {
  errOut = "";
  dbgLogf("Mifare write: start id=%ld", id);
  if (!g_pn5180Ready || !g_pn5180) { errOut = "reader not ready"; return false; }
  if (id < 0) { errOut = "invalid ID"; return false; }

  String s = String(id);
  // 10, not PN5180_ID_BYTES: the field is 12 bytes wide, but pn5180ParseIdBytes only
  // accepts up to 10 digits (the widest uint32) AND requires at least one padding byte
  // after them. A wider value would be written and verified here, then read back as -1
  // for the rest of the tag's life. Keep this bound and the parser's in step.
  if ((int)s.length() > 10) { errOut = "ID too long"; return false; }
  uint8_t idbuf[16];
  for (int i = 0; i < 16; i++)
    idbuf[i] = (i < (int)s.length()) ? (uint8_t)s[i] : (uint8_t)' ';

  dbgLog("Mifare write: probing NFC-A...");
  PN5180ProbeResult pr;
  if (!pn5180ProbeNfcA(pr)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = "no NFC-A tag";
    dbgLog("Mifare write: no NFC-A tag found, aborting");
    return false;
  }
  dbgLogf("Mifare write: probe OK, uid=%s atqa=0x%04X sak=0x%02X", pr.uid.c_str(), pr.atqa, pr.sak);
  // Only Mifare Classic (SAK 0x08/0x18/0x28, or the ISO14443-4 smartcard bit) takes
  // this path -- reject Type-2 tags (NTAG/Ultralight) with a clear reason instead of a
  // confusing "auth failed" from trying Crypto1 on a tag that doesn't support it.
  bool isClassic = (pr.sak == 0x08 || pr.sak == 0x18 || pr.sak == 0x09 || pr.sak == 0x28);
  if (!isClassic) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = String("unsupported tag (") + pn5180NfcaProduct(pr.atqa, pr.sak) +
             "): only Mifare Classic can be written on this path";
    dbgLogf("Mifare write: rejected, %s", errOut.c_str());
    return false;
  }

  String authErr;
  bool ok = pn5180MifareAuth(pr.uid, MIFARE_ID_BLOCK, authErr);
  if (!ok) errOut = authErr;
  else ok = pn5180MifareWriteBlock(MIFARE_ID_BLOCK, idbuf, errOut);

  dbgLogf("Mifare write: done, ok=%d err=%s", ok, errOut.c_str());
  g_pn5180->reset(); g_pn5180->setupRF();  // back to normal ISO15693 operation
  return ok;
}

// --- Mifare Classic Extended format (C.1) -------------------------------------
// Sector 1/block 4 = legacy ID anchor (unchanged, written by pn5180WriteMifareId).
// Sector 2 (blocks 8-9) = numeric fields + CRC-8. Sector 3 (blocks 12-14) = strings.
// Confirmed live (multi-sector auth spike): a Crypto1 auth for one sector survives,
// without reset()/setupRF(), long enough to authenticate + read/write a second and
// third sector in the same RF session.
//
// Block 8 layout (16 B, little-endian multi-byte fields):
//   [0]    'O'            magic byte 0
//   [1]    'S'            magic byte 1
//   [2]    format version (0x02+: block 10 holds bed temp min/max)
//   [3]    flags          bit0 = strings present (sector 3 written)
//   [4-7]  databaseId     uint32
//   [8-9]  totalWeight    uint16, grams (0xFFFF = not set)
//   [10-11] spoolWeight   uint16, grams (0xFFFF = not set)
//   [12-13] usedWeight    uint16, grams (0xFFFF = not set)
//   [14-15] density       uint16, g/cm3 * 1000 (0xFFFF = not set)
// Block 9 layout (16 B):
//   [0-1]  diameter            uint16, mm * 1000 (0xFFFF = not set)
//   [2-3]  diameterTolerance   uint16, mm * 1000 (0xFFFF = not set)
//   [4]    hotend temp         uint8, deg C (0xFF = not set)
//   [5]    bed temp            uint8, deg C (0xFF = not set)
//   [6]    enclosure temp      uint8, deg C (0xFF = not set)
//   [7]    hotend offset       int8, deg C
//   [8]    bed offset          int8, deg C
//   [9]    enclosure offset    int8, deg C
//   [10-12] color RGB          3 B (0,0,0 if unparseable/multi-color/sentinel)
//   [13]   hotend temp min     uint8, deg C (0xFF = not set) -- v2+, was "reserved"
//   [14]   hotend temp max     uint8, deg C (0xFF = not set) -- v2+, was "reserved"
//   [15]   CRC-8 (poly 0x07) over block8[0..15] + block9[0..14] + block10[0..1] -- v2+
// Block 10 layout (16 B, v2+; unused/unallocated on v1 tags):
//   [0]    bed temp min        uint8, deg C (0xFF = not set)
//   [1]    bed temp max        uint8, deg C (0xFF = not set)
//   [2-15] reserved            0x00
//
// Blocks 12-14 (sector 3): a flat 48-byte buffer of length-prefixed UTF-8 strings
// ([len][bytes]...), in order vendor, material, colorName. A field that doesn't fit is
// simply omitted (len=0) rather than truncating badly mid-run -- callers can tell what
// was dropped by comparing input vs. what pn5180ReadMifareExtended reads back.

// Minute-of-day (0..1439) <-> uint16. Deliberately NOT routed through pn5180ScaleU16:
// this value has a hard valid range, and anything outside it is a bug in the caller
// rather than a large-but-real measurement. Out-of-range is stored as "not set" instead
// of being wrapped or clamped to 1439, because a wrong time of day is worse than none.
// Two representations, deliberately: 0xFFFF is the ON-TAG "not set" (uint16 has no
// negative), -1 is the in-RAM/JSON one every other SpoolTagData number already uses.
// The conversion happens here, so /nfcprobe reports -1 like all its sibling fields
// rather than leaking the raw wire sentinel into the API.
inline uint16_t pn5180MinuteOfDayU16(int v) {
  return (v >= 0 && v <= 1439) ? (uint16_t)v : 0xFFFF;
}
inline int pn5180UnscaleMinuteOfDay(uint16_t raw) {
  return (raw <= 1439) ? (int)raw : -1;  // 0xFFFF and any junk -> not set
}

// --- v5 drying/td field codecs (shared by ALL THREE Extended carriers) -------------
// Layout agreed with SpoolManagerExtended: dryingTemperature u8 (0xFF = not set),
// dryingTime u16 LE in MINUTES (0xFFFF), td u16 LE scaled x100 (0xFFFF).
//
// dryingTime deliberately stays in MINUTES on the tag -- our whole API speaks minutes
// (OpenPrintTag spec key 58, /nfcprobe, /nfcwritespool), and every carrier here has room
// for a u16, so there is no reason to convert. That is the direct lesson from TigerTag's
// single hour-byte, where the missing division cost a factor of 60 (see the unit trap in
// lib/../HARDWARE.md). One encode/decode pair, used by all three writers and readers --
// do not open-code these at the call sites.
inline uint8_t pn5180DryTempU8(int degC) {
  return (degC >= 0 && degC <= 0xFE) ? (uint8_t)degC : 0xFF;
}
inline int pn5180UnscaleDryTemp(uint8_t raw) {
  return (raw == 0xFF) ? -1 : (int)raw;
}
inline uint16_t pn5180DryTimeU16(int minutes) {
  return (minutes >= 0 && minutes <= 0xFFFE) ? (uint16_t)minutes : 0xFFFF;
}
inline int pn5180UnscaleDryTime(uint16_t raw) {
  return (raw == 0xFFFF) ? -1 : (int)raw;
}
// td is a dimensionless opacity number (0.1 = most opaque .. 100 = most transparent),
// stored x100 so one decimal survives. Never label it mm.
inline uint16_t pn5180TdU16(float td) {
  if (td < 0) return 0xFFFF;
  long scaled = lroundf(td * 100.0f);
  return (scaled >= 0 && scaled <= 0xFFFE) ? (uint16_t)scaled : 0xFFFF;
}
inline float pn5180UnscaleTd(uint16_t raw) {
  return (raw == 0xFFFF) ? -1.0f : (float)raw / 100.0f;
}
// Little-endian u16 helpers -- the v5 fields are LE on every carrier.
inline void pn5180PutU16LE(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
inline uint16_t pn5180GetU16LE(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

inline uint16_t pn5180ScaleU16(float v, float scale, uint16_t notSet = 0xFFFF) {
  if (v < 0) return notSet;
  long scaled = lroundf(v * scale);
  if (scaled < 0 || scaled > 0xFFFE) return notSet;
  return (uint16_t)scaled;
}
inline float pn5180UnscaleU16(uint16_t v, float scale, uint16_t notSet = 0xFFFF) {
  if (v == notSet) return -1;
  return (float)v / scale;
}

// usedWeight and remainingWeight are not independent: the database keeps them so that
// used + remaining == totalWeight. Rounding each to whole grams on its own breaks that
// -- 646.6 and 353.4 both round up and the tag then claims 1001 g on a 1000 g spool,
// a total the spool never had. So round usedWeight and derive remainingWeight from it,
// which keeps the sum exact and holds each field within half a gram of the database.
// Only applies when all three values are present; with totalWeight or either component
// missing there is nothing to reconcile and both fall back to plain rounding.
inline void pn5180ScaleWeightPair(float usedWeight, float remainingWeight, float totalWeight,
                                  uint16_t &usedOut, uint16_t &remainingOut) {
  usedOut = pn5180ScaleU16(usedWeight, 1.0f);
  remainingOut = pn5180ScaleU16(remainingWeight, 1.0f);
  if (usedWeight < 0 || remainingWeight < 0 || totalWeight < 0) return;
  uint16_t totU = pn5180ScaleU16(totalWeight, 1.0f);
  if (totU == 0xFFFF || usedOut == 0xFFFF || remainingOut == 0xFFFF) return;
  if ((uint32_t)usedOut + remainingOut == totU) return;  // already consistent
  // Derive the second field rather than rounding it. Guarded against a pair that
  // doesn't belong to this total (a stale tag value paired with a fresh one): the
  // correction is only applied when it stays within one gram of what was measured.
  long derived = (long)totU - (long)usedOut;
  if (derived < 0 || derived > 0xFFFE) return;
  if (fabsf((float)derived - remainingWeight) > 1.0f) return;
  remainingOut = (uint16_t)derived;
}

// Same not-set-sentinel convention as the u16 pair above, for fields needing more than
// 16 bits of range (totalLength/usedLength, mm -- a 16-bit limit would cap filament
// length at ~65 m, too small for real spools). 3 bytes LE, 0xFFFFFF reserved as the
// sentinel (matches the u16 helpers reserving their top value, 0xFFFF/0xFFFE split).
inline uint32_t pn5180ScaleU24(long v, uint32_t notSet = 0xFFFFFF) {
  if (v < 0 || v > 0xFFFFFE) return notSet;
  return (uint32_t)v;
}
inline long pn5180UnscaleU24(uint32_t v, uint32_t notSet = 0xFFFFFF) {
  if (v == notSet) return -1;
  return (long)v;
}

// Writes the Extended payload. Requires databaseId >= 0 (block 4, the legacy anchor,
// is written by the caller via pn5180WriteMifareId BEFORE this -- kept as two steps so
// the legacy-only write path stays exactly as it was for plain /nfcwriteid callers).
// Block 8 (the magic/commit marker) is written LAST: a write aborted at any earlier
// point leaves a tag with no magic in block 8, which pn5180ReadMifareExtended below
// reads back as "legacy ID only, no Extended payload" -- never as a corrupt Extended
// record. bytesWrittenOut/droppedFieldsOut report what actually made it onto the tag
// (strings that didn't fit sector 3's 48-byte budget).
inline bool pn5180WriteMifareExtended(const String &uidHex, const SpoolTagData &d,
                                      int &bytesWrittenOut, String &droppedFieldsOut,
                                      String &errOut) {
  bytesWrittenOut = 0;
  droppedFieldsOut = "";
  errOut = "";
  if (d.databaseId < 0) { errOut = "databaseId required"; return false; }

  // --- Build sector 3 (strings) first -- its actual fit determines flags bit0. ---
  uint8_t strBuf[MIFARE_EXT_STRING_BYTES] = {0};
  int pos = 0;
  bool anyDropped = false;
  const String *strs[3] = { &d.vendor, &d.material, &d.colorName };
  const char *strNames[3] = { "vendor", "material", "colorName" };
  for (int i = 0; i < 3; i++) {
    String v = pn5180Utf8SafeTruncate(*strs[i], 255);  // len-prefix is 1 byte
    int need = 1 + v.length();
    if (v.length() == 0 || pos + need > MIFARE_EXT_STRING_BYTES) {
      if (v.length() > 0) {
        anyDropped = true;
        if (droppedFieldsOut.length()) droppedFieldsOut += ",";
        droppedFieldsOut += strNames[i];
      }
      continue;
    }
    strBuf[pos++] = (uint8_t)v.length();
    for (int c = 0; c < (int)v.length(); c++) strBuf[pos++] = (uint8_t)v[c];
  }
  bool hasStrings = (pos > 0);

  // --- Build the v3 string buffer (sectors 5-8, 192 B) -- 5 fields, PER-FIELD
  // truncation (not whole-field dropping like the v1/v2 block above): the 192 B
  // budget was sized with margin (worst case 149 B, see plan), so overflow-driven
  // dropping should never trigger in practice. What DOES matter is an individual
  // field being longer than its own cap (e.g. a 60-char displayName truncated to 48)
  // -- that's still silent data loss and must be reported the same way.
  uint8_t strBuf2[MIFARE_EXT_STRING2_BYTES] = {0};
  int pos2 = 0;
  const String *strs2[5] = { &d.code, &d.batchNumber, &d.purchasedFrom, &d.finish, &d.displayName };
  const char *strNames2[5] = { "code", "batchNumber", "purchasedFrom", "finish", "displayName" };
  const int caps2[5] = { 32, 24, 32, 8, 48 };
  for (int i = 0; i < 5; i++) {
    String v = pn5180Utf8SafeTruncate(*strs2[i], caps2[i]);
    if (v.length() > 0 && v.length() < strs2[i]->length()) {
      anyDropped = true;
      if (droppedFieldsOut.length()) droppedFieldsOut += ",";
      droppedFieldsOut += strNames2[i];
    }
    int need = 1 + v.length();
    if (pos2 + need > MIFARE_EXT_STRING2_BYTES) {
      // Budget genuinely exhausted (shouldn't happen given the margin, but stay safe
      // rather than overrun the buffer) -- omit the rest, report same as truncation.
      if (v.length() > 0) {
        anyDropped = true;
        if (droppedFieldsOut.length()) droppedFieldsOut += ",";
        droppedFieldsOut += strNames2[i];
      }
      continue;
    }
    strBuf2[pos2++] = (uint8_t)v.length();
    for (int c = 0; c < (int)v.length(); c++) strBuf2[pos2++] = (uint8_t)v[c];
  }
  bool hasStrings2 = (pos2 > 0);

  // Re-select: a caller may have just run pn5180WriteMifareId (block 4, the legacy
  // anchor) first, which ends with reset()+setupRF() and leaves the card deselected --
  // Crypto1 auth requires an actively selected card. Cheap (one anticollision round
  // trip) and correct regardless of what the caller did before this.
  PN5180ProbeResult reselectPr;
  if (!pn5180ProbeNfcA(reselectPr) || reselectPr.uid != uidHex) {
    errOut = "lost tag before Extended write";
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }

  // --- Authenticate sector 2, write blocks 9/10 (not the magic yet), block 8 last. ---
  String authErr;
  if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK8, authErr)) { errOut = authErr; return false; }

  uint8_t block9[16] = {0};
  uint16_t diaU  = pn5180ScaleU16(d.diameter, 1000.0f);
  uint16_t dtolU = pn5180ScaleU16(d.diameterTolerance, 1000.0f);
  block9[0] = (uint8_t)(diaU & 0xFF);  block9[1] = (uint8_t)(diaU >> 8);
  block9[2] = (uint8_t)(dtolU & 0xFF); block9[3] = (uint8_t)(dtolU >> 8);
  block9[4] = (d.temperature >= 0 && d.temperature <= 254) ? (uint8_t)d.temperature : 0xFF;
  block9[5] = (d.bedTemperature >= 0 && d.bedTemperature <= 254) ? (uint8_t)d.bedTemperature : 0xFF;
  block9[6] = (d.enclosureTemperature >= 0 && d.enclosureTemperature <= 254) ? (uint8_t)d.enclosureTemperature : 0xFF;
  block9[7] = (int8_t)constrain(d.offsetTemperature, -127, 127);
  block9[8] = (int8_t)constrain(d.offsetBedTemperature, -127, 127);
  block9[9] = (int8_t)constrain(d.offsetEnclosureTemperature, -127, 127);
  uint8_t rgb[3] = {0, 0, 0};
  pn5180PrimaryColorRgb(d, rgb);  // leaves rgb zeroed if unparseable -- not an error
  block9[10] = rgb[0]; block9[11] = rgb[1]; block9[12] = rgb[2];
  block9[13] = (d.temperatureMin >= 0 && d.temperatureMin <= 254) ? (uint8_t)d.temperatureMin : 0xFF;
  block9[14] = (d.temperatureMax >= 0 && d.temperatureMax <= 254) ? (uint8_t)d.temperatureMax : 0xFF;
  // block9[15] (CRC) filled in once block8 is known, below.

  uint8_t block10[16] = {0};
  block10[0] = (d.bedTemperatureMin >= 0 && d.bedTemperatureMin <= 254) ? (uint8_t)d.bedTemperatureMin : 0xFF;
  block10[1] = (d.bedTemperatureMax >= 0 && d.bedTemperatureMax <= 254) ? (uint8_t)d.bedTemperatureMax : 0xFF;
  // v4 multi-colour: colours 2 and 3 plus the flags byte go into block 10's reserve.
  // Colour 1 stays in block 9[10..12] where every previous version put it, so a v3
  // reader still finds the primary colour exactly where it expects it.
  for (int i = 0; i < 3; i++) {
    block10[2 + i] = (d.colorCount > 1) ? d.colorRgb[1][i] : 0;
    block10[5 + i] = (d.colorCount > 2) ? d.colorRgb[2][i] : 0;
  }
  block10[8] = pn5180PackColorFlags(d);

  if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK9, block9, errOut)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }
  bytesWrittenOut += 16;
  if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK10, block10, errOut)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }
  bytesWrittenOut += 16;

  if (hasStrings) {
    // Sector 3 needs its own auth (confirmed to work mid-session without reset()).
    if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK12, authErr)) {
      errOut = authErr; g_pn5180->reset(); g_pn5180->setupRF(); return false;
    }
    uint8_t b12[16], b13[16], b14[16];
    memcpy(b12, strBuf, 16);
    memcpy(b13, strBuf + 16, 16);
    memcpy(b14, strBuf + 32, 16);
    if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK12, b12, errOut) ||
        !pn5180MifareWriteBlock(MIFARE_EXT_BLOCK13, b13, errOut) ||
        !pn5180MifareWriteBlock(MIFARE_EXT_BLOCK14, b14, errOut)) {
      g_pn5180->reset(); g_pn5180->setupRF();
      return false;
    }
    bytesWrittenOut += 48;
  }

  // --- v3: sector 4 (block 16), 7 packed numeric fields. ---
  uint8_t block16[16] = {0};
  // Rounded together with usedWeight even though that one lives in block 8 further
  // down: the two have to add up to totalWeight, which independent rounding breaks.
  uint16_t remW, useW;
  pn5180ScaleWeightPair(d.usedWeight, d.remainingWeight, d.totalWeight, useW, remW);
  uint32_t totLen = pn5180ScaleU24(d.totalLength);
  uint32_t useLen = pn5180ScaleU24(d.usedLength);
  uint16_t fu = pn5180ScaleU16((float)d.firstUse, 1.0f);
  uint16_t lu = pn5180ScaleU16((float)d.lastUse, 1.0f);
  uint16_t po = pn5180ScaleU16((float)d.purchasedOn, 1.0f);
  uint16_t costU = pn5180ScaleU16(d.cost, 100.0f);
  block16[0] = (uint8_t)(remW & 0xFF);   block16[1] = (uint8_t)(remW >> 8);
  block16[2] = (uint8_t)(totLen & 0xFF); block16[3] = (uint8_t)(totLen >> 8); block16[4] = (uint8_t)(totLen >> 16);
  block16[5] = (uint8_t)(useLen & 0xFF); block16[6] = (uint8_t)(useLen >> 8); block16[7] = (uint8_t)(useLen >> 16);
  block16[8] = (uint8_t)(fu & 0xFF);     block16[9] = (uint8_t)(fu >> 8);
  block16[10] = (uint8_t)(lu & 0xFF);    block16[11] = (uint8_t)(lu >> 8);
  block16[12] = (uint8_t)(po & 0xFF);    block16[13] = (uint8_t)(po >> 8);
  block16[14] = (uint8_t)(costU & 0xFF); block16[15] = (uint8_t)(costU >> 8);
  if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK16, authErr)) {
    errOut = authErr; g_pn5180->reset(); g_pn5180->setupRF(); return false;
  }
  if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK16, block16, errOut)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }
  // Block 17, same sector -> still covered by the auth above.
  uint8_t block17[16] = {0};
  uint16_t fuM = pn5180MinuteOfDayU16(d.firstUseMinuteOfDay);
  uint16_t luM = pn5180MinuteOfDayU16(d.lastUseMinuteOfDay);
  uint16_t poM = pn5180MinuteOfDayU16(d.purchasedOnMinuteOfDay);
  block17[0] = (uint8_t)(fuM & 0xFF); block17[1] = (uint8_t)(fuM >> 8);
  block17[2] = (uint8_t)(luM & 0xFF); block17[3] = (uint8_t)(luM >> 8);
  block17[4] = (uint8_t)(poM & 0xFF); block17[5] = (uint8_t)(poM >> 8);
  // v5: drying/td in the previously unused tail. Byte 7 is a deliberate pad so both
  // u16s land 2-byte aligned and a u8 slot stays free. Bytes 12-15 stay 0x00 (the
  // buffer is zero-initialised) -- reserved, NOT 0xFF, per the agreed layout.
  block17[6] = pn5180DryTempU8(d.dryingTemperature);
  pn5180PutU16LE(&block17[8],  pn5180DryTimeU16(d.dryingTime));
  pn5180PutU16LE(&block17[10], pn5180TdU16(d.td));
  if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK17, block17, errOut)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }
  bytesWrittenOut += 16;

  // --- v3: sectors 5-8, the 192 B string buffer -- one auth per sector (Crypto1 is
  // per-sector, not global), same loop shape as the v1/v2 string block above just
  // repeated 4x. ---
  if (hasStrings2) {
    for (int sectorStart = 0; sectorStart < 12; sectorStart += 3) {
      uint8_t firstBlockOfSector = (uint8_t)MIFARE_EXT_STRING2_BLOCKS[sectorStart];
      if (!pn5180MifareAuth(uidHex, firstBlockOfSector, authErr)) {
        errOut = authErr; g_pn5180->reset(); g_pn5180->setupRF(); return false;
      }
      for (int i = sectorStart; i < sectorStart + 3; i++) {
        uint8_t blk[16];
        memcpy(blk, strBuf2 + i * 16, 16);
        if (!pn5180MifareWriteBlock((uint8_t)MIFARE_EXT_STRING2_BLOCKS[i], blk, errOut)) {
          g_pn5180->reset(); g_pn5180->setupRF();
          return false;
        }
      }
    }
    bytesWrittenOut += MIFARE_EXT_STRING2_BYTES;
  }

  // --- v3: sector 9 (block 36), commit marker for the v3 blocks -- own sub-magic so a
  // reader can distinguish "v3 fields genuinely intact" from "version byte says v3 but
  // the write died before block 36 landed" independent of block 8's own magic check. ---
  uint8_t block36[16] = {0};
  block36[0] = 'O'; block36[1] = '3';
  block36[2] = hasStrings2 ? 0x01 : 0x00;
  if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK36, authErr)) {
    errOut = authErr; g_pn5180->reset(); g_pn5180->setupRF(); return false;
  }
  if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK36, block36, errOut)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }
  bytesWrittenOut += 16;

  // Re-authenticate sector 2 (we may have moved to other sectors' auth above) to finish
  // block 8 -- the commit step.
  if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK8, authErr)) {
    errOut = authErr; g_pn5180->reset(); g_pn5180->setupRF(); return false;
  }
  uint8_t block8[16] = {0};
  block8[0] = MIFARE_EXT_MAGIC0; block8[1] = MIFARE_EXT_MAGIC1; block8[2] = MIFARE_EXT_VERSION;
  block8[3] = hasStrings ? 0x01 : 0x00;
  uint32_t dbId = (uint32_t)d.databaseId;
  block8[4] = (uint8_t)(dbId & 0xFF); block8[5] = (uint8_t)(dbId >> 8);
  block8[6] = (uint8_t)(dbId >> 16);  block8[7] = (uint8_t)(dbId >> 24);
  uint16_t totW = pn5180ScaleU16(d.totalWeight, 1.0f);
  uint16_t spoW = pn5180ScaleU16(d.spoolWeight, 1.0f);
  // useW comes from pn5180ScaleWeightPair above -- paired with remainingWeight so the
  // two add up to totalWeight.
  uint16_t denU = pn5180ScaleU16(d.density, 1000.0f);
  block8[8]  = (uint8_t)(totW & 0xFF); block8[9]  = (uint8_t)(totW >> 8);
  block8[10] = (uint8_t)(spoW & 0xFF); block8[11] = (uint8_t)(spoW >> 8);
  block8[12] = (uint8_t)(useW & 0xFF); block8[13] = (uint8_t)(useW >> 8);
  block8[14] = (uint8_t)(denU & 0xFF); block8[15] = (uint8_t)(denU >> 8);

  // CRC needs the final block8 bytes -- compute now, patch into block9, rewrite block9.
  // Covers block8[0..15] + block9[0..14] + block10[0..1] (33 B) -- v1 tags only covered
  // 31 B (no block10), but v1 readers never checked block10 anyway. v3 extends this by
  // 16 more bytes to also cover block16's numeric fields (the v3 string buffer and
  // block 36 are NOT covered -- same precedent as the v1/v2 string block, which relies
  // solely on flags bit0 as its presence indicator, no CRC; block 36's own sub-magic
  // serves that same role for the new strings).
  // v4 widens the CRC from block10[0..1] to block10[0..8] so the new colour bytes are
  // covered like every other numeric field. That shifts block16 within the buffer --
  // the reader below picks its length by version, so v1/v2/v3 tags keep their own.
  uint8_t crcBuf[56];
  memcpy(crcBuf, block8, 16);
  memcpy(crcBuf + 16, block9, 15);
  memcpy(crcBuf + 31, block10, 9);
  memcpy(crcBuf + 40, block16, 16);
  block9[15] = pn5180Crc8(crcBuf, 56);
  if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK9, block9, errOut)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }

  bool ok = pn5180MifareWriteBlock(MIFARE_EXT_BLOCK8, block8, errOut);  // commit
  if (ok) bytesWrittenOut += 16;
  droppedFieldsOut = anyDropped ? droppedFieldsOut : "";
  g_pn5180->reset(); g_pn5180->setupRF();
  return ok;
}

// Reads the Extended payload if present (block 8's magic matches); otherwise leaves
// `out` at its defaults and returns false (caller falls back to the legacy ID read).
// Requires the card to already be selected (call right after pn5180ProbeNfcA).
inline bool pn5180ReadMifareExtended(const String &uidHex, SpoolTagData &out) {
  out = SpoolTagData();
  String authErr;
  if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK8, authErr)) return false;
  uint8_t block8[16], block9[16];
  if (!pn5180MifareReadBlock(MIFARE_EXT_BLOCK8, block8)) return false;
  if (block8[0] != MIFARE_EXT_MAGIC0 || block8[1] != MIFARE_EXT_MAGIC1) return false;  // no magic -> legacy-only tag
  if (!pn5180MifareReadBlock(MIFARE_EXT_BLOCK9, block9)) return false;

  // v1 tags never wrote block 10 -- their CRC only covers 31 B (no temp-range bytes).
  // v2+ tags cover 33 B (block9[0..14] + block10[0..1]). v3+ additionally covers
  // block16's 16 B (49 B total). Reading a block a tag's own version never wrote would
  // return whatever garbage/factory data is there, so each is gated on version.
  bool isV2 = block8[2] >= 0x02;
  bool isV3 = block8[2] >= 0x03;
  bool isV4 = block8[2] >= 0x04;
  bool isV5 = block8[2] >= 0x05;
  uint8_t block10[16] = {0};
  if (isV2 && !pn5180MifareReadBlock(MIFARE_EXT_BLOCK10, block10)) return false;
  uint8_t block16[16] = {0};
  if (isV3 && !pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK16, authErr)) return false;
  if (isV3 && !pn5180MifareReadBlock(MIFARE_EXT_BLOCK16, block16)) return false;
  // Block 17 (same sector, so the auth above still covers it) holds the minute-of-day
  // companions. A tag written before these existed has it all-zero -> minute 0 =
  // midnight, which is exactly what those tags effectively carried. A read failure here
  // is NOT fatal: block 16's dates are complete on their own, so degrade to "no time of
  // day" rather than failing the whole Extended read over an optional refinement.
  uint8_t block17[16] = {0};
  bool haveB17 = isV3 && pn5180MifareReadBlock(MIFARE_EXT_BLOCK17, block17);

  // CRC coverage grew with each version and the length is what tells them apart:
  //   v1 31 B, v2 +block10[0..1] = 33, v3 +block16 = 49, v4 widens block10 to [0..8] = 56.
  // Picking the wrong length is the one way a valid tag reads back as corrupt, so this
  // mirrors the writer exactly -- including that v4 places block16 at a different
  // offset inside the buffer than v3 did.
  uint8_t crcBuf[56];
  memcpy(crcBuf, block8, 16);
  memcpy(crcBuf + 16, block9, 15);
  int crcLen = 31;
  if (isV4) {
    memcpy(crcBuf + 31, block10, 9);
    memcpy(crcBuf + 40, block16, 16);
    crcLen = 56;
  } else {
    if (isV2) { memcpy(crcBuf + 31, block10, 2); crcLen = 33; }
    if (isV3) { memcpy(crcBuf + 33, block16, 16); crcLen = 49; }
  }
  if (pn5180Crc8(crcBuf, crcLen) != block9[15]) return false;  // corrupt/partial write -> treat as absent

  out.databaseId = (long)(block8[4] | (block8[5] << 8) | (block8[6] << 16) | ((uint32_t)block8[7] << 24));
  out.totalWeight = pn5180UnscaleU16((uint16_t)(block8[8] | (block8[9] << 8)), 1.0f);
  out.spoolWeight = pn5180UnscaleU16((uint16_t)(block8[10] | (block8[11] << 8)), 1.0f);
  out.usedWeight  = pn5180UnscaleU16((uint16_t)(block8[12] | (block8[13] << 8)), 1.0f);
  out.density     = pn5180UnscaleU16((uint16_t)(block8[14] | (block8[15] << 8)), 1000.0f);
  out.diameter          = pn5180UnscaleU16((uint16_t)(block9[0] | (block9[1] << 8)), 1000.0f);
  out.diameterTolerance = pn5180UnscaleU16((uint16_t)(block9[2] | (block9[3] << 8)), 1000.0f);
  out.temperature          = (block9[4] == 0xFF) ? -1 : block9[4];
  out.bedTemperature       = (block9[5] == 0xFF) ? -1 : block9[5];
  out.enclosureTemperature = (block9[6] == 0xFF) ? -1 : block9[6];
  out.offsetTemperature          = (int8_t)block9[7];
  out.offsetBedTemperature       = (int8_t)block9[8];
  out.offsetEnclosureTemperature = (int8_t)block9[9];
  // Primary colour bytes: captured raw, decoded below once the flag byte is available.
  const uint8_t primaryRgb[3] = { block9[10], block9[11], block9[12] };
  if (isV2) {
    out.temperatureMin    = (block9[13] == 0xFF) ? -1 : block9[13];
    out.temperatureMax    = (block9[14] == 0xFF) ? -1 : block9[14];
    out.bedTemperatureMin = (block10[0] == 0xFF) ? -1 : block10[0];
    out.bedTemperatureMax = (block10[1] == 0xFF) ? -1 : block10[1];
    if (isV4) {
      for (int i = 0; i < 3; i++) {
        out.colorRgb[1][i] = block10[2 + i];
        out.colorRgb[2][i] = block10[5 + i];
      }
    }
  }
  // MUST stay outside the isV2/isV4 blocks: a pre-v4 tag has no flag byte but can still
  // carry a colour, and gating this call would drop it entirely.
  pn5180ApplyExtendedColor(out, primaryRgb, isV4, isV4 ? block10[8] : 0);
  if (isV3) {
    out.remainingWeight = pn5180UnscaleU16((uint16_t)(block16[0] | (block16[1] << 8)), 1.0f);
    out.totalLength = pn5180UnscaleU24((uint32_t)(block16[2] | (block16[3] << 8) | (block16[4] << 16)));
    out.usedLength  = pn5180UnscaleU24((uint32_t)(block16[5] | (block16[6] << 8) | (block16[7] << 16)));
    long fu = (long)pn5180UnscaleU16((uint16_t)(block16[8] | (block16[9] << 8)), 1.0f);
    long lu = (long)pn5180UnscaleU16((uint16_t)(block16[10] | (block16[11] << 8)), 1.0f);
    long po = (long)pn5180UnscaleU16((uint16_t)(block16[12] | (block16[13] << 8)), 1.0f);
    out.firstUse = fu; out.lastUse = lu; out.purchasedOn = po;
    out.cost = pn5180UnscaleU16((uint16_t)(block16[14] | (block16[15] << 8)), 100.0f);
    if (haveB17) {
      out.firstUseMinuteOfDay = pn5180UnscaleMinuteOfDay((uint16_t)(block17[0] | (block17[1] << 8)));
      out.lastUseMinuteOfDay = pn5180UnscaleMinuteOfDay((uint16_t)(block17[2] | (block17[3] << 8)));
      out.purchasedOnMinuteOfDay = pn5180UnscaleMinuteOfDay((uint16_t)(block17[4] | (block17[5] << 8)));
      // v5 only: on a v3/v4 tag these bytes are whatever the writer left there (0x00 from
      // the zero-init), and 0x00 is NOT the not-set sentinel here -- decoding them
      // unconditionally would invent a 0 °C / 0 min drying spec on every older tag and
      // overwrite the spool's real values on import. Gate strictly on the version byte.
      if (isV5) {
        out.dryingTemperature = pn5180UnscaleDryTemp(block17[6]);
        out.dryingTime = pn5180UnscaleDryTime(pn5180GetU16LE(&block17[8]));
        out.td = pn5180UnscaleTd(pn5180GetU16LE(&block17[10]));
      }
    }
  }

  bool hasStrings = (block8[3] & 0x01) != 0;
  if (hasStrings && pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK12, authErr)) {
    uint8_t strBuf[MIFARE_EXT_STRING_BYTES];
    uint8_t b12[16], b13[16], b14[16];
    if (pn5180MifareReadBlock(MIFARE_EXT_BLOCK12, b12) &&
        pn5180MifareReadBlock(MIFARE_EXT_BLOCK13, b13) &&
        pn5180MifareReadBlock(MIFARE_EXT_BLOCK14, b14)) {
      memcpy(strBuf, b12, 16); memcpy(strBuf + 16, b13, 16); memcpy(strBuf + 32, b14, 16);
      int pos = 0;
      String *targets[3] = { &out.vendor, &out.material, &out.colorName };
      for (int i = 0; i < 3 && pos < MIFARE_EXT_STRING_BYTES; i++) {
        uint8_t len = strBuf[pos++];
        if (len == 0 || pos + len > MIFARE_EXT_STRING_BYTES) break;
        char tmp[256];
        memcpy(tmp, strBuf + pos, len);
        tmp[len] = 0;
        *targets[i] = String(tmp);
        pos += len;
      }
    }
  }

  // v3 strings: gated on the block-36 commit marker's own sub-magic, independent of
  // block 8's -- a v3 tag whose numeric block16 checked out fine (CRC OK) but whose
  // string write got cut off mid-sector still degrades gracefully here (numeric v3
  // fields already populated above, strings just stay unset), same "give me whatever's
  // verifiably intact" philosophy as isV2 applied to block9 vs block10 fields.
  if (isV3 && pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK36, authErr)) {
    uint8_t block36[16];
    if (pn5180MifareReadBlock(MIFARE_EXT_BLOCK36, block36) &&
        block36[0] == 'O' && block36[1] == '3' && (block36[2] & 0x01)) {
      uint8_t strBuf2[MIFARE_EXT_STRING2_BYTES];
      bool allOk = true;
      for (int sectorStart = 0; sectorStart < 12 && allOk; sectorStart += 3) {
        uint8_t firstBlockOfSector = (uint8_t)MIFARE_EXT_STRING2_BLOCKS[sectorStart];
        if (!pn5180MifareAuth(uidHex, firstBlockOfSector, authErr)) { allOk = false; break; }
        for (int i = sectorStart; i < sectorStart + 3; i++) {
          uint8_t blk[16];
          if (!pn5180MifareReadBlock((uint8_t)MIFARE_EXT_STRING2_BLOCKS[i], blk)) { allOk = false; break; }
          memcpy(strBuf2 + i * 16, blk, 16);
        }
      }
      if (allOk) {
        int pos2 = 0;
        String *targets2[5] = { &out.code, &out.batchNumber, &out.purchasedFrom, &out.finish, &out.displayName };
        for (int i = 0; i < 5 && pos2 < MIFARE_EXT_STRING2_BYTES; i++) {
          uint8_t len = strBuf2[pos2++];
          // len==0 means THIS field is empty (the writer always emits a 1-byte
          // length prefix, even for "") -- it must not stop the loop, or every
          // field after the first empty one silently reads back empty too. Only
          // a length that would run past the buffer is the real end-of-data case.
          if (pos2 + len > MIFARE_EXT_STRING2_BYTES) break;
          if (len == 0) continue;
          char tmp[256];
          memcpy(tmp, strBuf2 + pos2, len);
          tmp[len] = 0;
          *targets2[i] = String(tmp);
          pos2 += len;
        }
      }
    }
  }
  return true;
}

// --- NTAG OpenSpool (NDEF) (C.2) -----------------------------------------------
// NDEF frame: TLV 0x03 <len> <message> 0xFE (terminator). Message = one short record:
// header 0xD2 (MB=1,ME=1,SR=1,TNF=0x02 MIME-type), type-length (1 B), payload-length
// (1 B, SR=short record), type "application/json" (16 B), then the JSON payload.
// Record header overhead = 1(header)+1(typeLen)+1(payloadLen)+16(type) = 19 B.
// TLV overhead = 1(tag)+1(len)+1(terminator) = 3 B (assuming payload+record < 255 B,
// true for anything that fits an NTAG213/215/216 anyway -- no 3-byte-length TLV path).
// Total fixed overhead = 22 B, matching the plan.
static const char NDEF_MIME_TYPE[] = "application/json";  // 16 bytes, no NUL counted

// Builds the OpenSpool JSON payload (spec fields + the os_db_id extension), applying
// progressive field-dropping from the back of the priority list if the full JSON
// doesn't fit budgetBytes. Returns the JSON string (possibly with fields dropped) and
// fills droppedOut with the names of anything left out. An empty return means even the
// mandatory core (protocol/version/type/os_db_id) didn't fit -- caller must reject the
// write rather than produce truncated/invalid JSON.
inline String pn5180BuildOpenSpoolJson(const SpoolTagData &d, int budgetBytes, String &droppedOut) {
  droppedOut = "";
  // Priority order, HIGHEST first -- dropped from the END (lowest priority) first when
  // over budget. os_db_id is the OctoScale extension (lets a re-read resolve the spool
  // directly instead of falling back to a UID lookup) and is treated as high priority.
  struct Field { const char *key; String value; bool isString; bool present; };
  // All zero-initialised: the Field[] table below builds String(buf) for EVERY entry
  // regardless of its 'present' flag (the flag only decides whether the field is later
  // emitted), so a buffer left untouched by its guarded snprintf would be read while
  // uninitialised. Sized for the widest output each format can produce ("%d" from an
  // int is up to 11 characters plus the terminator).
  char osDbId[16] = {0};
  if (d.databaseId >= 0) snprintf(osDbId, sizeof(osDbId), "%ld", d.databaseId);
  char weightBuf[16] = {0};
  if (d.totalWeight >= 0) snprintf(weightBuf, sizeof(weightBuf), "%.0f", d.totalWeight);
  char diaBuf[16] = {0};
  if (d.diameter >= 0) snprintf(diaBuf, sizeof(diaBuf), "%.2f", d.diameter);
  char hotMin[12] = {0}, hotMax[12] = {0}, bedMin[12] = {0}, bedMax[12] = {0};
  if (d.temperatureMin >= 0) snprintf(hotMin, sizeof(hotMin), "%d", d.temperatureMin);
  if (d.temperatureMax >= 0) snprintf(hotMax, sizeof(hotMax), "%d", d.temperatureMax);
  if (d.bedTemperatureMin >= 0) snprintf(bedMin, sizeof(bedMin), "%d", d.bedTemperatureMin);
  if (d.bedTemperatureMax >= 0) snprintf(bedMax, sizeof(bedMax), "%d", d.bedTemperatureMax);
  // OpenSpool's color_hex holds ONE colour, no leading '#' per its spec. Must go through
  // pn5180PrimaryColorRgb like every other writer: a plain substring(1) of d.color took
  // the grammar string apart wrongly -- "transparent:#FF0000" became the literal
  // "ransparent:#FF0000", i.e. invalid JSON content on the tag.
  String colorHex = "";
  {
    uint8_t rgb[3];
    if (pn5180PrimaryColorRgb(d, rgb)) {
      char hx[8];
      snprintf(hx, sizeof(hx), "%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
      colorHex = hx;
    }
  }

  Field fields[] = {
    { "\"protocol\"",     "\"openspool\"", false, true },
    { "\"version\"",      "\"1.0\"",       false, true },
    { "\"os_db_id\"",     String(osDbId),  false, d.databaseId >= 0 },
    { "\"type\"",         "\"" + d.material + "\"", true, d.material.length() > 0 },
    { "\"color_hex\"",    "\"" + colorHex + "\"",   true, colorHex.length() > 0 },
    { "\"brand\"",        "\"" + d.vendor + "\"",   true, d.vendor.length() > 0 },
    { "\"weight\"",       String(weightBuf), false, d.totalWeight >= 0 },
    { "\"diameter\"",     String(diaBuf),    false, d.diameter >= 0 },
    { "\"min_temp\"",     String(hotMin),    false, d.temperatureMin >= 0 },
    { "\"max_temp\"",     String(hotMax),    false, d.temperatureMax >= 0 },
    { "\"bed_min_temp\"", String(bedMin),    false, d.bedTemperatureMin >= 0 },
    { "\"bed_max_temp\"", String(bedMax),    false, d.bedTemperatureMax >= 0 },
  };
  const int n = sizeof(fields) / sizeof(fields[0]);
  bool include[n];
  for (int i = 0; i < n; i++) include[i] = fields[i].present;

  auto buildJson = [&]() {
    String out = "{";
    bool first = true;
    for (int i = 0; i < n; i++) {
      if (!include[i]) continue;
      if (!first) out += ",";
      first = false;
      out += fields[i].key; out += ":"; out += fields[i].value;
    }
    out += "}";
    return out;
  };

  String json = buildJson();
  // Drop from the back (lowest priority) until it fits, but never drop the mandatory
  // core (protocol/version/os_db_id -- indices 0,1,2).
  for (int i = n - 1; i >= 3 && (int)json.length() > budgetBytes; i--) {
    if (!include[i]) continue;
    include[i] = false;
    if (droppedOut.length()) droppedOut += ",";
    droppedOut += fields[i].key;
    json = buildJson();
  }
  if ((int)json.length() > budgetBytes) return "";  // even the mandatory core doesn't fit
  return json;
}

// Reads the Capability Container (page 3, 4 bytes) and checks it's virgin (all zero) --
// CC is OTP (bits only settable 0->1), so writing over a non-virgin CC can permanently
// disable the tag's NDEF capability. Returns true only if page 3 is entirely zero.
// If it already holds a valid-looking NDEF CC (0xE1 magic), that's reported separately
// so a second write to an already-NDEF tag can skip re-writing the CC instead of
// treating it as an error.
inline bool pn5180NtagCcIsVirgin(const uint8_t cc[4], bool &alreadyNdefOut) {
  alreadyNdefOut = (cc[0] == 0xE1);
  return cc[0] == 0 && cc[1] == 0 && cc[2] == 0 && cc[3] == 0;
}

// Writes the OpenSpool NDEF message. Length-then-message ordering: the TLV length byte
// is written as 0x00 FIRST, the actual message pages after, and the real length is
// patched in LAST -- an aborted write then reads back as an empty (harmless) NDEF
// message, never a corrupt one with a length pointing past valid data.
// CC (page 3) is written only if virgin; if it already looks like a valid NDEF CC, it's
// left untouched (second write to the same tag). Any OTHER non-virgin, non-NDEF content
// aborts immediately -- writing over it could permanently break the tag.
inline bool pn5180WriteNtagOpenSpool(const SpoolTagData &d, String &droppedFieldsOut,
                                     int &bytesWrittenOut, String &errOut) {
  errOut = ""; droppedFieldsOut = ""; bytesWrittenOut = 0;
  if (d.databaseId < 0) { errOut = "databaseId required"; return false; }
  if (!g_pn5180Ready || !g_pn5180) { errOut = "reader not ready"; return false; }

  dbgLog("NTAG OpenSpool write: probing NFC-A...");
  PN5180ProbeResult pr;
  if (!pn5180ProbeNfcA(pr)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = "no NFC-A tag"; return false;
  }
  if ((pr.sak & 0x20) || (pr.sak & ~0x04) != 0x00) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = String("unsupported tag (") + pn5180NfcaProduct(pr.atqa, pr.sak) + "): only NTAG/Ultralight support OpenSpool";
    return false;
  }

  NtagVariant variant = pn5180NtagGetVersion();
  int userBytes = ntagUserBytes(variant);
  int budget = userBytes - 22;  // 22 B fixed NDEF/record overhead (see above)
  dbgLogf("NTAG OpenSpool write: variant=%s userBytes=%d jsonBudget=%d", ntagVariantName(variant), userBytes, budget);

  String json = pn5180BuildOpenSpoolJson(d, budget, droppedFieldsOut);
  if (json.length() == 0) {
    errOut = "spool data too large even after dropping optional fields";
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }
  int payloadLen = json.length();
  int msgLen = 1 + 1 + 1 + 16 + payloadLen;  // header+typeLen+payloadLen+type+json
  if (msgLen > 255) {  // stay in the short-TLV-length path (1-byte length)
    errOut = "NDEF message too large for this tag";
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }

  // Page 3: Capability Container -- OTP, only write if virgin.
  uint8_t page3[4];
  if (!pn5180NtagReadPages(3, 1, page3)) { errOut = "CC read failed"; g_pn5180->reset(); g_pn5180->setupRF(); return false; }
  bool alreadyNdef;
  bool virgin = pn5180NtagCcIsVirgin(page3, alreadyNdef);
  if (!virgin && !alreadyNdef) {
    errOut = "tag's Capability Container is neither virgin nor a recognized NDEF CC -- refusing to write (would risk permanently damaging the tag)";
    g_pn5180->reset(); g_pn5180->setupRF();
    return false;
  }

  // Build the full page buffer: TLV(len=0 placeholder) + message, page-aligned (pad
  // with 0x00 to a 4-byte boundary; the terminator 0xFE follows the real message).
  int bodyLen = 2 + msgLen + 1;  // TLV tag+len(placeholder) + message + terminator
  int totalLen = ((bodyLen + 3) / 4) * 4;
  uint8_t *buf = (uint8_t *)calloc(totalLen, 1);
  if (!buf) { errOut = "out of memory"; g_pn5180->reset(); g_pn5180->setupRF(); return false; }
  buf[0] = 0x03; buf[1] = 0x00;  // TLV tag=NDEF message, len=0 (placeholder, patched last)
  int p = 2;
  buf[p++] = 0xD2;                          // record header: MB|ME|SR|TNF=MIME
  buf[p++] = (uint8_t)strlen(NDEF_MIME_TYPE);  // type length
  buf[p++] = (uint8_t)payloadLen;              // payload length (short record)
  memcpy(buf + p, NDEF_MIME_TYPE, strlen(NDEF_MIME_TYPE)); p += strlen(NDEF_MIME_TYPE);
  memcpy(buf + p, json.c_str(), payloadLen); p += payloadLen;
  buf[p++] = 0xFE;  // terminator TLV

  // Write CC first (if needed), THEN the message pages with length=0, THEN patch the
  // real length into page 4 last -- an interrupted write anywhere before the final
  // patch reads back as an empty NDEF message (harmless), never a corrupt one.
  bool ok = true;
  int pageIdx = 4;  // NDEF message area starts at page 4
  if (virgin) {
    uint8_t cc[4] = { 0xE1, 0x10, (uint8_t)(userBytes / 8), 0x00 };  // standard NDEF CC, size in 8-byte units
    ok = pn5180NtagWritePage(pr.uid, 3, cc, /*reselect=*/false, errOut);  // still selected from probe
    dbgLogf("NTAG OpenSpool write: CC written (was virgin), ok=%d", ok);
  }
  for (int i = 0; ok && i < totalLen / 4; i++) {
    ok = pn5180NtagWritePage(pr.uid, pageIdx + i, buf + i * 4, /*reselect=*/true, errOut);
  }
  if (ok) {
    uint8_t lenPage[4];
    if (pn5180NtagReadPages(pageIdx, 1, lenPage)) {
      lenPage[1] = (uint8_t)msgLen;  // patch the real length into the already-written page
      ok = pn5180NtagWritePage(pr.uid, pageIdx, lenPage, /*reselect=*/true, errOut);
    } else { ok = false; errOut = "length-patch read failed"; }
  }
  free(buf);
  if (ok) bytesWrittenOut = totalLen;
  dbgLogf("NTAG OpenSpool write: done ok=%d err=%s bytes=%d", ok, errOut.c_str(), bytesWrittenOut);
  g_pn5180->reset(); g_pn5180->setupRF();
  return ok;
}

// Reads back an OpenSpool NDEF tag. Only parses the fields OctoScale itself cares
// about (os_db_id primarily, plus enough to display what's on the tag) -- a full JSON
// parse via ArduinoJson, since the payload is small and this path isn't hot (once per
// tag-present event, not per poll).
inline bool pn5180ReadNtagOpenSpool(SpoolTagData &out) {
  out = SpoolTagData();
  uint8_t hdr[8];  // pages 4-5: TLV tag/len + record header/typeLen/payloadLen
  if (!pn5180NtagReadPages(4, 2, hdr)) return false;
  if (hdr[0] != 0x03) return false;  // no NDEF TLV -> not an OpenSpool tag
  uint8_t msgLen = hdr[1];
  if (msgLen == 0) return false;  // empty/aborted-write placeholder, not a real record
  if (hdr[2] != 0xD2) return false;  // not our short MIME record shape
  uint8_t typeLen = hdr[3];
  uint8_t payloadLen = hdr[4];
  if (typeLen != 16 || payloadLen == 0 || payloadLen > 220) return false;

  int totalNeeded = 2 + 3 + typeLen + payloadLen;  // TLV hdr + record hdr + type + payload
  int pagesNeeded = (totalNeeded + 3) / 4 + 1;      // +1 rounding margin
  uint8_t *buf = (uint8_t *)malloc(pagesNeeded * 4);
  if (!buf) return false;
  bool ok = pn5180NtagReadPages(4, pagesNeeded, buf);
  if (ok) {
    const char *jsonStart = (const char *)(buf + 2 + 3 + typeLen);
    JsonDocument doc;
    if (!deserializeJson(doc, jsonStart, payloadLen)) {
      if (doc["os_db_id"].is<long>() || doc["os_db_id"].is<const char*>())
        out.databaseId = doc["os_db_id"] | -1L;
      out.material = String((const char *)(doc["type"] | ""));
      out.vendor = String((const char *)(doc["brand"] | ""));
      const char *ch = doc["color_hex"] | "";
      // OpenSpool encodes "no colour" by omitting the key, so a present "000000" is
      // black -- no zero-check here. colorCount is set so colorFull carries it too, but
      // only if the hex actually parses: color_hex is arbitrary JSON and may be junk.
      if (ch && ch[0]) {
        out.color = String("#") + ch;
        if (pn5180ParseColorHex(out.color, out.colorRgb[0])) out.colorCount = 1;
      }
      float w = doc["weight"] | -1.0f; if (w > 0) out.totalWeight = w;
      float dia = doc["diameter"] | -1.0f; if (dia > 0) out.diameter = dia;
      int mn = doc["min_temp"] | -1; if (mn >= 0) out.temperatureMin = mn;
      int mx = doc["max_temp"] | -1; if (mx >= 0) out.temperatureMax = mx;
      int bn = doc["bed_min_temp"] | -1; if (bn >= 0) out.bedTemperatureMin = bn;
      int bx = doc["bed_max_temp"] | -1; if (bx >= 0) out.bedTemperatureMax = bx;
    } else {
      ok = false;
    }
  }
  free(buf);
  return ok;
}

// --- Write the databaseId to an NFC-V/ISO15693 tag ---------------------------
// Writes the ID as ASCII decimal, space-padded, across consecutive blocks starting
// at PN5180_ID_START_BLOCK, verified by reading each block back. Block size comes
// from getSystemInfo (usually 4 bytes, same layout pn5180ReadNfcvId expects).
inline bool pn5180WriteNfcvId(long id, String &errOut) {
  errOut = "";
  dbgLogf("NFC-V write: start id=%ld", id);
  if (!g_pn5180Ready || !g_pn5180) { errOut = "reader not ready"; return false; }
  if (id < 0) { errOut = "invalid ID"; return false; }

  String s = String(id);
  // 10, not PN5180_ID_BYTES: the field is 12 bytes wide, but pn5180ParseIdBytes only
  // accepts up to 10 digits (the widest uint32) AND requires at least one padding byte
  // after them. A wider value would be written and verified here, then read back as -1
  // for the rest of the tag's life. Keep this bound and the parser's in step.
  if ((int)s.length() > 10) { errOut = "ID too long"; return false; }
  uint8_t idbuf[PN5180_ID_BYTES];
  for (int i = 0; i < PN5180_ID_BYTES; i++)
    idbuf[i] = (i < (int)s.length()) ? (uint8_t)s[i] : (uint8_t)' ';

  uint8_t uid[8] = {0};
  if (g_pn5180->getInventory(uid) != ISO15693_EC_OK) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = "no NFC-V tag";
    dbgLog("NFC-V write: no NFC-V tag found, aborting");
    return false;
  }
  dbgLog("NFC-V write: tag found, writing blocks...");
  // Deliberately skip getSystemInfo here: the library parses its response without
  // checking the actual reply length against the flag bits it reads -> an
  // out-of-bounds read (and, on cheap/non-conforming tags, a possible crash) if a tag
  // replies shorter than expected. 4 bytes/block is the documented fallback and covers
  // the common ICODE-family tags this project targets.
  uint8_t blockSize = 4;
  uint8_t uidTmp[8];

  bool ok = true;
  int written = 0;
  uint8_t blk = PN5180_ID_START_BLOCK;
  while (written < PN5180_ID_BYTES) {
    uint8_t block[32] = {0};
    int n = min((int)blockSize, PN5180_ID_BYTES - written);
    memcpy(block, idbuf + written, n);
    memcpy(uidTmp, uid, 8);
    if (g_pn5180->writeSingleBlock(uidTmp, blk, block, blockSize) != ISO15693_EC_OK) {
      ok = false; errOut = "write failed (block " + String(blk) + ")"; break;
    }
    uint8_t rb[32] = {0};
    memcpy(uidTmp, uid, 8);
    if (g_pn5180->readSingleBlock(uidTmp, blk, rb, blockSize) != ISO15693_EC_OK) {
      ok = false; errOut = "verify read failed (block " + String(blk) + ")"; break;
    }
    if (memcmp(rb, block, n) != 0) { ok = false; errOut = "verify mismatch"; break; }
    dbgLogf("NFC-V write: block %u verified OK", blk);
    written += n;
    blk++;
  }

  g_pn5180->reset(); g_pn5180->setupRF();
  dbgLogf("NFC-V write: done, ok=%d err=%s", ok, errOut.c_str());
  return ok;
}

// Writes+verifies one 4-byte NFC-V block. No auth/session needed (unlike Mifare) --
// each ISO15693 block command carries the UID itself.
inline bool pn5180NfcvWriteBlock(const uint8_t uid[8], uint8_t block, const uint8_t data[4], String &errOut) {
  uint8_t uidTmp[8];
  uint8_t wr[4]; memcpy(wr, data, 4);
  memcpy(uidTmp, uid, 8);
  if (g_pn5180->writeSingleBlock(uidTmp, block, wr, NFCV_BLOCK_SIZE) != ISO15693_EC_OK) {
    errOut = "write failed (block " + String(block) + ")"; return false;
  }
  uint8_t rb[4] = {0};
  memcpy(uidTmp, uid, 8);
  if (g_pn5180->readSingleBlock(uidTmp, block, rb, NFCV_BLOCK_SIZE) != ISO15693_EC_OK) {
    errOut = "verify read failed (block " + String(block) + ")"; return false;
  }
  if (memcmp(rb, data, 4) != 0) { errOut = "verify mismatch (block " + String(block) + ")"; return false; }
  return true;
}
inline bool pn5180NfcvReadBlock(const uint8_t uid[8], uint8_t block, uint8_t out[4]) {
  uint8_t uidTmp[8]; memcpy(uidTmp, uid, 8);
  return g_pn5180->readSingleBlock(uidTmp, block, out, NFCV_BLOCK_SIZE) == ISO15693_EC_OK;
}

// --- Write the Extended payload to an NFC-V tag --------------------------------
// Same commit-marker discipline as Mifare (see NFCV_EXT_* layout comment): strings
// first, then numeric/physical blocks, then the magic block LAST so an aborted write
// never reads back as a corrupt Extended record. No sector auth needed (ISO15693 has
// no Crypto1 concept) -- every block write is independent, UID-addressed.
inline bool pn5180WriteNfcvExtended(const uint8_t uid[8], const SpoolTagData &d,
                                    int &bytesWrittenOut, String &droppedFieldsOut,
                                    String &errOut) {
  bytesWrittenOut = 0; droppedFieldsOut = ""; errOut = "";
  if (d.databaseId < 0) { errOut = "databaseId required"; return false; }

  uint8_t strBuf[NFCV_EXT_STRING_BYTES] = {0};
  int pos = 0;
  const String *strs[3] = { &d.vendor, &d.material, &d.colorName };
  const char *strNames[3] = { "vendor", "material", "colorName" };
  for (int i = 0; i < 3; i++) {
    String v = pn5180Utf8SafeTruncate(*strs[i], 255);
    int need = 1 + v.length();
    if (v.length() == 0 || pos + need > NFCV_EXT_STRING_BYTES) {
      if (v.length() > 0) {
        if (droppedFieldsOut.length()) droppedFieldsOut += ",";
        droppedFieldsOut += strNames[i];
      }
      continue;
    }
    strBuf[pos++] = (uint8_t)v.length();
    for (int c = 0; c < (int)v.length(); c++) strBuf[pos++] = (uint8_t)v[c];
  }
  bool hasStrings = (pos > 0);

  // Strings first (12 blocks, 4 B each).
  if (hasStrings) {
    for (int i = 0; i < NFCV_EXT_STRING_BYTES / 4; i++) {
      if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_STR + i, strBuf + i * 4, errOut)) return false;
    }
    bytesWrittenOut += NFCV_EXT_STRING_BYTES;
  }

  // Physical/temp block (7-10, now 16 of 16 bytes used since v2).
  uint8_t physBuf[16] = {0};
  uint16_t diaU  = pn5180ScaleU16(d.diameter, 1000.0f);
  uint16_t dtolU = pn5180ScaleU16(d.diameterTolerance, 1000.0f);
  physBuf[0] = (uint8_t)(diaU & 0xFF);  physBuf[1] = (uint8_t)(diaU >> 8);
  physBuf[2] = (uint8_t)(dtolU & 0xFF); physBuf[3] = (uint8_t)(dtolU >> 8);
  physBuf[4] = (d.temperature >= 0 && d.temperature <= 254) ? (uint8_t)d.temperature : 0xFF;
  physBuf[5] = (d.bedTemperature >= 0 && d.bedTemperature <= 254) ? (uint8_t)d.bedTemperature : 0xFF;
  physBuf[6] = (d.enclosureTemperature >= 0 && d.enclosureTemperature <= 254) ? (uint8_t)d.enclosureTemperature : 0xFF;
  physBuf[7] = (int8_t)constrain(d.offsetTemperature, -127, 127);
  physBuf[8] = (int8_t)constrain(d.offsetBedTemperature, -127, 127);
  physBuf[9] = (int8_t)constrain(d.offsetEnclosureTemperature, -127, 127);
  uint8_t rgb[3] = {0, 0, 0};
  pn5180PrimaryColorRgb(d, rgb);
  physBuf[10] = rgb[0]; physBuf[11] = rgb[1]; physBuf[12] = rgb[2];
  physBuf[13] = 0;  // CRC placeholder, patched once the numeric block is known below
  physBuf[14] = (d.temperatureMin >= 0 && d.temperatureMin <= 254) ? (uint8_t)d.temperatureMin : 0xFF;
  physBuf[15] = (d.temperatureMax >= 0 && d.temperatureMax <= 254) ? (uint8_t)d.temperatureMax : 0xFF;

  // Temp-range block (23, v2+): bed temp min/max.
  uint8_t tempRangeBuf[4] = {0};
  tempRangeBuf[0] = (d.bedTemperatureMin >= 0 && d.bedTemperatureMin <= 254) ? (uint8_t)d.bedTemperatureMin : 0xFF;
  tempRangeBuf[1] = (d.bedTemperatureMax >= 0 && d.bedTemperatureMax <= 254) ? (uint8_t)d.bedTemperatureMax : 0xFF;

  // Numeric block (5-6, 8 bytes).
  uint8_t numBuf[8] = {0};
  uint16_t totW = pn5180ScaleU16(d.totalWeight, 1.0f);
  uint16_t spoW = pn5180ScaleU16(d.spoolWeight, 1.0f);
  uint16_t useW = pn5180ScaleU16(d.usedWeight, 1.0f);
  uint16_t denU = pn5180ScaleU16(d.density, 1000.0f);
  numBuf[0] = (uint8_t)(totW & 0xFF); numBuf[1] = (uint8_t)(totW >> 8);
  numBuf[2] = (uint8_t)(spoW & 0xFF); numBuf[3] = (uint8_t)(spoW >> 8);
  numBuf[4] = (uint8_t)(useW & 0xFF); numBuf[5] = (uint8_t)(useW >> 8);
  numBuf[6] = (uint8_t)(denU & 0xFF); numBuf[7] = (uint8_t)(denU >> 8);

  // Magic block (3-4, 8 bytes) -- built last-but-written-last too (the actual write
  // order below writes numeric+physical before magic).
  uint8_t magicBuf[8] = {0};
  magicBuf[0] = NFCV_EXT_MAGIC0; magicBuf[1] = NFCV_EXT_MAGIC1; magicBuf[2] = NFCV_EXT_VERSION;
  magicBuf[3] = hasStrings ? 0x01 : 0x00;
  uint32_t dbId = (uint32_t)d.databaseId;
  magicBuf[4] = (uint8_t)(dbId & 0xFF); magicBuf[5] = (uint8_t)(dbId >> 8);
  magicBuf[6] = (uint8_t)(dbId >> 16);  magicBuf[7] = (uint8_t)(dbId >> 24);

  // CRC over magic(8) + numeric(8) + physical bytes 0-12,14-15 (everything except the
  // CRC byte itself, physBuf[13]) + tempRangeBuf[0-1] = 33 bytes (v2+; v1 covered 29).
  uint8_t crcBuf[33];
  memcpy(crcBuf, magicBuf, 8);
  memcpy(crcBuf + 8, numBuf, 8);
  memcpy(crcBuf + 16, physBuf, 13);
  memcpy(crcBuf + 29, physBuf + 14, 2);
  memcpy(crcBuf + 31, tempRangeBuf, 2);
  physBuf[13] = pn5180Crc8(crcBuf, 33);

  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_NUM, numBuf, errOut)) return false;
  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_NUM + 1, numBuf + 4, errOut)) return false;
  bytesWrittenOut += 8;

  for (int i = 0; i < 4; i++)
    if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_PHYS + i, physBuf + i * 4, errOut)) return false;
  bytesWrittenOut += 16;

  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_TEMPRANGE, tempRangeBuf, errOut)) return false;
  // v3 multi-colour, blocks 24-25. Written unconditionally, like every other field:
  // colour data is spool data, not a nice-to-have. Extended already needs blocks 0-23
  // (legacy anchor, header, 48 B of strings, temp range), so any tag that can carry
  // Extended at all has 24 blocks; only the narrow 24/25-block case would lack these
  // two, and no ISO15693 variant in circulation has that size (SLIX has 28). A tag
  // that genuinely cannot hold them fails the write with a clear block-write error
  // rather than silently dropping colours the user asked to store.
  uint8_t colBuf[4] = {0}, col2Buf[4] = {0};
  for (int i = 0; i < 3; i++) {
    colBuf[i]  = (d.colorCount > 1) ? d.colorRgb[1][i] : 0;
    col2Buf[i] = (d.colorCount > 2) ? d.colorRgb[2][i] : 0;
  }
  colBuf[3] = pn5180PackColorFlags(d);
  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_COLOR, colBuf, errOut)) return false;
  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_COLOR + 1, col2Buf, errOut)) return false;

  // v4 drying/td, blocks 26-27. Written before the magic block, like everything else,
  // so an interrupted write never leaves a tag claiming v4 without these present.
  // Reserved bytes stay 0x00 (not 0xFF, which is the not-set sentinel).
  uint8_t dryBuf[NFCV_BLOCK_SIZE] = {0};
  uint8_t dryBuf2[NFCV_BLOCK_SIZE] = {0};
  dryBuf[0] = pn5180DryTempU8(d.dryingTemperature);
  dryBuf[1] = 0;  // pad, keeps the u16 2-byte aligned
  pn5180PutU16LE(&dryBuf[2], pn5180DryTimeU16(d.dryingTime));
  pn5180PutU16LE(&dryBuf2[0], pn5180TdU16(d.td));
  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_DRY, dryBuf, errOut)) return false;
  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_DRY + 1, dryBuf2, errOut)) return false;
  bytesWrittenOut += 8;
  bytesWrittenOut += 4;

  // Magic LAST -- the commit marker.
  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_START, magicBuf, errOut)) return false;
  if (!pn5180NfcvWriteBlock(uid, NFCV_EXT_BLOCK_START + 1, magicBuf + 4, errOut)) return false;
  bytesWrittenOut += 8;

  return true;
}

// Reads the NFC-V Extended payload if present (magic matches + CRC valid); otherwise
// leaves `out` at defaults and returns false (caller falls back to legacy ID read).
inline bool pn5180ReadNfcvExtended(const uint8_t uid[8], SpoolTagData &out) {
  out = SpoolTagData();
  uint8_t magicBuf[8], numBuf[8], physBuf[16];
  if (!pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_START, magicBuf) ||
      !pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_START + 1, magicBuf + 4)) return false;
  if (magicBuf[0] != NFCV_EXT_MAGIC0 || magicBuf[1] != NFCV_EXT_MAGIC1) return false;
  if (!pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_NUM, numBuf) ||
      !pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_NUM + 1, numBuf + 4)) return false;
  for (int i = 0; i < 4; i++)
    if (!pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_PHYS + i, physBuf + i * 4)) return false;

  // v1 tags never wrote block 23 (temp range) or physBuf[14-15] -- their CRC only
  // covers 29 B. v2+ covers 33 B (physBuf[14-15] + tempRangeBuf[0-1]).
  bool isV2 = magicBuf[2] >= 0x02;
  uint8_t tempRangeBuf[4] = {0};
  if (isV2 && !pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_TEMPRANGE, tempRangeBuf)) return false;
  // v3 colour blocks -- required on a v3 tag, so a read failure fails the whole read
  // like any other block. They stay outside the CRC only because the CRC's length is
  // itself the version discriminator here (see the Mifare reader's comment); the
  // version byte already gates them, which is the same guarantee.
  uint8_t colBuf[4] = {0}, col2Buf[4] = {0};
  bool isV3nfcv = magicBuf[2] >= 0x03;
  if (isV3nfcv) {
    if (!pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_COLOR, colBuf)) return false;
    if (!pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_COLOR + 1, col2Buf)) return false;
  }
  bool haveCol = isV3nfcv;
  // v4 drying/td blocks. Same version gate: on a v3 tag blocks 26-27 were never written
  // and hold whatever the tag shipped with, so decoding them unconditionally could
  // invent a drying spec out of factory bytes.
  uint8_t dryBuf[4] = {0}, dryBuf2[4] = {0};
  bool isV4nfcv = magicBuf[2] >= 0x04;
  if (isV4nfcv) {
    if (!pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_DRY, dryBuf)) return false;
    if (!pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_DRY + 1, dryBuf2)) return false;
  }

  uint8_t crcBuf[33];
  memcpy(crcBuf, magicBuf, 8);
  memcpy(crcBuf + 8, numBuf, 8);
  memcpy(crcBuf + 16, physBuf, 13);
  int crcLen = 29;
  if (isV2) {
    memcpy(crcBuf + 29, physBuf + 14, 2);
    memcpy(crcBuf + 31, tempRangeBuf, 2);
    crcLen = 33;
  }
  if (pn5180Crc8(crcBuf, crcLen) != physBuf[13]) return false;  // corrupt/partial write -> treat as absent

  out.databaseId = (long)(magicBuf[4] | (magicBuf[5] << 8) | (magicBuf[6] << 16) | ((uint32_t)magicBuf[7] << 24));
  out.totalWeight = pn5180UnscaleU16((uint16_t)(numBuf[0] | (numBuf[1] << 8)), 1.0f);
  out.spoolWeight = pn5180UnscaleU16((uint16_t)(numBuf[2] | (numBuf[3] << 8)), 1.0f);
  out.usedWeight  = pn5180UnscaleU16((uint16_t)(numBuf[4] | (numBuf[5] << 8)), 1.0f);
  out.density     = pn5180UnscaleU16((uint16_t)(numBuf[6] | (numBuf[7] << 8)), 1000.0f);
  out.diameter          = pn5180UnscaleU16((uint16_t)(physBuf[0] | (physBuf[1] << 8)), 1000.0f);
  out.diameterTolerance = pn5180UnscaleU16((uint16_t)(physBuf[2] | (physBuf[3] << 8)), 1000.0f);
  out.temperature          = (physBuf[4] == 0xFF) ? -1 : physBuf[4];
  out.bedTemperature       = (physBuf[5] == 0xFF) ? -1 : physBuf[5];
  out.enclosureTemperature = (physBuf[6] == 0xFF) ? -1 : physBuf[6];
  out.offsetTemperature          = (int8_t)physBuf[7];
  out.offsetBedTemperature       = (int8_t)physBuf[8];
  out.offsetEnclosureTemperature = (int8_t)physBuf[9];
  // Primary colour bytes: captured raw, decoded below once the flag byte is available.
  const uint8_t primaryRgb[3] = { physBuf[10], physBuf[11], physBuf[12] };
  if (isV2) {
    out.temperatureMin    = (physBuf[14] == 0xFF) ? -1 : physBuf[14];
    out.temperatureMax    = (physBuf[15] == 0xFF) ? -1 : physBuf[15];
    out.bedTemperatureMin = (tempRangeBuf[0] == 0xFF) ? -1 : tempRangeBuf[0];
    out.bedTemperatureMax = (tempRangeBuf[1] == 0xFF) ? -1 : tempRangeBuf[1];
  }
  if (haveCol) {
    for (int i = 0; i < 3; i++) {
      out.colorRgb[1][i] = colBuf[i];
      out.colorRgb[2][i] = col2Buf[i];
    }
  }
  // MUST stay outside the haveCol block -- see the Mifare reader's note.
  pn5180ApplyExtendedColor(out, primaryRgb, haveCol, haveCol ? colBuf[3] : 0);

  if (isV4nfcv) {
    out.dryingTemperature = pn5180UnscaleDryTemp(dryBuf[0]);
    out.dryingTime = pn5180UnscaleDryTime(pn5180GetU16LE(&dryBuf[2]));
    out.td = pn5180UnscaleTd(pn5180GetU16LE(&dryBuf2[0]));
  }

  bool hasStrings = (magicBuf[3] & 0x01) != 0;
  if (hasStrings) {
    uint8_t strBuf[NFCV_EXT_STRING_BYTES];
    bool ok = true;
    for (int i = 0; i < NFCV_EXT_STRING_BYTES / 4 && ok; i++)
      ok = pn5180NfcvReadBlock(uid, NFCV_EXT_BLOCK_STR + i, strBuf + i * 4);
    if (ok) {
      int pos = 0;
      String *targets[3] = { &out.vendor, &out.material, &out.colorName };
      for (int i = 0; i < 3 && pos < NFCV_EXT_STRING_BYTES; i++) {
        uint8_t len = strBuf[pos++];
        if (len == 0 || pos + len > NFCV_EXT_STRING_BYTES) break;
        char tmp[256];
        memcpy(tmp, strBuf + pos, len);
        tmp[len] = 0;
        *targets[i] = String(tmp);
        pos += len;
      }
    }
  }
  return true;
}

// --- NTAG Extended (NTAG215/216 only) -------------------------------------------
// Alternative to openSpool for NTAG tags: OctoScale's own binary layout (full v3 field
// set), same tradeoff as nfcvExtended vs. nfcvOpenSpool -- not readable by a generic
// phone NFC app, but no JSON/NDEF text overhead eating into the byte budget. Chosen
// per-write by the caller (SpoolManagerExtended plugin setting, mirrors nfcvFormat) via
// ntagFormat="extended"/"openSpool" on pn5180WriteSpoolTag. See the NTAG_EXT_* layout
// comment above for the exact page-by-page byte layout.
//
// Capacity gate: NTAG213's ~144 B usable can't fit this layout (20 B fixed pages +
// magic/commit-marker pages + at minimum a handful of length-prefix bytes even with
// every string empty leaves well under 100 B for 8 strings) -- callers should not
// offer this format for a 213 in the first place (SpoolManagerExtended plans to gray the
// option out using capacityBytes from /nfcprobe), but this function itself also
// refuses outright on a 213 as a hard backstop, same "fail loud, not silently" stance
// as pn5180WriteMifareExtended's databaseId check.
inline bool pn5180WriteNtagExtended(const SpoolTagData &d, int &bytesWrittenOut,
                                    String &droppedFieldsOut, String &errOut) {
  errOut = ""; droppedFieldsOut = ""; bytesWrittenOut = 0;
  if (d.databaseId < 0) { errOut = "databaseId required"; return false; }
  if (!g_pn5180Ready || !g_pn5180) { errOut = "reader not ready"; return false; }

  dbgLog("NTAG Extended write: probing NFC-A...");
  PN5180ProbeResult pr;
  if (!pn5180ProbeNfcA(pr)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = "no NFC-A tag"; return false;
  }
  if ((pr.sak & 0x20) || (pr.sak & ~0x04) != 0x00) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = String("unsupported tag (") + pn5180NfcaProduct(pr.atqa, pr.sak) + "): only NTAG/Ultralight support Extended";
    return false;
  }

  NtagVariant variant = pn5180NtagGetVersion();
  if (variant == NTAG_213 || variant == NTAG_UNKNOWN) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = "tag too small for Extended (NTAG213/unknown) -- use openSpool instead";
    return false;
  }
  int userBytes = ntagUserBytes(variant);
  int strBudget = userBytes - ((NTAG_EXT_PAGE_STR - NTAG_EXT_PAGE_START) * 4) - 4;  // fixed pages + 1-page commit marker
  dbgLogf("NTAG Extended write: variant=%s userBytes=%d strBudget=%d", ntagVariantName(variant), userBytes, strBudget);

  // --- Build the string buffer first -- its actual fit determines what gets written,
  // same "strings first, decide what fits before touching the tag" order as Mifare. ---
  uint8_t *strBuf = (uint8_t *)malloc(strBudget > 0 ? strBudget : 1);
  if (!strBuf) { errOut = "out of memory"; g_pn5180->reset(); g_pn5180->setupRF(); return false; }
  int pos = 0;
  const String *strs[8] = { &d.vendor, &d.material, &d.colorName, &d.code,
                            &d.batchNumber, &d.purchasedFrom, &d.finish, &d.displayName };
  const char *strNames[8] = { "vendor", "material", "colorName", "code",
                              "batchNumber", "purchasedFrom", "finish", "displayName" };
  const int caps[8] = { 64, 64, 32, 32, 24, 32, 8, 48 };  // generous vs. Mifare -- more room here
  for (int i = 0; i < 8 && strBudget > 0; i++) {
    String v = pn5180Utf8SafeTruncate(*strs[i], caps[i]);
    if (v.length() > 0 && v.length() < strs[i]->length()) {
      if (droppedFieldsOut.length()) droppedFieldsOut += ",";
      droppedFieldsOut += strNames[i];
    }
    int need = 1 + v.length();
    if (pos + need > strBudget) {
      if (v.length() > 0) {
        if (droppedFieldsOut.length()) droppedFieldsOut += ",";
        droppedFieldsOut += strNames[i];
      }
      continue;
    }
    strBuf[pos++] = (uint8_t)v.length();
    for (int c = 0; c < (int)v.length(); c++) strBuf[pos++] = (uint8_t)v[c];
  }
  bool hasStrings = (pos > 0);
  int strPagesUsed = (pos + 3) / 4;  // round up to whole pages

  // --- Fixed numeric pages (4-18), same byte-packing conventions as Mifare's block 9/16. ---
  uint8_t fixedBuf[(NTAG_EXT_PAGE_STR - NTAG_EXT_PAGE_START) * 4] = {0};  // pages 4-18, 60 B
  auto fp = [&](int page) -> uint8_t* { return fixedBuf + (page - NTAG_EXT_PAGE_START) * 4; };

  fp(NTAG_EXT_PAGE_START)[0] = NTAG_EXT_MAGIC0;
  fp(NTAG_EXT_PAGE_START)[1] = NTAG_EXT_MAGIC1;
  fp(NTAG_EXT_PAGE_START)[2] = NTAG_EXT_VERSION;
  fp(NTAG_EXT_PAGE_START)[3] = 0;  // flags, reserved
  uint32_t dbId = (uint32_t)d.databaseId;
  fp(NTAG_EXT_PAGE_START + 1)[0] = (uint8_t)(dbId & 0xFF);
  fp(NTAG_EXT_PAGE_START + 1)[1] = (uint8_t)(dbId >> 8);
  fp(NTAG_EXT_PAGE_START + 1)[2] = (uint8_t)(dbId >> 16);
  fp(NTAG_EXT_PAGE_START + 1)[3] = (uint8_t)(dbId >> 24);

  uint16_t tw = pn5180ScaleU16(d.totalWeight, 1.0f);
  uint16_t sw = pn5180ScaleU16(d.spoolWeight, 1.0f);
  fp(NTAG_EXT_PAGE_NUM)[0] = (uint8_t)(tw & 0xFF);  fp(NTAG_EXT_PAGE_NUM)[1] = (uint8_t)(tw >> 8);
  fp(NTAG_EXT_PAGE_NUM)[2] = (uint8_t)(sw & 0xFF);  fp(NTAG_EXT_PAGE_NUM)[3] = (uint8_t)(sw >> 8);
  uint16_t uw, rw;
  pn5180ScaleWeightPair(d.usedWeight, d.remainingWeight, d.totalWeight, uw, rw);
  fp(NTAG_EXT_PAGE_NUM + 1)[0] = (uint8_t)(uw & 0xFF); fp(NTAG_EXT_PAGE_NUM + 1)[1] = (uint8_t)(uw >> 8);
  fp(NTAG_EXT_PAGE_NUM + 1)[2] = (uint8_t)(rw & 0xFF); fp(NTAG_EXT_PAGE_NUM + 1)[3] = (uint8_t)(rw >> 8);

  uint16_t den = pn5180ScaleU16(d.density, 1000.0f);
  uint16_t dia = pn5180ScaleU16(d.diameter, 1000.0f);
  fp(NTAG_EXT_PAGE_DENSITY)[0] = (uint8_t)(den & 0xFF); fp(NTAG_EXT_PAGE_DENSITY)[1] = (uint8_t)(den >> 8);
  fp(NTAG_EXT_PAGE_DENSITY)[2] = (uint8_t)(dia & 0xFF); fp(NTAG_EXT_PAGE_DENSITY)[3] = (uint8_t)(dia >> 8);

  uint16_t dtol = pn5180ScaleU16(d.diameterTolerance, 1000.0f);
  uint8_t *phys = fp(NTAG_EXT_PAGE_PHYS);
  phys[0] = (uint8_t)(dtol & 0xFF); phys[1] = (uint8_t)(dtol >> 8);
  phys[2] = (d.temperature >= 0 && d.temperature <= 254) ? (uint8_t)d.temperature : 0xFF;
  phys[3] = (d.bedTemperature >= 0 && d.bedTemperature <= 254) ? (uint8_t)d.bedTemperature : 0xFF;
  uint8_t *phys2 = fp(NTAG_EXT_PAGE_PHYS + 1);
  phys2[0] = (d.enclosureTemperature >= 0 && d.enclosureTemperature <= 254) ? (uint8_t)d.enclosureTemperature : 0xFF;
  phys2[1] = (int8_t)constrain(d.offsetTemperature, -127, 127);
  phys2[2] = (int8_t)constrain(d.offsetBedTemperature, -127, 127);
  phys2[3] = (int8_t)constrain(d.offsetEnclosureTemperature, -127, 127);
  uint8_t rgb[3] = {0, 0, 0};
  pn5180PrimaryColorRgb(d, rgb);
  uint8_t *phys3 = fp(NTAG_EXT_PAGE_PHYS + 2);
  phys3[0] = rgb[0]; phys3[1] = rgb[1]; phys3[2] = rgb[2];
  phys3[3] = (d.temperatureMax >= 0 && d.temperatureMax <= 254) ? (uint8_t)d.temperatureMax : 0xFF;

  uint8_t *tmin = fp(NTAG_EXT_PAGE_TEMPMIN);
  tmin[0] = (d.temperatureMin >= 0 && d.temperatureMin <= 254) ? (uint8_t)d.temperatureMin : 0xFF;
  tmin[1] = (d.bedTemperatureMin >= 0 && d.bedTemperatureMin <= 254) ? (uint8_t)d.bedTemperatureMin : 0xFF;
  tmin[2] = (d.bedTemperatureMax >= 0 && d.bedTemperatureMax <= 254) ? (uint8_t)d.bedTemperatureMax : 0xFF;
  tmin[3] = 0;  // reserved

  // CRC-8 over pages 4-12 (36 B, everything above), stored as page 13 byte 0.
  fp(NTAG_EXT_PAGE_CRC)[0] = pn5180Crc8(fixedBuf, NTAG_EXT_CRC_LEN);
  fp(NTAG_EXT_PAGE_CRC)[1] = 0; fp(NTAG_EXT_PAGE_CRC)[2] = 0; fp(NTAG_EXT_PAGE_CRC)[3] = 0;

  uint32_t totLen = pn5180ScaleU24(d.totalLength);
  uint32_t useLen = pn5180ScaleU24(d.usedLength);
  uint16_t costU = pn5180ScaleU16(d.cost, 100.0f);
  uint16_t fu = pn5180ScaleU16((float)d.firstUse, 1.0f);
  uint8_t *lenp = fp(NTAG_EXT_PAGE_LEN);
  lenp[0] = (uint8_t)(totLen & 0xFF); lenp[1] = (uint8_t)(totLen >> 8); lenp[2] = (uint8_t)(totLen >> 16);
  lenp[3] = (uint8_t)(useLen & 0xFF);
  uint8_t *lenp2 = fp(NTAG_EXT_PAGE_LEN + 1);
  lenp2[0] = (uint8_t)(useLen >> 8); lenp2[1] = (uint8_t)(useLen >> 16);
  lenp2[2] = (uint8_t)(costU & 0xFF); lenp2[3] = (uint8_t)(costU >> 8);
  uint8_t *lenp3 = fp(NTAG_EXT_PAGE_LEN + 2);
  // Minute-of-day companions go into bytes that were already reserved here, so the
  // string area still starts at NTAG_EXT_PAGE_STR (19) and a v1 reader sees an
  // unshifted layout -- no version bump needed, it just ignores these bytes.
  uint16_t fuM = pn5180MinuteOfDayU16(d.firstUseMinuteOfDay);
  lenp3[0] = (uint8_t)(fu & 0xFF); lenp3[1] = (uint8_t)(fu >> 8);
  lenp3[2] = (uint8_t)(fuM & 0xFF); lenp3[3] = (uint8_t)(fuM >> 8);

  uint16_t lu = pn5180ScaleU16((float)d.lastUse, 1.0f);
  uint16_t po = pn5180ScaleU16((float)d.purchasedOn, 1.0f);
  uint16_t luM = pn5180MinuteOfDayU16(d.lastUseMinuteOfDay);
  uint16_t poM = pn5180MinuteOfDayU16(d.purchasedOnMinuteOfDay);
  uint8_t *datesp = fp(NTAG_EXT_PAGE_DATES);
  datesp[0] = (uint8_t)(lu & 0xFF); datesp[1] = (uint8_t)(lu >> 8);
  datesp[2] = (uint8_t)(po & 0xFF); datesp[3] = (uint8_t)(po >> 8);
  uint8_t *datesp2 = fp(NTAG_EXT_PAGE_DATES + 1);
  // v2 multi-colour, pages 19-20. These sit INSIDE the fixed region (which grew from 15
  // to 17 pages when NTAG_EXT_PAGE_STR moved from 19 to 21), so they are written by the
  // same loop as everything else -- no separate write step, no partial-write window.
  // Colour 1 stays on page 11[0..2] where v1 put it.
  uint8_t *colp = fp(NTAG_EXT_PAGE_COLOR);
  uint8_t *colp2 = fp(NTAG_EXT_PAGE_COLOR + 1);
  for (int i = 0; i < 3; i++) {
    colp[i]  = (d.colorCount > 1) ? d.colorRgb[1][i] : 0;
    colp2[i] = (d.colorCount > 2) ? d.colorRgb[2][i] : 0;
  }
  colp[3] = pn5180PackColorFlags(d);
  colp2[3] = 0;  // reserved
  datesp2[0] = (uint8_t)(luM & 0xFF); datesp2[1] = (uint8_t)(luM >> 8);
  datesp2[2] = (uint8_t)(poM & 0xFF); datesp2[3] = (uint8_t)(poM >> 8);

  // v3 drying/td, pages 21-22. Like the colour pages above these live inside the fixed
  // region, so they ride along on the same write loop. Page 22[2..3] stays 0x00
  // (reserved, zero-initialised buffer) -- not 0xFF, which is the not-set sentinel.
  uint8_t *dryp = fp(NTAG_EXT_PAGE_DRY);
  uint8_t *dryp2 = fp(NTAG_EXT_PAGE_DRY + 1);
  dryp[0] = pn5180DryTempU8(d.dryingTemperature);
  dryp[1] = 0;  // pad, keeps the u16 below 2-byte aligned
  pn5180PutU16LE(&dryp[2], pn5180DryTimeU16(d.dryingTime));
  pn5180PutU16LE(&dryp2[0], pn5180TdU16(d.td));
  dryp2[2] = 0; dryp2[3] = 0;  // reserved

  // --- Write: fixed pages first, then strings, commit-marker page LAST -- same "an
  // aborted write never reads back as corrupt" discipline as every other Extended
  // format (a reader sees no NTAG_EXT_MAGIC2 -> treats it as "numeric fields only"). ---
  int totalFixedPages = NTAG_EXT_PAGE_STR - NTAG_EXT_PAGE_START;  // 15 pages, 4-18
  bool ok = true;
  for (int i = 0; i < totalFixedPages && ok; i++) {
    ok = pn5180NtagWritePage(pr.uid, NTAG_EXT_PAGE_START + i, fixedBuf + i * 4,
                             /*reselect=*/(i > 0), errOut);
  }
  if (!ok) { free(strBuf); g_pn5180->reset(); g_pn5180->setupRF(); return false; }
  bytesWrittenOut += totalFixedPages * 4;

  if (hasStrings) {
    int padded = strPagesUsed * 4;
    uint8_t *padBuf = (uint8_t *)calloc(padded, 1);
    if (!padBuf) { free(strBuf); errOut = "out of memory"; g_pn5180->reset(); g_pn5180->setupRF(); return false; }
    memcpy(padBuf, strBuf, pos);
    for (int i = 0; i < strPagesUsed && ok; i++) {
      ok = pn5180NtagWritePage(pr.uid, NTAG_EXT_PAGE_STR + i, padBuf + i * 4, /*reselect=*/true, errOut);
    }
    free(padBuf);
    if (!ok) { free(strBuf); g_pn5180->reset(); g_pn5180->setupRF(); return false; }
    bytesWrittenOut += padded;
  }
  free(strBuf);

  uint8_t marker[4] = { NTAG_EXT_MAGIC2_0, NTAG_EXT_MAGIC2_1, (uint8_t)(hasStrings ? 0x01 : 0x00), 0 };
  uint8_t markerPage = NTAG_EXT_PAGE_STR + (uint8_t)strPagesUsed;
  ok = pn5180NtagWritePage(pr.uid, markerPage, marker, /*reselect=*/true, errOut);
  if (ok) bytesWrittenOut += 4;
  dbgLogf("NTAG Extended write: done ok=%d err=%s bytes=%d strPages=%d", ok, errOut.c_str(), bytesWrittenOut, strPagesUsed);
  g_pn5180->reset(); g_pn5180->setupRF();
  return ok;
}

// Writes the TigerTag Standard (unsigned) layout -- see the constants block above for
// the byte-offset table and its provenance. Unlike every OctoScale-native format,
// this one has NO commit-marker discipline of its own (the SDK's discriminator is the
// magic value at offset 0, not a separate "write finished" marker) -- so the magic
// page is still written LAST here, same "an aborted write reads back as unprogrammed,
// never as corrupt" principle as everywhere else: a reader seeing zeros at offset 0
// won't recognize 0x5BF59264 and will treat the tag as blank/foreign, not as a broken
// TigerTag tag.
//
// The 6 registry IDs (material/brand/aspect/type/diameter/measureUnit) are the only
// fields this function requires -- everything else (color/measure/temps) is optional,
// same -1/"" = not-set convention as every other SpoolTagData field, written as 0 if
// unset (TigerTag has no dedicated NOT_SET sentinel documented for these bytes).
inline bool pn5180WriteNtagTigerTag(const SpoolTagData &d, int &bytesWrittenOut,
                                    String &errOut) {
  errOut = ""; bytesWrittenOut = 0;
  // tigerTagAspectId is the one optional ID of the six: SpoolManagerExtended's own payload
  // builder deliberately leaves it null/unresolved for any spool with no Finish set,
  // which field testing confirmed is the common case, not an edge case (see the
  // TigerTag write-support conversation). Written as 0 when unset -- same byte the
  // peer's spec already told us to put in the adjacent, always-unused Aspect2 slot.
  if (d.tigerTagMaterialId < 0 || d.tigerTagBrandId < 0 ||
      d.tigerTagTypeId < 0 || d.tigerTagDiameterId < 0 || d.tigerTagMeasureUnitId < 0) {
    errOut = "tigerTagMaterialId/BrandId/TypeId/DiameterId/MeasureUnitId all required (tigerTagAspectId is optional)";
    return false;
  }
  if (!g_pn5180Ready || !g_pn5180) { errOut = "reader not ready"; return false; }

  dbgLog("NTAG TigerTag write: probing NFC-A...");
  PN5180ProbeResult pr;
  if (!pn5180ProbeNfcA(pr)) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = "no NFC-A tag"; return false;
  }
  if ((pr.sak & 0x20) || (pr.sak & ~0x04) != 0x00) {
    g_pn5180->reset(); g_pn5180->setupRF();
    errOut = String("unsupported tag (") + pn5180NfcaProduct(pr.atqa, pr.sak) + "): only NTAG/Ultralight support TigerTag";
    return false;
  }

  uint8_t buf[TIGERTAG_PAYLOAD_LEN] = {0};

  // Product ID (offset 4-7): generic/no catalog link, per the peer's SDK reading.
  buf[4] = (uint8_t)(TIGERTAG_PRODUCT_ID_GENERIC >> 24);
  buf[5] = (uint8_t)(TIGERTAG_PRODUCT_ID_GENERIC >> 16);
  buf[6] = (uint8_t)(TIGERTAG_PRODUCT_ID_GENERIC >> 8);
  buf[7] = (uint8_t)(TIGERTAG_PRODUCT_ID_GENERIC);

  uint16_t matId = (uint16_t)constrain(d.tigerTagMaterialId, 0, 0xFFFF);
  buf[8] = (uint8_t)(matId >> 8); buf[9] = (uint8_t)matId;
  buf[10] = (uint8_t)constrain(d.tigerTagAspectId, 0, 0xFF);
  buf[11] = 0;  // Aspect2, unused (per peer: write 0)
  buf[12] = (uint8_t)constrain(d.tigerTagTypeId, 0, 0xFF);
  buf[13] = (uint8_t)constrain(d.tigerTagDiameterId, 0, 0xFF);
  uint16_t brandId = (uint16_t)constrain(d.tigerTagBrandId, 0, 0xFFFF);
  buf[14] = (uint8_t)(brandId >> 8); buf[15] = (uint8_t)brandId;

  uint8_t rgb[3] = {0, 0, 0};
  pn5180PrimaryColorRgb(d, rgb);  // leaves rgb zeroed if unparseable -- not an error
  buf[16] = rgb[0]; buf[17] = rgb[1]; buf[18] = rgb[2]; buf[19] = 0xFF;  // A=opaque

  // Measure (offset 20-22, u24 BE): spool's total weight/length, whichever the
  // caller's measureUnitId denotes -- the firmware has no registry knowledge of what
  // that unit means, so it just packs totalWeight verbatim (the common case: TigerTag
  // spools are typically weight-based). d.totalLength is NOT used here -- the two
  // fields are mutually exclusive per that single measure slot, and totalWeight
  // matches the ID field OctoScale's own writers already populate as a matter of
  // course, unlike totalLength which is Extended-only today.
  uint32_t measure = (d.totalWeight >= 0) ? (uint32_t)constrain(d.totalWeight, 0, 0xFFFFFE) : 0;
  buf[20] = (uint8_t)(measure >> 16); buf[21] = (uint8_t)(measure >> 8); buf[22] = (uint8_t)measure;
  buf[23] = (uint8_t)constrain(d.tigerTagMeasureUnitId, 0, 0xFF);

  uint16_t tMin = (d.temperatureMin >= 0) ? (uint16_t)constrain(d.temperatureMin, 0, 0xFFFF) : 0;
  uint16_t tMax = (d.temperatureMax >= 0) ? (uint16_t)constrain(d.temperatureMax, 0, 0xFFFF)
                : (d.temperature >= 0)    ? (uint16_t)constrain(d.temperature, 0, 0xFFFF) : 0;
  buf[24] = (uint8_t)(tMin >> 8); buf[25] = (uint8_t)tMin;
  buf[26] = (uint8_t)(tMax >> 8); buf[27] = (uint8_t)tMax;
  buf[28] = (d.dryingTemperature >= 0) ? (uint8_t)constrain(d.dryingTemperature, 0, 0xFF) : 0;
  // dryingTime crosses this boundary in MINUTES (our internal unit, matching
  // OpenPrintTag spec key 58), but TigerTag's byte 29 is HOURS -- verified against
  // TigerTag's official spec/reference SDK via the SpoolManagerExtended peer, and
  // measured on real hardware: writing 3h (=180 min) without this division put 180 in
  // byte 29, which the plugin then read back as 180 HOURS (factor-60 error on every
  // value, not just long ones). Rounded to nearest rather than truncated, so 90 min
  // becomes 2h and not 1h. As a side effect this also retires the old clamp problem:
  // in minutes a single byte capped at 255 (4h15), in hours 255h covers any real
  // drying profile.
  int dryHours = (d.dryingTime >= 0) ? (int)((d.dryingTime + 30) / 60) : -1;
  // A set-but-short time (1..29 min) would round to 0, and 0 is this layout's
  // "not set" -- the value would vanish silently on read-back. Floor it to 1h
  // instead: wrong by under half an hour, but still visibly "there is a drying
  // time", which beats losing it.
  if (d.dryingTime > 0 && dryHours == 0) dryHours = 1;
  buf[29] = (dryHours >= 0) ? (uint8_t)constrain(dryHours, 0, 0xFF) : 0;
  buf[30] = (d.bedTemperatureMin >= 0) ? (uint8_t)constrain(d.bedTemperatureMin, 0, 0xFF)
          : (d.bedTemperature >= 0)    ? (uint8_t)constrain(d.bedTemperature, 0, 0xFF) : 0;
  buf[31] = (d.bedTemperatureMax >= 0) ? (uint8_t)constrain(d.bedTemperatureMax, 0, 0xFF) : 0;

  // Magic (offset 0-3) LAST -- see function comment.
  buf[0] = (uint8_t)(TIGERTAG_MAGIC_STANDARD >> 24);
  buf[1] = (uint8_t)(TIGERTAG_MAGIC_STANDARD >> 16);
  buf[2] = (uint8_t)(TIGERTAG_MAGIC_STANDARD >> 8);
  buf[3] = (uint8_t)(TIGERTAG_MAGIC_STANDARD);

  int totalPages = TIGERTAG_PAYLOAD_LEN / 4;  // 20
  bool ok = true;
  // Write every page EXCEPT the first (holds the magic) first, then the magic page.
  for (int i = 1; i < totalPages && ok; i++) {
    ok = pn5180NtagWritePage(pr.uid, TIGERTAG_PAGE_START + i, buf + i * 4, /*reselect=*/true, errOut);
  }
  if (ok) {
    ok = pn5180NtagWritePage(pr.uid, TIGERTAG_PAGE_START, buf, /*reselect=*/true, errOut);
  }
  if (ok) bytesWrittenOut = TIGERTAG_PAYLOAD_LEN;
  dbgLogf("NTAG TigerTag write: done ok=%d err=%s bytes=%d", ok, errOut.c_str(), bytesWrittenOut);
  g_pn5180->reset(); g_pn5180->setupRF();
  return ok;
}

// Reads back a TigerTag Standard (unsigned) tag written by pn5180WriteNtagTigerTag.
// Gated purely on the magic value at offset 0 -- there is no separate commit marker
// on this foreign layout (see the writer's comment on why the magic itself is written
// last). Rejects the TigerTag+ (signed) and Init (blank) magic values: this reader
// only understands the Standard layout's field placement, and a signed tag on
// OctoScale would misdecode signature bytes as spool data.
// All multi-byte fields big-endian, matching the writer -- do not reuse
// pn5180UnscaleU16/pn5180UnscaleU24 here, their byte order is wrong for this format.
inline bool pn5180ReadNtagTigerTag(SpoolTagData &out) {
  uint8_t buf[TIGERTAG_PAYLOAD_LEN];
  if (!pn5180NtagReadPages(TIGERTAG_PAGE_START, TIGERTAG_PAYLOAD_LEN / 4, buf)) return false;

  uint32_t magic = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
  if (magic != TIGERTAG_MAGIC_STANDARD) return false;

  out.tigerTagMaterialId = (long)((buf[8] << 8) | buf[9]);
  out.tigerTagAspectId = (long)buf[10];
  // buf[11] (Aspect2) intentionally not surfaced -- we never write anything but 0
  // there and have no field to put it in; see the writer's comment.
  out.tigerTagTypeId = (long)buf[12];
  out.tigerTagDiameterId = (long)buf[13];
  out.tigerTagBrandId = (long)((buf[14] << 8) | buf[15]);

  // No gate on the bytes here, deliberately: this is a fixed foreign layout with no
  // documented NOT_SET sentinel for the colour (see the writer's own note), so 0,0,0 is
  // black -- treating it as "unset" dropped the colour of every black spool, and unlike
  // the Extended carriers there is no flag byte to consult. buf[19] is alpha, which we
  // always write as 0xFF; it is NOT usable as a presence marker either, since a foreign
  // writer may legitimately leave it at 0x00.
  out.colorRgb[0][0] = buf[16];
  out.colorRgb[0][1] = buf[17];
  out.colorRgb[0][2] = buf[18];
  out.colorCount = 1;
  {
    char hex[8]; snprintf(hex, sizeof(hex), "#%02X%02X%02X", buf[16], buf[17], buf[18]);
    out.color = String(hex);
  }

  uint32_t measure = ((uint32_t)buf[20] << 16) | ((uint32_t)buf[21] << 8) | buf[22];
  // Written into the same slot totalWeight would otherwise use (see the writer) --
  // read back as totalWeight, matching the writer's own priority when both were sent.
  out.totalWeight = (measure > 0) ? (float)measure : -1;
  out.tigerTagMeasureUnitId = (long)buf[23];

  uint16_t tMin = (buf[24] << 8) | buf[25];
  uint16_t tMax = (buf[26] << 8) | buf[27];
  out.temperatureMin = (tMin > 0) ? (int)tMin : -1;
  out.temperatureMax = (tMax > 0) ? (int)tMax : -1;
  out.dryingTemperature = (buf[28] > 0) ? (int)buf[28] : -1;
  out.dryingTime = (buf[29] > 0) ? (int)buf[29] * 60 : -1;  // byte is HOURS, our field is minutes
  out.bedTemperatureMin = (buf[30] > 0) ? (int)buf[30] : -1;
  out.bedTemperatureMax = (buf[31] > 0) ? (int)buf[31] : -1;

  return true;
}

// Reads back an NTAG Extended tag. Mirrors pn5180ReadMifareExtended's structure:
// numeric fields are read (and CRC-checked) independently of the strings, so a write
// that got the fixed pages down but was interrupted before the commit-marker page
// still yields usable numeric data on read-back, strings just stay unset.
inline bool pn5180ReadNtagExtended(SpoolTagData &out) {
  uint8_t fixedBuf[(NTAG_EXT_PAGE_STR - NTAG_EXT_PAGE_START) * 4];
  int totalFixedPages = NTAG_EXT_PAGE_STR - NTAG_EXT_PAGE_START;
  if (!pn5180NtagReadPages(NTAG_EXT_PAGE_START, totalFixedPages, fixedBuf)) return false;
  if (fixedBuf[0] != NTAG_EXT_MAGIC0 || fixedBuf[1] != NTAG_EXT_MAGIC1) return false;
  uint8_t version = fixedBuf[2];
  if (version < 1) return false;

  auto fp = [&](int page) -> uint8_t* { return fixedBuf + (page - NTAG_EXT_PAGE_START) * 4; };
  uint8_t crcPage = NTAG_EXT_PAGE_CRC - NTAG_EXT_PAGE_START;
  uint8_t storedCrc = fixedBuf[crcPage * 4];
  uint8_t calcCrc = pn5180Crc8(fixedBuf, NTAG_EXT_CRC_LEN);
  if (storedCrc != calcCrc) return false;  // corrupt/torn write -- same "all or nothing" as Mifare

  out.databaseId = (long)(fp(NTAG_EXT_PAGE_START + 1)[0] | (fp(NTAG_EXT_PAGE_START + 1)[1] << 8) |
                          (fp(NTAG_EXT_PAGE_START + 1)[2] << 16) | ((uint32_t)fp(NTAG_EXT_PAGE_START + 1)[3] << 24));

  uint16_t tw = fp(NTAG_EXT_PAGE_NUM)[0] | (fp(NTAG_EXT_PAGE_NUM)[1] << 8);
  uint16_t sw = fp(NTAG_EXT_PAGE_NUM)[2] | (fp(NTAG_EXT_PAGE_NUM)[3] << 8);
  out.totalWeight = pn5180UnscaleU16(tw, 1.0f);
  out.spoolWeight = pn5180UnscaleU16(sw, 1.0f);
  uint16_t uw = fp(NTAG_EXT_PAGE_NUM + 1)[0] | (fp(NTAG_EXT_PAGE_NUM + 1)[1] << 8);
  uint16_t rw = fp(NTAG_EXT_PAGE_NUM + 1)[2] | (fp(NTAG_EXT_PAGE_NUM + 1)[3] << 8);
  out.usedWeight = pn5180UnscaleU16(uw, 1.0f);
  out.remainingWeight = pn5180UnscaleU16(rw, 1.0f);

  uint16_t den = fp(NTAG_EXT_PAGE_DENSITY)[0] | (fp(NTAG_EXT_PAGE_DENSITY)[1] << 8);
  uint16_t dia = fp(NTAG_EXT_PAGE_DENSITY)[2] | (fp(NTAG_EXT_PAGE_DENSITY)[3] << 8);
  out.density = pn5180UnscaleU16(den, 1000.0f);
  out.diameter = pn5180UnscaleU16(dia, 1000.0f);

  uint8_t *phys = fp(NTAG_EXT_PAGE_PHYS);
  uint16_t dtol = phys[0] | (phys[1] << 8);
  out.diameterTolerance = pn5180UnscaleU16(dtol, 1000.0f);
  out.temperature = (phys[2] == 0xFF) ? -1 : phys[2];
  out.bedTemperature = (phys[3] == 0xFF) ? -1 : phys[3];
  uint8_t *phys2 = fp(NTAG_EXT_PAGE_PHYS + 1);
  out.enclosureTemperature = (phys2[0] == 0xFF) ? -1 : phys2[0];
  out.offsetTemperature = (int8_t)phys2[1];
  out.offsetBedTemperature = (int8_t)phys2[2];
  out.offsetEnclosureTemperature = (int8_t)phys2[3];
  uint8_t *phys3 = fp(NTAG_EXT_PAGE_PHYS + 2);
  // Primary colour bytes: captured raw, decoded below once the flag byte is available.
  const uint8_t primaryRgb[3] = { phys3[0], phys3[1], phys3[2] };
  out.temperatureMax = (phys3[3] == 0xFF) ? -1 : phys3[3];

  uint8_t *tmin = fp(NTAG_EXT_PAGE_TEMPMIN);
  out.temperatureMin = (tmin[0] == 0xFF) ? -1 : tmin[0];
  out.bedTemperatureMin = (tmin[1] == 0xFF) ? -1 : tmin[1];
  out.bedTemperatureMax = (tmin[2] == 0xFF) ? -1 : tmin[2];

  uint8_t *lenp = fp(NTAG_EXT_PAGE_LEN);
  uint8_t *lenp2 = fp(NTAG_EXT_PAGE_LEN + 1);
  uint32_t totLen = lenp[0] | (lenp[1] << 8) | (lenp[2] << 16);
  uint32_t useLen = lenp[3] | (lenp2[0] << 8) | (lenp2[1] << 16);
  out.totalLength = pn5180UnscaleU24(totLen);
  out.usedLength = pn5180UnscaleU24(useLen);
  uint16_t costU = lenp2[2] | (lenp2[3] << 8);
  out.cost = pn5180UnscaleU16(costU, 100.0f);
  uint8_t *lenp3 = fp(NTAG_EXT_PAGE_LEN + 2);
  uint16_t fu = lenp3[0] | (lenp3[1] << 8);
  out.firstUse = pn5180UnscaleU16(fu, 1.0f);

  uint8_t *datesp = fp(NTAG_EXT_PAGE_DATES);
  uint16_t lu = datesp[0] | (datesp[1] << 8);
  uint16_t po = datesp[2] | (datesp[3] << 8);
  out.lastUse = pn5180UnscaleU16(lu, 1.0f);
  out.purchasedOn = pn5180UnscaleU16(po, 1.0f);

  // Minute-of-day companions. On a tag written before these existed the bytes are 0x00,
  // which decodes to minute 0 = midnight -- i.e. exactly the behaviour those tags had
  // when the time was truncated on write, so an old tag reads the same as it always did.
  out.firstUseMinuteOfDay = pn5180UnscaleMinuteOfDay(lenp3[2] | (lenp3[3] << 8));
  uint8_t *datesp2 = fp(NTAG_EXT_PAGE_DATES + 1);
  out.lastUseMinuteOfDay = pn5180UnscaleMinuteOfDay(datesp2[0] | (datesp2[1] << 8));
  out.purchasedOnMinuteOfDay = pn5180UnscaleMinuteOfDay(datesp2[2] | (datesp2[3] << 8));
  // v2 multi-colour. Version-gated: on a v1 tag pages 19-20 are the first two STRING
  // pages, so reading colours from them would produce garbage RGB from UTF-8 bytes.
  bool haveColorFlags = (fixedBuf[2] >= 0x02);   // fixedBuf[2] = the version byte on page 4
  uint8_t colorFlagByte = 0;
  if (haveColorFlags) {
    uint8_t *colp = fp(NTAG_EXT_PAGE_COLOR);
    uint8_t *colp2 = fp(NTAG_EXT_PAGE_COLOR + 1);
    colorFlagByte = colp[3];
    for (int i = 0; i < 3; i++) {
      out.colorRgb[1][i] = colp[i];
      out.colorRgb[2][i] = colp2[i];
    }
  }
  // MUST stay outside the version block -- see the Mifare reader's note.
  pn5180ApplyExtendedColor(out, primaryRgb, haveColorFlags, colorFlagByte);

  // v3 drying/td, pages 21-22. Version-gated for the same reason as the colours above,
  // only sharper: on a v2 tag those pages are the first two STRING pages, so decoding
  // them unconditionally would turn UTF-8 vendor bytes into a drying temperature. On a
  // v1 tag they are string pages too -- only a v3 tag has real values there.
  if (fixedBuf[2] >= 0x03) {
    uint8_t *dryp = fp(NTAG_EXT_PAGE_DRY);
    uint8_t *dryp2 = fp(NTAG_EXT_PAGE_DRY + 1);
    out.dryingTemperature = pn5180UnscaleDryTemp(dryp[0]);
    out.dryingTime = pn5180UnscaleDryTime(pn5180GetU16LE(&dryp[2]));
    out.td = pn5180UnscaleTd(pn5180GetU16LE(&dryp2[0]));
  }

  // Strings: gated on the commit-marker page's own sub-magic, same reasoning as
  // Mifare's block 36 -- numeric fields above are already valid (CRC checked) even if
  // the string write never landed. Scan forward from NTAG_EXT_PAGE_STR for the marker:
  // its exact page number depends on how many string pages were written, so it isn't a
  // fixed constant -- read pages incrementally until 2 zero-length-looking string
  // slots are exhausted or a plausible marker ('N','X') is found. To keep this simple
  // and bounded, read up to a generous max (NTAG216's full capacity) and locate the
  // marker by scanning for the sub-magic bytes rather than computing its position.
  int maxStrPages = (888 - (NTAG_EXT_PAGE_STR - NTAG_EXT_PAGE_START) * 4) / 4 + 2;
  uint8_t *scanBuf = (uint8_t *)malloc(maxStrPages * 4);
  if (!scanBuf) return true;  // numeric fields already populated -- degrade gracefully
  bool gotAny = false;
  // Read in chunks (pn5180NtagReadPages handles >4 pages internally via multiple READs)
  // -- stop early once the marker is found to avoid reading past tag capacity.
  for (int chunk = 0; chunk < maxStrPages; chunk += 4) {
    int want = min(4, maxStrPages - chunk);
    if (!pn5180NtagReadPages(NTAG_EXT_PAGE_STR + chunk, want, scanBuf + chunk * 4)) break;
    gotAny = true;
    // Scan the newly-read chunk for the marker.
    for (int i = 0; i < want; i++) {
      uint8_t *pg = scanBuf + (chunk + i) * 4;
      if (pg[0] == NTAG_EXT_MAGIC2_0 && pg[1] == NTAG_EXT_MAGIC2_1) {
        if (pg[2] & 0x01) {
          int strBytes = (chunk + i) * 4;  // bytes before the marker page = the string buffer
          int pos = 0;
          String *targets[8] = { &out.vendor, &out.material, &out.colorName, &out.code,
                                 &out.batchNumber, &out.purchasedFrom, &out.finish, &out.displayName };
          for (int f = 0; f < 8 && pos < strBytes; f++) {
            uint8_t len = scanBuf[pos++];
            if (pos + len > strBytes) break;
            if (len == 0) continue;
            char tmp[256];
            memcpy(tmp, scanBuf + pos, len);
            tmp[len] = 0;
            *targets[f] = String(tmp);
            pos += len;
          }
        }
        free(scanBuf);
        return true;
      }
    }
    if (!gotAny) break;
  }
  free(scanBuf);
  return true;  // no marker found within the scan window -- numeric-only read, not an error
}

// --- NFC-V/ISO15693 OpenSpool (NFC Forum Type 5 Tag NDEF) -----------------------
// Alternative to nfcvExtended for NFC-V tags: standard Type 5 NDEF instead of
// OctoScale's own binary layout, so a phone (Android/iOS, both read T5T natively) can
// read the spool with a generic NFC app, same tradeoff as openSpool on NTAG. Chosen
// per-write by the caller (SpoolManagerExtended plugin setting, see pn5180WriteSpoolTag's
// nfcvFormat param) -- NOT auto-detected from tag capacity, since either format fits
// on any tag this project targets; it's purely a compatibility preference.
//
// Layout: block 0 = Capability Container (4 B, NFC Forum Type 5 Tag Operation spec),
// blocks 1+ = NDEF TLV (0x03 <len> <message> 0xFE), same MIME/application-json record
// shape as the NTAG openSpool format (pn5180BuildOpenSpoolJson is shared as-is -- same
// field schema, same os_db_id extension). Only the 4-byte CC form is implemented (byte
// 0 = 0xE1, byte 2 = MLEN in 8-byte units, 1-byte range) -- every NFC-V tag this
// project targets (SLI-X 112 B .. ST25DV16K 2 KB) fits well under the 4-byte CC's
// 2040-byte ceiling, so the 8-byte CC form (needed only past that) isn't built.
// CC byte layout verified against ST's AN4911/community documentation of the NFC
// Forum Type 5 Tag spec (not guessed) -- 0xE1 magic, 0x40 = version 1.0/all access,
// byte 2 = MLEN (T5T_Area size in 8-byte units), byte 3 = feature flags (0x00, no
// special read/write requirements advertised).
static const uint8_t NFCV_OS_CC_BLOCK = 0;     // Capability Container
static const uint8_t NFCV_OS_NDEF_BLOCK = 1;   // NDEF TLV starts here

// Same OTP-style caution as the NTAG CC: page/block 0 on a Type 5 tag isn't literally
// one-way-settable like NTAG's OTP bits, but treating it the same way (only write if
// virgin, leave alone if already valid NDEF, refuse anything else) avoids clobbering a
// tag that might carry unrelated vendor data in block 0.
inline bool pn5180NfcvCcIsVirgin(const uint8_t cc[4], bool &alreadyNdefOut) {
  alreadyNdefOut = (cc[0] == 0xE1);
  return cc[0] == 0 && cc[1] == 0 && cc[2] == 0 && cc[3] == 0;
}


// Writes the OpenSpool NDEF message to an NFC-V/Type-5 tag. Same length-then-message,
// magic-block-last discipline as the NTAG version: TLV length written as 0x00 first,
// patched to the real value LAST, so an interrupted write reads back as an empty
// (harmless) NDEF message rather than a corrupt one.
// Queries the tag's real capacity via getSystemInfo() for the nfcvOpenSpool budget --
// unlike the legacy NFC-V write path (pn5180WriteNfcvId) and nfcvExtended, which both
// deliberately skip it (see their own comments: the library parses the response
// without checking length against the flag bits it reads -> an OOB read/possible
// crash on a short/non-conforming reply), OpenSpool's JSON payload is large enough
// that a bigger tag (SLIX2 ~316 B, ST25DV16K 2 KB) genuinely benefits from knowing its
// true size -- a fixed 112 B budget would otherwise drop fields (weight/diameter/
// temps) on every write, no matter how much room the tag actually has. Mitigates the
// crash risk with a sanity-checked fallback: only trusts the result if blockSize/
// numBlocks are both nonzero and the computed capacity isn't absurd (>2080 B -- past
// the largest tag this project has evaluated, see "Tags evaluated for NFC-V" in
// HARDWARE.md); anything else (call failure, garbage values) falls back to the
// conservative 112 B SLI-X assumption. bsOut/nbOut (optional) return the raw
// getSystemInfo values for logging.
inline int pn5180NfcvUserBytes(const uint8_t uid[8], uint8_t *bsOut, uint8_t *nbOut) {
  uint8_t bs = 0, nb = 0, uidTmp[8];
  memcpy(uidTmp, uid, 8);
  int userBytes = 112;
  if (g_pn5180->getSystemInfo(uidTmp, &bs, &nb) == ISO15693_EC_OK && bs > 0 && nb > 0) {
    int cap = (int)bs * (int)nb;
    if (cap >= 32 && cap <= 2080) userBytes = cap;
  }
  if (bsOut) *bsOut = bs;
  if (nbOut) *nbOut = nb;
  return userBytes;
}

inline bool pn5180WriteNfcvOpenSpool(const uint8_t uid[8], const SpoolTagData &d,
                                     String &droppedFieldsOut, int &bytesWrittenOut,
                                     String &errOut) {
  errOut = ""; droppedFieldsOut = ""; bytesWrittenOut = 0;
  if (d.databaseId < 0) { errOut = "databaseId required"; return false; }

  uint8_t bs = 0, nb = 0;
  int userBytes = pn5180NfcvUserBytes(uid, &bs, &nb);
  const int budget = userBytes - 4 /* CC block */ - 22 /* NDEF/record overhead */;
  dbgLogf("NFC-V OpenSpool write: userBytes=%d (bs=%u nb=%u) jsonBudget=%d", userBytes, bs, nb, budget);

  String json = pn5180BuildOpenSpoolJson(d, budget, droppedFieldsOut);
  if (json.length() == 0) {
    errOut = "spool data too large even after dropping optional fields";
    return false;
  }
  int payloadLen = json.length();
  int msgLen = 1 + 1 + 1 + 16 + payloadLen;  // header+typeLen+payloadLen+type+json
  if (msgLen > 255) {
    errOut = "NDEF message too large for this tag";
    return false;
  }

  uint8_t cc[4];
  if (!pn5180NfcvReadBlock(uid, NFCV_OS_CC_BLOCK, cc)) { errOut = "CC read failed"; return false; }
  bool alreadyNdef;
  bool virgin = pn5180NfcvCcIsVirgin(cc, alreadyNdef);
  if (!virgin && !alreadyNdef) {
    errOut = "tag's Capability Container is neither virgin nor a recognized NDEF CC -- refusing to write (would risk permanently damaging the tag)";
    return false;
  }

  int bodyLen = 2 + msgLen + 1;  // TLV tag+len(placeholder) + message + terminator
  int totalLen = ((bodyLen + 3) / 4) * 4;  // pad to a 4-byte block boundary
  uint8_t *buf = (uint8_t *)calloc(totalLen, 1);
  if (!buf) { errOut = "out of memory"; return false; }
  buf[0] = 0x03; buf[1] = 0x00;  // TLV tag=NDEF message, len=0 (placeholder, patched last)
  int p = 2;
  buf[p++] = 0xD2;                             // record header: MB|ME|SR|TNF=MIME
  buf[p++] = (uint8_t)strlen(NDEF_MIME_TYPE);  // type length
  buf[p++] = (uint8_t)payloadLen;              // payload length (short record)
  memcpy(buf + p, NDEF_MIME_TYPE, strlen(NDEF_MIME_TYPE)); p += strlen(NDEF_MIME_TYPE);
  memcpy(buf + p, json.c_str(), payloadLen); p += payloadLen;
  buf[p++] = 0xFE;  // terminator TLV

  bool ok = true;
  if (virgin) {
    uint8_t newCc[4] = { 0xE1, 0x40, (uint8_t)(userBytes / 8), 0x00 };  // MLEN in 8-byte units
    ok = pn5180NfcvWriteBlock(uid, NFCV_OS_CC_BLOCK, newCc, errOut);
    dbgLogf("NFC-V OpenSpool write: CC written (was virgin), ok=%d", ok);
  }
  for (int i = 0; ok && i < totalLen / 4; i++) {
    ok = pn5180NfcvWriteBlock(uid, NFCV_OS_NDEF_BLOCK + i, buf + i * 4, errOut);
  }
  if (ok) {
    uint8_t lenBlock[4];
    if (pn5180NfcvReadBlock(uid, NFCV_OS_NDEF_BLOCK, lenBlock)) {
      lenBlock[1] = (uint8_t)msgLen;  // patch the real length into the already-written block
      ok = pn5180NfcvWriteBlock(uid, NFCV_OS_NDEF_BLOCK, lenBlock, errOut);
    } else { ok = false; errOut = "length-patch read failed"; }
  }
  free(buf);
  if (ok) bytesWrittenOut = totalLen;
  dbgLogf("NFC-V OpenSpool write: done ok=%d err=%s bytes=%d", ok, errOut.c_str(), bytesWrittenOut);
  return ok;
}

// Reads back an NFC-V OpenSpool NDEF tag. Same parse as the NTAG version (identical
// JSON schema) -- only the block-read plumbing differs.
inline bool pn5180ReadNfcvOpenSpool(const uint8_t uid[8], SpoolTagData &out) {
  out = SpoolTagData();
  uint8_t hdr[8];  // blocks 1-2: TLV tag/len + record header/typeLen/payloadLen
  if (!pn5180NfcvReadBlock(uid, NFCV_OS_NDEF_BLOCK, hdr) ||
      !pn5180NfcvReadBlock(uid, NFCV_OS_NDEF_BLOCK + 1, hdr + 4)) return false;
  if (hdr[0] != 0x03) return false;  // no NDEF TLV -> not an OpenSpool tag
  uint8_t msgLen = hdr[1];
  if (msgLen == 0) return false;  // empty/aborted-write placeholder, not a real record
  if (hdr[2] != 0xD2) return false;  // not our short MIME record shape
  uint8_t typeLen = hdr[3];
  uint8_t payloadLen = hdr[4];
  if (typeLen != 16 || payloadLen == 0 || payloadLen > 220) return false;

  int totalNeeded = 2 + 3 + typeLen + payloadLen;
  int blocksNeeded = (totalNeeded + 3) / 4 + 1;  // +1 rounding margin
  uint8_t *buf = (uint8_t *)malloc(blocksNeeded * 4);
  if (!buf) return false;
  bool ok = true;
  for (int i = 0; i < blocksNeeded && ok; i++)
    ok = pn5180NfcvReadBlock(uid, NFCV_OS_NDEF_BLOCK + i, buf + i * 4);
  if (ok) {
    const char *jsonStart = (const char *)(buf + 2 + 3 + typeLen);
    JsonDocument doc;
    if (!deserializeJson(doc, jsonStart, payloadLen)) {
      if (doc["os_db_id"].is<long>() || doc["os_db_id"].is<const char*>())
        out.databaseId = doc["os_db_id"] | -1L;
      out.material = String((const char *)(doc["type"] | ""));
      out.vendor = String((const char *)(doc["brand"] | ""));
      const char *ch = doc["color_hex"] | "";
      // OpenSpool encodes "no colour" by omitting the key, so a present "000000" is
      // black -- no zero-check here. colorCount is set so colorFull carries it too, but
      // only if the hex actually parses: color_hex is arbitrary JSON and may be junk.
      if (ch && ch[0]) {
        out.color = String("#") + ch;
        if (pn5180ParseColorHex(out.color, out.colorRgb[0])) out.colorCount = 1;
      }
      float w = doc["weight"] | -1.0f; if (w > 0) out.totalWeight = w;
      float dia = doc["diameter"] | -1.0f; if (dia > 0) out.diameter = dia;
      int mn = doc["min_temp"] | -1; if (mn >= 0) out.temperatureMin = mn;
      int mx = doc["max_temp"] | -1; if (mx >= 0) out.temperatureMax = mx;
      int bn = doc["bed_min_temp"] | -1; if (bn >= 0) out.bedTemperatureMin = bn;
      int bx = doc["bed_max_temp"] | -1; if (bx >= 0) out.bedTemperatureMax = bx;
    } else {
      ok = false;
    }
  }
  free(buf);
  return ok;
}

// --- Unified write dispatch (plan C.4) -----------------------------------------
// Replaces the old "try NFC-V, then NTAG, then retry Mifare if NTAG's error string
// starts with 'unsupported tag ('" chain: that string-sniffing approach doesn't scale
// past two fallback formats and is fragile (any wording change in the NTAG rejection
// message would silently break the Mifare retry). This probes the tag ONCE up front
// (getInventory for NFC-V, then a NFC-A probe + SAK check) and dispatches directly to
// the one write function that applies -- no retry-on-error-text needed.
enum PN5180WriteKind { PN5180_WK_NONE, PN5180_WK_NFCV, PN5180_WK_NTAG, PN5180_WK_MIFARE };

// nfcvUidOut: raw 8-byte ISO15693 UID, filled only when the result is PN5180_WK_NFCV
// (NFC-V block commands need the raw UID, not a hex string -- avoids a second
// getInventory() call in pn5180WriteSpoolTag's NFC-V branch).
inline PN5180WriteKind pn5180ProbeWriteKind(String &uidHexOut, uint8_t nfcvUidOut[8] = nullptr) {
  uidHexOut = "";
  if (!g_pn5180Ready || !g_pn5180) return PN5180_WK_NONE;
  uint8_t uid[8] = {0};
  if (g_pn5180->getInventory(uid) == ISO15693_EC_OK) {
    if (nfcvUidOut) memcpy(nfcvUidOut, uid, 8);
    g_pn5180->reset(); g_pn5180->setupRF();
    return PN5180_WK_NFCV;
  }
  g_pn5180->reset(); g_pn5180->setupRF();
  PN5180ProbeResult pr;
  if (!pn5180ProbeNfcA(pr)) { g_pn5180->reset(); g_pn5180->setupRF(); return PN5180_WK_NONE; }
  uidHexOut = pr.uid;
  bool isClassic = (pr.sak == 0x08 || pr.sak == 0x18 || pr.sak == 0x09 || pr.sak == 0x28);
  g_pn5180->reset(); g_pn5180->setupRF();
  return isClassic ? PN5180_WK_MIFARE : PN5180_WK_NTAG;
}

// Legacy ID-only write (used by /nfcwriteid): probes once, dispatches directly.
inline bool pn5180WriteLegacyId(long id, String &errOut) {
  String uidHex;
  PN5180WriteKind kind = pn5180ProbeWriteKind(uidHex);
  switch (kind) {
    case PN5180_WK_NFCV:   return pn5180WriteNfcvId(id, errOut);
    case PN5180_WK_NTAG:   return pn5180WriteNtagId(id, errOut);
    case PN5180_WK_MIFARE: return pn5180WriteMifareId(id, errOut);
    default: errOut = "no tag found"; return false;
  }
}

// Spool-fields write (used by /nfcwritespool): probes once, dispatches to the format
// that matches the tag. NFC-V can go to either of two formats -- opened up by explicit
// user request for blank/user-purchased NFC-V tags specifically; still never
// auto-applied to a tag the user hasn't chosen to write to (the write only ever
// happens via an explicit /nfcwritespool call, same trust boundary as before).
// nfcvFormat selects which one for an NFC-V tag ("extended" = OctoScale's own binary
// nfcvExtended layout, default; "openSpool" = standard Type 5 NDEF, readable by a
// generic phone NFC app) -- a per-request choice, not auto-detected from the tag,
// since either format fits on any NFC-V tag this project targets. ntagFormat is the
// same idea for NTAG ("openSpool" = NDEF, default/backward-compatible; "extended" =
// ntagExtended's own binary layout, NTAG215/216 only -- pn5180WriteNtagExtended itself
// refuses on a 213, see its own comment). Irrelevant for Mifare (only one format).
inline bool pn5180WriteSpoolTag(const SpoolTagData &d, String &formatOut,
                                int &bytesWrittenOut, String &droppedFieldsOut, String &errOut,
                                const String &nfcvFormat = "extended",
                                const String &ntagFormat = "openSpool") {
  formatOut = ""; bytesWrittenOut = 0; droppedFieldsOut = ""; errOut = "";
  String uidHex;
  uint8_t nfcvUid[8];
  PN5180WriteKind kind = pn5180ProbeWriteKind(uidHex, nfcvUid);
  switch (kind) {
    case PN5180_WK_NFCV: {
      if (nfcvFormat == "openSpool") {
        formatOut = "nfcvOpenSpool";
        // No separate legacy-anchor write here, same reasoning as NTAG's openSpool:
        // the NDEF message starts at block 1 right after blocks the legacy anchor
        // would otherwise use (0-2) -- os_db_id inside the JSON is the only id on
        // this tag, exactly like NTAG openSpool overwriting pages 4-6.
        return pn5180WriteNfcvOpenSpool(nfcvUid, d, droppedFieldsOut, bytesWrittenOut, errOut);
      }
      formatOut = "nfcvExtended";
      // Legacy anchor (blocks 0-2) first, then the Extended blocks -- same "anchor
      // stays byte-identical, written independently" pattern as Mifare's block 4.
      if (!pn5180WriteNfcvId(d.databaseId, errOut)) return false;
      bytesWrittenOut += PN5180_ID_BYTES;
      int extBytes = 0;
      String extErr;
      bool ok = pn5180WriteNfcvExtended(nfcvUid, d, extBytes, droppedFieldsOut, extErr);
      bytesWrittenOut += extBytes;
      if (!ok) errOut = extErr;  // legacy anchor still landed even if Extended failed
      return ok;
    }
    case PN5180_WK_NTAG:
      if (ntagFormat == "extended") {
        formatOut = "ntagExtended";
        // No separate legacy-anchor write, same reasoning as NTAG openSpool/NFC-V
        // openSpool: databaseId lives inside the Extended pages themselves (page 5).
        return pn5180WriteNtagExtended(d, bytesWrittenOut, droppedFieldsOut, errOut);
      }
      if (ntagFormat == "tigerTag") {
        formatOut = "tigerTag";
        // Foreign format, no truncatable strings -- droppedFieldsOut stays empty.
        return pn5180WriteNtagTigerTag(d, bytesWrittenOut, errOut);
      }
      formatOut = "openSpool";
      return pn5180WriteNtagOpenSpool(d, droppedFieldsOut, bytesWrittenOut, errOut);
    case PN5180_WK_MIFARE: {
      formatOut = "octoscaleExtended";
      // Legacy anchor (block 4) first, then the Extended sectors -- matches C.1's
      // "block 4 stays byte-identical, written independently" design.
      if (!pn5180WriteMifareId(d.databaseId, errOut)) return false;
      bytesWrittenOut += PN5180_ID_BYTES;
      int extBytes = 0;
      String extErr;
      bool ok = pn5180WriteMifareExtended(uidHex, d, extBytes, droppedFieldsOut, extErr);
      bytesWrittenOut += extBytes;
      if (!ok) errOut = extErr;  // legacy anchor still landed even if Extended failed
      return ok;
    }
    default:
      errOut = "no tag found";
      return false;
  }
}

// --- Erase (used by /nfcerase) --------------------------------------------------
// Wipes every area OctoScale could have written on the tag -- the legacy ID anchor
// AND all Extended-format areas, regardless of which format (if any) is actually
// present, since erase doesn't know in advance what's on the tag and blank areas
// are harmless to overwrite with the same zero pattern anyway. Afterwards the tag
// reads back exactly like a virgin, never-written tag to every read path in this
// file (idParsed=-1, hasExtended=false).
//
// NOT a factory reset: the Capability Container on NTAG/NFC-V-OpenSpool tags is an
// NXP/Type-5-spec OTP field (bits only ever settable 0->1) -- once it's been written
// as a valid NDEF CC, no amount of rewriting can un-set it back to all-zero virgin
// state. That's a hardware property of the tag, not a firmware limitation. What IS
// erased: the NDEF message itself, patched back to a zero-length TLV (0x03 0x00) --
// functionally empty, reads as "no NDEF content" to any reader, same as the
// commit-marker-last write path already treats an aborted write. Mifare/NFC-V's own
// binary Extended areas have no OTP concept and are erased outright (all-zero).
inline bool pn5180EraseTag(String &formatOut, String &errOut) {
  formatOut = ""; errOut = "";
  String uidHex;
  uint8_t nfcvUid[8];
  PN5180WriteKind kind = pn5180ProbeWriteKind(uidHex, nfcvUid);
  switch (kind) {
    case PN5180_WK_NFCV: {
      formatOut = "nfcv";
      uint8_t zero4[4] = {0, 0, 0, 0};
      bool ok = true;
      // Block 0 doubles as the legacy anchor's first ID byte on nfcvExtended AND as
      // the Capability Container on nfcvOpenSpool/OpenPrintTag (both put their CC at
      // block 0, see NFCV_OS_CC_BLOCK/OPT_CC_BLOCK) -- erase doesn't know in advance
      // which format (if any) is on the tag, so block 0 needs format-aware handling:
      // a valid NDEF CC there is OTP (same one-way-settable caveat as NTAG's CC, see
      // pn5180NfcvCcIsVirgin's comment) and must survive an erase, exactly like NTAG's
      // erase already leaves its own CC (page 3) untouched below. This used to zero
      // block 0 unconditionally, which silently violated that same OTP rule for any
      // nfcvOpenSpool/OpenPrintTag tag -- caught while adding OpenPrintTag support.
      uint8_t cc[4];
      bool ccIsNdef = pn5180NfcvReadBlock(nfcvUid, 0, cc) && cc[0] == 0xE1;
      uint8_t startBlk = ccIsNdef ? 1 : 0;
      for (uint8_t blk = startBlk; ok && blk <= 22; blk++) {
        ok = pn5180NfcvWriteBlock(nfcvUid, blk, zero4, errOut);
      }
      // Block 1 was just zeroed above when ccIsNdef -- that already reads back as TLV
      // tag=0x00 (not a valid 0x03 NDEF marker), so every read path here already sees
      // "no NDEF content" without needing a separate length-patch step (unlike NTAG,
      // where the CC itself is preserved but block 1's TLV byte is deliberately left
      // as a valid 0x03 tag with length patched to 0 -- NFC-V's sweep already zeroes
      // that whole block, so there's nothing left to patch specifically).
      return ok;
    }
    case PN5180_WK_NTAG: {
      formatOut = "ntag";
      uint8_t zero4[4] = {0, 0, 0, 0};
      bool ok = true;
      // Legacy anchor (pages 4-6) + NDEF TLV area. The CC (page 3) is deliberately
      // left untouched -- see the function comment above. NDEF is erased by patching
      // the TLV length byte (page 4) back to 0x00, not by zeroing the whole message
      // area (unnecessary and slower -- a zero-length TLV already reads as "no
      // content" to every reader, this codebase's own included).
      // Page 4 gets reselect=true like the rest: the caller's probe leaves the tag
      // selected, but an Extended write immediately before this leaves the transceiver
      // in a state where the very first WRITE can be dropped silently -- which left the
      // Extended magic ('O','X') standing on page 4 while erase reported success.
      for (uint8_t page = 4; ok && page <= 6; page++) {
        ok = pn5180NtagWritePage(uidHex, page, zero4, /*reselect=*/true, errOut);
      }
      // An Extended tag keeps its payload out to page ~34, but only page 4 decides:
      // pn5180ReadNtagExtended() bails on its first two bytes ('O','X') and
      // pn5180NtagOccupancy() reads the same page. Once 4-6 are zero the rest is
      // unreachable by every read path here, so sweeping all ~30 Extended pages would
      // cost ~350ms each (measured: an 11s Extended write) to erase data nothing can
      // reach. What actually broke was page 4 not being zeroed at all -- see above.
      if (ok) {
        uint8_t lenPage[4];
        if (pn5180NtagReadPages(4, 1, lenPage)) {
          if (lenPage[0] == 0x03 && lenPage[1] != 0x00) {  // an NDEF TLV is present
            lenPage[1] = 0x00;
            ok = pn5180NtagWritePage(uidHex, 4, lenPage, /*reselect=*/true, errOut);
          }
        }
      }
      g_pn5180->reset(); g_pn5180->setupRF();
      return ok;
    }
    case PN5180_WK_MIFARE: {
      formatOut = "mifareClassic1k";
      uint8_t zero16[16] = {0};
      // Re-select: pn5180ProbeWriteKind() above ends with reset()+setupRF(), which
      // deselects the card -- Crypto1 auth requires an actively selected card in the
      // same RF session (same bug/fix as pn5180WriteMifareExtended's own re-select).
      PN5180ProbeResult reselectPr;
      if (!pn5180ProbeNfcA(reselectPr) || reselectPr.uid != uidHex) {
        errOut = "lost tag before erase";
        g_pn5180->reset(); g_pn5180->setupRF();
        return false;
      }
      // Legacy anchor (block 4, sector 1 -- already authenticated by the block-4 auth
      // below) + Extended sectors 2 (blocks 8-9) and 3 (blocks 12-14).
      String authErr;
      if (!pn5180MifareAuth(uidHex, 4, authErr)) { errOut = authErr; return false; }
      if (!pn5180MifareWriteBlock(4, zero16, errOut)) return false;
      if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK8, authErr)) { errOut = authErr; return false; }
      if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK8, zero16, errOut)) return false;
      if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK9, zero16, errOut)) return false;
      if (!pn5180MifareAuth(uidHex, MIFARE_EXT_BLOCK12, authErr)) { errOut = authErr; return false; }
      if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK12, zero16, errOut)) return false;
      if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK13, zero16, errOut)) return false;
      if (!pn5180MifareWriteBlock(MIFARE_EXT_BLOCK14, zero16, errOut)) return false;
      return true;
    }
    default:
      errOut = "no tag found";
      return false;
  }
}
