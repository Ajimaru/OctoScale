#pragma once
#include <Arduino.h>
#include "cbor.h"
#include "pn5180nfc.h"

// openprinttag.h — OpenPrintTag (OPT) read/write for NFC-V tags.
// Spec: https://github.com/OpenPrintTag/openprinttag-specification
//       (rendered: https://specs.openprinttag.org/)
//
// OPT is a vendor-neutral filament-tag standard: NFC-V, one NDEF record, MIME type
// application/vnd.openprinttag, CBOR payload split into Meta/Main/Aux regions. Unlike
// OctoScale's own nfcvExtended/nfcvOpenSpool, an OPT tag carries NO OctoScale database
// ID -- the load flow falls back to a UID lookup for these tags (see pn5180ReadSpoolEx's
// caller in main.cpp, which already has that fallback via g_flowLookupByCode).
//
// Layout on the wire (verified against data/*.yaml, docs_src/nfc_data_format.md, and
// utils/nfc_initialize.py in the spec repo, not guessed):
//   Block 0        : Capability Container (4 B) -- E1 40 <size/8> <flags>, same shape
//                     as nfcvOpenSpool's CC (see NFCV_OS_CC_BLOCK).
//   Blocks 1+       : ONE NDEF TLV (0x03 <len> <record> 0xFE), record = MIME "application/
//                     vnd.openprinttag", payload = [Meta][Main][Aux].
//   Meta (8 B)      : CBOR map, keys 0-3 (all optional): main_region_offset/size,
//                      aux_region_offset/size, all relative to the NDEF payload start.
//                      OctoScale always writes all four explicitly (no relying on the
//                      "if absent" defaults) so a reader never has to guess.
//   Main            : CBOR map, integer keys from main_fields.yaml (material_class,
//                      material_name, temperatures, ...) -- see optFieldsMap below.
//   Aux (32 B)      : empty CBOR map ({}) -- OctoScale writes but never populates it
//                      (no per-vendor fields it needs to carry yet).
// No magic bytes, no version marker -- the spec deliberately omits them (breaking
// changes would get a new MIME type instead), so detection is "CC valid + NDEF TLV
// present + record MIME matches", nothing more.

static const uint8_t OPT_CC_BLOCK = 0;    // same block as nfcvOpenSpool's CC (disjoint tags never mix formats on one write)
static const uint8_t OPT_NDEF_BLOCK = 1;  // NDEF TLV starts here
static const char OPT_MIME_TYPE[] = "application/vnd.openprinttag";  // 29 bytes, no NUL
// Fixed size for the Meta region -- NOT the reference tool's max_meta_section_size (8),
// which is a different constraint (its own minimum-payload assertion), not the actual
// bytes OctoScale's 4 CBOR key/value pairs need. Meta always holds exactly 4 uint keys
// (0-3, 1 byte each) + 4 uint values (offsets/sizes) whose CBOR width depends on the
// tag's capacity: values <24 cost 1 B, <256 cost 2 B, <65536 cost 3 B. Worst case (all
// four values needing the 3-byte form, plausible on a large tag like an ST25DV16K)
// costs 1 (map header) + 4*(1+3) = 17 B -- computed and verified against a host-side
// test before settling on this number, not guessed. 24 leaves headroom.
static const int OPT_META_SIZE = 24;
static const int OPT_AUX_SIZE = 32;    // spec: SHALL be >=16 B if present, 32 B recommended

// --- Field keys (main_fields.yaml) ----------------------------------------------
// Only the subset OctoScale can actually fill from SpoolTagData -- the spec defines
// far more (GTIN, UUIDs, drying parameters, SLA-specific fields, ...) that have no
// source in SpoolManagerExtended's data model today.
enum OptFieldKey {
  OPT_K_MATERIAL_CLASS = 8,             // enum, required -- OctoScale always writes 0 (FFF)
  OPT_K_MATERIAL_TYPE = 9,              // enum, recommended -- see optMaterialTypeEnum()
  OPT_K_MATERIAL_NAME = 10,             // text, max 63 B
  OPT_K_BRAND_NAME = 11,                // text, max 31 B
  OPT_K_NOMINAL_FULL_WEIGHT = 16,       // number, g
  OPT_K_EMPTY_CONTAINER_WEIGHT = 18,    // number, g
  OPT_K_PRIMARY_COLOR = 19,             // bstr, RGBA (3-4 B) -- OctoScale writes 3 (RGB, no alpha)
  OPT_K_DENSITY = 29,                   // number, g/cm3
  OPT_K_FILAMENT_DIAMETER = 30,         // number, mm
  OPT_K_MIN_PRINT_TEMPERATURE = 34,     // int, deg C
  OPT_K_MAX_PRINT_TEMPERATURE = 35,     // int, deg C
  OPT_K_PREHEAT_TEMPERATURE = 36,       // int, deg C -- OctoScale's "target" temperature value
  OPT_K_MIN_BED_TEMPERATURE = 37,       // int, deg C
  OPT_K_MAX_BED_TEMPERATURE = 38,       // int, deg C
  // Opacity, NOT a length: 0.1 = most opaque, 100 = most transparent ("HueForge TD").
  OPT_K_TRANSMISSION_DISTANCE = 27,     // number, dimensionless
  OPT_K_DRYING_TEMPERATURE = 57,        // int, deg C
  OPT_K_DRYING_TIME = 58,               // int, MINUTES (not hours -- spec unit)
};
static const int OPT_MATERIAL_CLASS_FFF = 0;  // material_class_enum.yaml: 0=FFF (filament), 1=SLA (resin)

// material_type_enum.yaml, verbatim (43 entries, key = array index). No "unknown"
// value exists in the spec -- a material name that doesn't match one of these is
// written WITHOUT key 9 (material_type is only "recommended"), material_name (key 10,
// free text) still carries it.
static const char *const OPT_MATERIAL_TYPES[] = {
  "PLA","PETG","TPU","ABS","ASA","PC","PCTG","PP","PA6","PA11","PA12","PA66","CPE","TPE",
  "HIPS","PHA","PET","PEI","PBT","PVB","PVA","PEKK","PEEK","BVOH","TPC","PPS","PPSU","PVC",
  "PEBA","PVDF","PPA","PCL","PES","PMMA","POM","PPE","PS","PSU","TPI","SBS","OBC","EVA","PA612"
};
static const int OPT_MATERIAL_TYPE_COUNT = sizeof(OPT_MATERIAL_TYPES) / sizeof(OPT_MATERIAL_TYPES[0]);

// Case-insensitive match against the abbreviation list. Returns -1 if none matches --
// caller must then omit key 9 rather than write a wrong/guessed value.
inline int optMaterialTypeIndex(const String &material) {
  String m = material; m.trim();
  for (int i = 0; i < OPT_MATERIAL_TYPE_COUNT; i++) {
    if (m.equalsIgnoreCase(OPT_MATERIAL_TYPES[i])) return i;
  }
  return -1;
}

// --- Same OTP-style CC caution as nfcvOpenSpool (see pn5180NfcvCcIsVirgin) ------
// Reused directly rather than reimplemented -- identical CC shape (E1 40 <MLEN> <flags>),
// same "only write if virgin, leave alone if already valid, refuse anything else" rule.

// --- Block-buffer helpers --------------------------------------------------------
// OPT's CBOR values routinely span multiple 4-byte NFC-V blocks (unlike nfcvOpenSpool's
// byte-for-byte NDEF copy, a CBOR map's fields don't align to block boundaries at all),
// so read/write work over a flat byte buffer that's block-read/written in bulk, not
// field-by-field like the fixed binary formats.

inline bool optReadBytes(const uint8_t uid[8], int payloadStartBlock, int byteOffset,
                         uint8_t *out, int len) {
  int firstBlock = payloadStartBlock + byteOffset / 4;
  int blockOfs = byteOffset % 4;
  int blocksNeeded = (blockOfs + len + 3) / 4;
  for (int i = 0; i < blocksNeeded; i++) {
    uint8_t blk[4];
    if (!pn5180NfcvReadBlock(uid, (uint8_t)(firstBlock + i), blk)) return false;
    int copyStart = (i == 0) ? blockOfs : 0;
    int copyEnd = 4;
    if (i == blocksNeeded - 1) copyEnd = ((blockOfs + len - 1) % 4) + 1;
    for (int b = copyStart; b < copyEnd; b++) {
      int destIdx = (i * 4 + b) - blockOfs;
      if (destIdx >= 0 && destIdx < len) out[destIdx] = blk[b];
    }
  }
  return true;
}

// Writes len bytes at byteOffset within the region starting at payloadStartBlock.
// Partial blocks at either end are read-modify-written so bytes outside [byteOffset,
// byteOffset+len) that share a block are preserved (e.g. writing 5 bytes starting at
// offset 2 touches bytes 2-3 of block 0 and bytes 0-2 of block 1 -- byte 0-1 of block 0
// must survive untouched).
inline bool optWriteBytes(const uint8_t uid[8], int payloadStartBlock, int byteOffset,
                          const uint8_t *data, int len, String &errOut) {
  int firstBlock = payloadStartBlock + byteOffset / 4;
  int blockOfs = byteOffset % 4;
  int blocksNeeded = (blockOfs + len + 3) / 4;
  for (int i = 0; i < blocksNeeded; i++) {
    uint8_t blk[4];
    bool needsMerge = (i == 0 && blockOfs != 0) || (i == blocksNeeded - 1 && (blockOfs + len) % 4 != 0);
    if (needsMerge) {
      if (!pn5180NfcvReadBlock(uid, (uint8_t)(firstBlock + i), blk)) { errOut = "region read failed"; return false; }
    }
    int copyStart = (i == 0) ? blockOfs : 0;
    int copyEnd = 4;
    if (i == blocksNeeded - 1) copyEnd = ((blockOfs + len - 1) % 4) + 1;
    for (int b = copyStart; b < copyEnd; b++) {
      int srcIdx = (i * 4 + b) - blockOfs;
      if (srcIdx >= 0 && srcIdx < len) blk[b] = data[srcIdx];
    }
    if (!pn5180NfcvWriteBlock(uid, (uint8_t)(firstBlock + i), blk, errOut)) return false;
  }
  return true;
}

// --- Detection -------------------------------------------------------------------
// Result of a successful detect: enough to read/rewrite Main without re-parsing Meta.
struct OptLayout {
  int ndefPayloadStartBlock = 0;  // block index where the NDEF record's payload begins
  int metaOffset = 0, metaSize = 0;
  int mainOffset = 0, mainSize = 0;
  int auxOffset = 0, auxSize = 0;  // auxSize=0 -> no aux region
};

// Parses the Meta region (already read into buf) into region offsets/sizes. All four
// keys are optional per the spec; OctoScale itself always writes all four (see
// optInitialize), but a foreign tag might omit some -- fall back to the spec's stated
// defaults (main follows meta immediately; no aux_region_offset means no aux region).
inline bool optParseMeta(const uint8_t *buf, int metaLen, OptLayout &layout) {
  int pos = 0;
  CborHeader mh;
  if (!cborReadHeader(buf, pos, metaLen, mh) || mh.majorType != CBOR_MT_MAP) return false;
  int mainOffset = -1, mainSize = -1, auxOffset = -1, auxSize = -1;
  for (uint64_t i = 0; i < mh.arg; i++) {
    CborHeader kh, vh;
    if (!cborReadHeader(buf, pos, metaLen, kh)) return false;
    int64_t key;
    if (!cborHeaderToInt(kh, key)) { if (!cborSkipValue(buf, pos, metaLen, kh)) return false; continue; }
    if (!cborReadHeader(buf, pos, metaLen, vh)) return false;
    int64_t val;
    bool isInt = cborHeaderToInt(vh, val);
    if (!isInt) { if (!cborSkipValue(buf, pos, metaLen, vh)) return false; continue; }
    if (key == 0) mainOffset = (int)val;
    else if (key == 1) mainSize = (int)val;
    else if (key == 2) auxOffset = (int)val;
    else if (key == 3) auxSize = (int)val;
  }
  layout.mainOffset = (mainOffset >= 0) ? mainOffset : layout.metaSize;
  layout.mainSize = mainSize;  // -1 = "until next region/payload end", resolved by the caller
  layout.auxOffset = auxOffset;  // -1 = no aux region
  layout.auxSize = auxSize;
  return true;
}

// --- NDEF/TLV framing sizes ------------------------------------------------------
// Both the TLV length field and the NDEF record's own payload-length field have a
// short form (1 B / 1 B) and a long form (3 B incl. 0xFF marker / 4 B) -- OctoScale's
// other NDEF writers (NTAG/nfcvOpenSpool's JSON) never exceed the short forms' 254/255
// B ceiling, but OpenPrintTag's whole reason to target larger NFC-V tags (SLIX2 320 B,
// ST25DV 2 KB) is to fit MORE than that, so both writer (optInitialize) and reader
// (optDetect) need to handle whichever form the actual payload size requires.
//
// TLV:    0x03 <len>                      if len <= 254
//         0x03 0xFF <len_hi> <len_lo>     if len >= 255 (len is the 2-byte BE value)
// Record: <hdr> <typeLen> <payloadLen>              <type> ...   if SR bit set (hdr&0x10), payload <= 255
//         <hdr> <typeLen> <len3><len2><len1><len0>  <type> ...   if SR bit clear, payload up to 4 GiB
// hdr = 0xC2 (MB|ME|TNF=MIME) when SR is clear, 0xD2 (MB|ME|SR|TNF=MIME) when SR is set.

struct OptFraming {
  int tlvHeaderLen;      // 2 or 4
  int recordHeaderLen;   // 3+typeLen (short) or 6+typeLen (long)
  int payloadLen;        // NDEF record's payload length (Meta+Main+Aux)
  int payloadByteOffset; // where the CBOR payload starts, bytes from OPT_NDEF_BLOCK
};

// Reads and parses the TLV+record header at OPT_NDEF_BLOCK, in either short or long
// form, and verifies the record's type matches OPT_MIME_TYPE. Returns false if
// anything about the framing doesn't look like an OctoScale-written OPT tag.
inline bool optReadFraming(const uint8_t uid[8], OptFraming &f) {
  uint8_t hdr[12] = {0};  // enough for the longest possible header (TLV long form + record long form)
  if (!optReadBytes(uid, OPT_NDEF_BLOCK, 0, hdr, 12)) return false;
  if (hdr[0] != 0x03) return false;  // no NDEF TLV

  int tlvLen, tlvHeaderLen;
  int p = 1;
  if (hdr[1] == 0xFF) {
    tlvLen = (hdr[2] << 8) | hdr[3];
    tlvHeaderLen = 4;
    p = 4;
  } else {
    tlvLen = hdr[1];
    tlvHeaderLen = 2;
    p = 2;
  }
  if (tlvLen == 0) return false;

  uint8_t recHdr = hdr[p];
  bool sr = (recHdr & 0x10) != 0;
  if ((recHdr & 0x07) != 0x02) return false;  // TNF must be 0x02 (MIME) -- top 5 bits (MB/ME/CF/SR/IL) vary, TNF is the low 3
  uint8_t typeLen = hdr[p + 1];
  int mimeLen = (int)strlen(OPT_MIME_TYPE);
  if (typeLen != mimeLen) return false;

  int payloadLen, recordHeaderLen;
  int typeStringOffset;
  if (sr) {
    payloadLen = hdr[p + 2];
    recordHeaderLen = 3 + typeLen;
    typeStringOffset = p + 3;
  } else {
    payloadLen = ((int)hdr[p+2] << 24) | ((int)hdr[p+3] << 16) | ((int)hdr[p+4] << 8) | hdr[p+5];
    recordHeaderLen = 6 + typeLen;
    typeStringOffset = p + 6;
  }
  if (payloadLen == 0) return false;

  uint8_t *mimeBuf = (uint8_t *)malloc(typeLen);
  if (!mimeBuf) return false;
  bool ok = optReadBytes(uid, OPT_NDEF_BLOCK, typeStringOffset, mimeBuf, typeLen);
  if (ok) ok = (memcmp(mimeBuf, OPT_MIME_TYPE, typeLen) == 0);
  free(mimeBuf);
  if (!ok) return false;

  f.tlvHeaderLen = tlvHeaderLen;
  f.recordHeaderLen = recordHeaderLen;
  f.payloadLen = payloadLen;
  f.payloadByteOffset = tlvHeaderLen + recordHeaderLen;
  return true;
}

// Probes for an OPT tag: CC must look like a Type-5 NDEF tag (same shape nfcvOpenSpool
// uses), then the NDEF TLV's record must carry MIME application/vnd.openprinttag. Does
// NOT require the record to be first in the message (spec allows others before it) --
// but OctoScale only ever writes a single record, so only that shape is parsed; a tag
// with multiple NDEF records is treated as "not an OPT tag OctoScale wrote", which is
// correct (reading a foreign multi-record tag is not a goal here).
inline bool optDetect(const uint8_t uid[8], OptLayout &layout) {
  uint8_t cc[4];
  if (!pn5180NfcvReadBlock(uid, OPT_CC_BLOCK, cc)) return false;
  if (cc[0] != 0xE1) return false;  // no valid Type-5 CC -> not an NDEF tag at all

  OptFraming f;
  if (!optReadFraming(uid, f)) return false;

  layout.ndefPayloadStartBlock = OPT_NDEF_BLOCK;
  layout.metaOffset = 0;
  layout.metaSize = OPT_META_SIZE;
  if (layout.metaSize > f.payloadLen) return false;  // payload too small to even hold Meta

  uint8_t metaBuf[OPT_META_SIZE];
  if (!optReadBytes(uid, OPT_NDEF_BLOCK, f.payloadByteOffset, metaBuf, OPT_META_SIZE)) return false;
  if (!optParseMeta(metaBuf, OPT_META_SIZE, layout)) return false;

  // Resolve "until next region/payload end" sizes now that all offsets are known.
  int payloadEnd = f.payloadLen;
  if (layout.mainSize < 0) {
    layout.mainSize = ((layout.auxOffset >= 0) ? layout.auxOffset : payloadEnd) - layout.mainOffset;
  }
  if (layout.auxOffset >= 0 && layout.auxSize < 0) layout.auxSize = payloadEnd - layout.auxOffset;
  if (layout.mainOffset < 0 || layout.mainSize < 0 || layout.mainOffset + layout.mainSize > payloadEnd) return false;

  // Store offsets as absolute byte offsets from OPT_NDEF_BLOCK (payloadByteOffset +
  // region-relative-to-payload offset) so callers don't need to re-derive this later.
  layout.mainOffset += f.payloadByteOffset;
  if (layout.auxOffset >= 0) layout.auxOffset += f.payloadByteOffset;
  layout.metaOffset += f.payloadByteOffset;
  return true;
}

// --- Read --------------------------------------------------------------------
inline bool optRead(const uint8_t uid[8], SpoolTagData &out) {
  out = SpoolTagData();
  OptLayout layout;
  if (!optDetect(uid, layout)) return false;
  if (layout.mainSize <= 0 || layout.mainSize > 512) return false;

  uint8_t *buf = (uint8_t *)malloc(layout.mainSize);
  if (!buf) return false;
  bool ok = optReadBytes(uid, layout.ndefPayloadStartBlock, layout.mainOffset, buf, layout.mainSize);
  if (!ok) { free(buf); return false; }

  int pos = 0;
  CborHeader mh;
  if (!cborReadHeader(buf, pos, layout.mainSize, mh) || mh.majorType != CBOR_MT_MAP) { free(buf); return false; }

  bool haveType = false, haveClass = false;
  for (uint64_t i = 0; i < mh.arg; i++) {
    CborHeader kh;
    if (!cborReadHeader(buf, pos, layout.mainSize, kh)) break;
    int64_t key;
    if (!cborHeaderToInt(kh, key)) { CborHeader dummy = kh; if (!cborSkipValue(buf, pos, layout.mainSize, dummy)) break; continue; }
    CborHeader vh;
    if (!cborReadHeader(buf, pos, layout.mainSize, vh)) break;
    switch (key) {
      case OPT_K_MATERIAL_CLASS: { int64_t v; if (cborHeaderToInt(vh, v)) haveClass = true; else cborSkipValue(buf, pos, layout.mainSize, vh); break; }
      case OPT_K_MATERIAL_TYPE: { int64_t v; if (cborHeaderToInt(vh, v) && v >= 0 && v < OPT_MATERIAL_TYPE_COUNT) {
            out.material = OPT_MATERIAL_TYPES[v]; haveType = true;
          } else cborSkipValue(buf, pos, layout.mainSize, vh); break; }
      case OPT_K_MATERIAL_NAME: { String s; if (cborReadTextBody(buf, pos, layout.mainSize, vh.arg, s)) { if (!haveType) out.material = s; } break; }
      case OPT_K_BRAND_NAME: { String s; if (cborReadTextBody(buf, pos, layout.mainSize, vh.arg, s)) out.vendor = s; break; }
      case OPT_K_NOMINAL_FULL_WEIGHT: { float f; int64_t iv;
          if (cborReadFloat(buf, pos, layout.mainSize, vh.additionalInfo, f)) out.totalWeight = f;
          else if (cborHeaderToInt(vh, iv)) out.totalWeight = (float)iv;
          break; }
      case OPT_K_EMPTY_CONTAINER_WEIGHT: { float f; int64_t iv;
          if (cborReadFloat(buf, pos, layout.mainSize, vh.additionalInfo, f)) out.spoolWeight = f;
          else if (cborHeaderToInt(vh, iv)) out.spoolWeight = (float)iv;
          break; }
      case OPT_K_PRIMARY_COLOR: {
          if (vh.majorType == CBOR_MT_BYTES && vh.arg >= 3 && pos + (int)vh.arg <= layout.mainSize) {
            char cbuf[8]; snprintf(cbuf, sizeof(cbuf), "#%02X%02X%02X", buf[pos], buf[pos+1], buf[pos+2]);
            out.color = cbuf;
            pos += (int)vh.arg;
          } else cborSkipValue(buf, pos, layout.mainSize, vh);
          break; }
      case OPT_K_DENSITY: { float f; if (cborReadFloat(buf, pos, layout.mainSize, vh.additionalInfo, f)) out.density = f; break; }
      case OPT_K_FILAMENT_DIAMETER: { float f; if (cborReadFloat(buf, pos, layout.mainSize, vh.additionalInfo, f)) out.diameter = f; break; }
      case OPT_K_MIN_PRINT_TEMPERATURE: { int64_t v; if (cborHeaderToInt(vh, v)) out.temperatureMin = (int)v; break; }
      case OPT_K_MAX_PRINT_TEMPERATURE: { int64_t v; if (cborHeaderToInt(vh, v)) out.temperatureMax = (int)v; break; }
      case OPT_K_PREHEAT_TEMPERATURE: { int64_t v; if (cborHeaderToInt(vh, v)) out.temperature = (int)v; break; }
      case OPT_K_MIN_BED_TEMPERATURE: { int64_t v; if (cborHeaderToInt(vh, v)) out.bedTemperatureMin = (int)v; break; }
      case OPT_K_MAX_BED_TEMPERATURE: { int64_t v; if (cborHeaderToInt(vh, v)) out.bedTemperatureMax = (int)v; break; }
      case OPT_K_DRYING_TEMPERATURE: { int64_t v; if (cborHeaderToInt(vh, v)) out.dryingTemperature = (int)v; break; }
      case OPT_K_DRYING_TIME: { int64_t v; if (cborHeaderToInt(vh, v)) out.dryingTime = (int)v; break; }  // minutes
      case OPT_K_TRANSMISSION_DISTANCE: { float f; if (cborReadFloat(buf, pos, layout.mainSize, vh.additionalInfo, f)) out.td = f; break; }
      default: cborSkipValue(buf, pos, layout.mainSize, vh); break;
    }
  }
  free(buf);
  // material_class is required by the spec -- its absence means this isn't really a
  // conforming OPT Main region, even though the NDEF/CC framing checked out.
  return haveClass;
}

// --- Initialize (blank tag -> OPT structure) --------------------------------------
// Only ever called on a tag whose CC is virgin (all-zero) -- same discipline as every
// other write in this project (Mifare/NTAG/nfcvOpenSpool all refuse to touch a CC that
// isn't virgin or already their own format). Lays out Meta(8B)+Main(rest)+Aux(32B,
// block-aligned at the end), matching utils/nfc_initialize.py's own scheme.
inline bool optInitialize(const uint8_t uid[8], int tagUserBytes, OptLayout &layoutOut, String &errOut) {
  uint8_t cc[4];
  if (!pn5180NfcvReadBlock(uid, OPT_CC_BLOCK, cc)) { errOut = "CC read failed"; return false; }
  bool alreadyNdef;
  bool virgin = pn5180NfcvCcIsVirgin(cc, alreadyNdef);
  if (!virgin) {
    if (!alreadyNdef) {
      errOut = "tag's Capability Container is neither virgin nor a recognized NDEF CC -- refusing to write";
      return false;
    }
    // CC is a valid NDEF CC (0xE1) but that alone doesn't mean there's an actual NDEF
    // message underneath -- pn5180EraseTag deliberately preserves this CC (it's OTP,
    // see pn5180NfcvCcIsVirgin's comment) while zeroing the TLV/NDEF content past it,
    // so a freshly erased tag looks exactly like this: CC set, no TLV. Distinguish the
    // two cases by checking for an actual NDEF TLV marker (0x03) at the start of the
    // message area -- if it's not there, this CC is safe to build a fresh OPT layout
    // under, same as a virgin tag.
    uint8_t tlvTag;
    if (!optReadBytes(uid, OPT_NDEF_BLOCK, 0, &tlvTag, 1)) { errOut = "TLV read failed"; return false; }
    if (tlvTag == 0x03) {
      errOut = "tag already carries NDEF data -- use optWrite to update an existing OPT tag, not optInitialize";
      return false;
    }
  }

  int mimeLen = (int)strlen(OPT_MIME_TYPE);
  // Try the short TLV+record forms first (matches every other NDEF writer in this
  // project); fall back to long forms only if the payload doesn't fit the short
  // forms' 254/255 B ceilings. A tag has to be considerably larger than the smallest
  // ones this project targets before that happens (payload > ~220 B once Meta/Aux
  // overhead is subtracted), but OpenPrintTag's whole point on bigger NFC-V tags is to
  // carry more than nfcvOpenSpool's JSON ever does, so this can't just assume short
  // forms and reject anything that overflows them the way nfcvOpenSpool does.
  for (int form = 0; form < 2; form++) {
    bool longForm = (form == 1);
    int tlvHeaderLen = longForm ? 4 : 2;
    int recordHeaderLen = (longForm ? 6 : 3) + mimeLen;
    int payloadBudget = tagUserBytes - 4 /* CC */ - tlvHeaderLen - recordHeaderLen - 1 /* TLV terminator */;
    int mainSize = payloadBudget - OPT_META_SIZE - OPT_AUX_SIZE;
    if (mainSize < 16) { if (longForm) { errOut = "tag too small for an OpenPrintTag Main region"; return false; } continue; }
    int payloadLen = OPT_META_SIZE + mainSize + OPT_AUX_SIZE;
    // Short record payload must fit a uint8; short TLV length must be < 255 (0xFF is
    // reserved as the long-TLV marker) and the record itself must fit inside it too.
    int msgLen = recordHeaderLen + payloadLen;
    if (!longForm && (payloadLen > 255 || msgLen >= 255)) continue;  // retry with long forms

    int payloadByteOffset = tlvHeaderLen + recordHeaderLen;
    layoutOut.ndefPayloadStartBlock = OPT_NDEF_BLOCK;
    layoutOut.metaOffset = payloadByteOffset;
    layoutOut.metaSize = OPT_META_SIZE;
    layoutOut.mainOffset = payloadByteOffset + OPT_META_SIZE;
    layoutOut.mainSize = mainSize;
    layoutOut.auxOffset = payloadByteOffset + OPT_META_SIZE + mainSize;
    layoutOut.auxSize = OPT_AUX_SIZE;

    // Build the full record+payload in one buffer (Meta with real offsets, Main and
    // Aux as empty maps) -- length-then-message discipline like every other NDEF
    // write here: TLV length is written as 0x00 (short) or 0/0 (long) first, patched
    // to the real value LAST, so an interrupted write reads back as an empty
    // (harmless) NDEF message.
    int totalLen = ((tlvHeaderLen + msgLen + 1 + 3) / 4) * 4;  // + TLV terminator, padded to a block
    uint8_t *buf = (uint8_t *)calloc(totalLen, 1);
    if (!buf) { errOut = "out of memory"; return false; }
    int p = 0;
    buf[p++] = 0x03;  // TLV tag = NDEF message
    if (longForm) { buf[p++] = 0xFF; buf[p++] = 0x00; buf[p++] = 0x00; }  // long-form length placeholder
    else buf[p++] = 0x00;  // short-form length placeholder
    buf[p++] = (uint8_t)(longForm ? 0xC2 : 0xD2);  // MB|ME|[SR]|TNF=MIME
    buf[p++] = (uint8_t)mimeLen;
    if (longForm) {
      buf[p++] = (uint8_t)(payloadLen >> 24); buf[p++] = (uint8_t)(payloadLen >> 16);
      buf[p++] = (uint8_t)(payloadLen >> 8);  buf[p++] = (uint8_t)payloadLen;
    } else {
      buf[p++] = (uint8_t)payloadLen;
    }
    memcpy(buf + p, OPT_MIME_TYPE, mimeLen); p += mimeLen;
    int cap = totalLen;

    // Meta at p: {0: mainOffset-rel-to-payload, 1: mainSize, 2: auxOffset-rel, 3: auxSize}.
    int metaStart = p;
    int metaPos = p;
    bool ok = cborWriteMapHeader(buf, metaPos, cap, 4);
    ok = ok && cborWriteUint(buf, metaPos, cap, 0) && cborWriteUint(buf, metaPos, cap, (uint64_t)OPT_META_SIZE);
    ok = ok && cborWriteUint(buf, metaPos, cap, 1) && cborWriteUint(buf, metaPos, cap, (uint64_t)mainSize);
    ok = ok && cborWriteUint(buf, metaPos, cap, 2) && cborWriteUint(buf, metaPos, cap, (uint64_t)(OPT_META_SIZE + mainSize));
    ok = ok && cborWriteUint(buf, metaPos, cap, 3) && cborWriteUint(buf, metaPos, cap, (uint64_t)OPT_AUX_SIZE);
    // Should not happen with OPT_META_SIZE=24 (worst case needs 17 B, see its own
    // comment) -- kept as a hard failure rather than falling through to the generic
    // "tag too small" message below, which would misdescribe an internal sizing bug
    // as a capacity problem.
    if (!ok || metaPos - metaStart > OPT_META_SIZE) { free(buf); errOut = "internal error: meta region encoding exceeded its reserved size"; return false; }
    p = metaStart + OPT_META_SIZE;  // Meta is fixed-size regardless of how much cborWrite* used

    // Main: empty map (optWrite's field-write pass fills it in afterward).
    int mainPos = p;
    ok = cborWriteMapHeader(buf, mainPos, cap, 0);
    if (!ok) { free(buf); errOut = "internal error writing empty main map"; return false; }
    p += mainSize;

    // Aux: empty map (OctoScale never populates it).
    int auxPos = p;
    ok = cborWriteMapHeader(buf, auxPos, cap, 0);
    if (!ok) { free(buf); errOut = "internal error writing empty aux map"; return false; }
    p += OPT_AUX_SIZE;

    buf[p] = 0xFE;  // TLV terminator

    bool wrote = true;
    if (virgin) {
      uint8_t newCc[4] = { 0xE1, 0x40, (uint8_t)(tagUserBytes / 8), 0x00 };  // MLEN in 8-byte units, same shape as nfcvOpenSpool's CC
      wrote = pn5180NfcvWriteBlock(uid, OPT_CC_BLOCK, newCc, errOut);
    }
    for (int i = 0; wrote && i < totalLen / 4; i++) {
      uint8_t blk[4]; memcpy(blk, buf + i * 4, 4);
      wrote = pn5180NfcvWriteBlock(uid, (uint8_t)(OPT_NDEF_BLOCK + i), blk, errOut);
    }
    if (wrote) {
      // Patch the real TLV/record length in LAST -- the commit step (matches
      // nfcvOpenSpool). Both length fields live within the first 2 blocks (8 bytes)
      // for any typeLen this project uses (OPT_MIME_TYPE is fixed), so re-reading and
      // patching those blocks covers both forms.
      uint8_t lenBlocks[8];
      if (pn5180NfcvReadBlock(uid, OPT_NDEF_BLOCK, lenBlocks) &&
          pn5180NfcvReadBlock(uid, OPT_NDEF_BLOCK + 1, lenBlocks + 4)) {
        if (longForm) { lenBlocks[2] = (uint8_t)(msgLen >> 8); lenBlocks[3] = (uint8_t)msgLen; }
        else lenBlocks[1] = (uint8_t)msgLen;
        wrote = pn5180NfcvWriteBlock(uid, OPT_NDEF_BLOCK, lenBlocks, errOut) &&
                pn5180NfcvWriteBlock(uid, OPT_NDEF_BLOCK + 1, lenBlocks + 4, errOut);
      } else { wrote = false; errOut = "length-patch read failed"; }
    }
    free(buf);
    return wrote;
  }
  errOut = "tag too small for an OpenPrintTag Main region";
  return false;
}

// --- Write --------------------------------------------------------------------
// Encodes SpoolTagData into the Main region's CBOR map and writes it. If the tag isn't
// already an OPT tag, initializes it first (optInitialize). Unlike OpenSpool's
// progressive field-dropping, a Main region that doesn't fit is a hard failure -- for a
// foreign, spec-defined format, a partially-populated tag is worse than a clean error
// (see the plan's reasoning: no silent, format-specific truncation of a standard other
// tools also read).
// Encodes the spool's CBOR map into `buf` (capacity `cap`), reporting the number of
// bytes used in posOut. Split out of optWrite so the capacity can be checked against a
// scratch buffer BEFORE anything is written to the tag -- see optWrite's comment.
inline bool optEncodeMain(const SpoolTagData &d, uint8_t *buf, int cap, int &posOut) {
  uint8_t rgb[3] = {0, 0, 0};
  bool haveColor = pn5180PrimaryColorRgb(d, rgb);  // grammar-aware, see its comment
  int typeIdx = optMaterialTypeIndex(d.material);

  // Count fields that will actually be present, matching the presence checks below --
  // the map header's count must equal exactly how many key/value pairs follow.
  int n = 1;  // material_class is always written (required by the spec)
  if (typeIdx >= 0) n++;
  if (d.material.length() > 0) n++;
  if (d.vendor.length() > 0) n++;
  if (d.totalWeight >= 0) n++;
  if (d.spoolWeight >= 0) n++;
  if (haveColor) n++;
  if (d.density >= 0) n++;
  if (d.diameter >= 0) n++;
  // SpoolTagData's sentinel for "not set" is -1 (see its own field comments), not
  // some physically-impossible temperature -- -1 deg C is itself a value SpoolManagerExtended
  // could legitimately send (e.g. a cold-chamber material), so this can't distinguish
  // "unset" from "explicitly minus one" any better than the rest of the codebase does;
  // it's the same convention every other write path in pn5180nfc.h already relies on.
  if (d.temperatureMin >= 0) n++;
  if (d.temperatureMax >= 0) n++;
  if (d.temperature >= 0) n++;
  if (d.bedTemperatureMin >= 0) n++;
  if (d.bedTemperatureMax >= 0) n++;

  int pos = 0;
  bool ok = cborWriteMapHeader(buf, pos, cap, (uint32_t)n);
  ok = ok && cborWriteUint(buf, pos, cap, OPT_K_MATERIAL_CLASS) && cborWriteUint(buf, pos, cap, OPT_MATERIAL_CLASS_FFF);
  if (ok && typeIdx >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_MATERIAL_TYPE) && cborWriteUint(buf, pos, cap, (uint64_t)typeIdx);
  if (ok && d.material.length() > 0) ok = cborWriteUint(buf, pos, cap, OPT_K_MATERIAL_NAME) && cborWriteText(buf, pos, cap, pn5180Utf8SafeTruncate(d.material, 63));
  if (ok && d.vendor.length() > 0) ok = cborWriteUint(buf, pos, cap, OPT_K_BRAND_NAME) && cborWriteText(buf, pos, cap, pn5180Utf8SafeTruncate(d.vendor, 31));
  if (ok && d.totalWeight >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_NOMINAL_FULL_WEIGHT) && cborWriteFloat(buf, pos, cap, d.totalWeight);
  if (ok && d.spoolWeight >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_EMPTY_CONTAINER_WEIGHT) && cborWriteFloat(buf, pos, cap, d.spoolWeight);
  if (ok && haveColor) ok = cborWriteUint(buf, pos, cap, OPT_K_PRIMARY_COLOR) && cborWriteBytes(buf, pos, cap, rgb, 3);
  if (ok && d.density >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_DENSITY) && cborWriteFloat(buf, pos, cap, d.density);
  if (ok && d.diameter >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_FILAMENT_DIAMETER) && cborWriteFloat(buf, pos, cap, d.diameter);
  if (ok && d.temperatureMin >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_MIN_PRINT_TEMPERATURE) && cborWriteInt(buf, pos, cap, d.temperatureMin);
  if (ok && d.temperatureMax >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_MAX_PRINT_TEMPERATURE) && cborWriteInt(buf, pos, cap, d.temperatureMax);
  if (ok && d.temperature >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_PREHEAT_TEMPERATURE) && cborWriteInt(buf, pos, cap, d.temperature);
  if (ok && d.bedTemperatureMin >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_MIN_BED_TEMPERATURE) && cborWriteInt(buf, pos, cap, d.bedTemperatureMin);
  if (ok && d.bedTemperatureMax >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_MAX_BED_TEMPERATURE) && cborWriteInt(buf, pos, cap, d.bedTemperatureMax);
  // Drying + opacity. >= 0 rather than > 0 deliberately: 0 is a legitimate drying value
  // ("do not dry"), and the -1 sentinel is what means "the user never set this". Writing
  // an unset field as 0 would put "dry at 0 °C" on the tag as if it were a real spec.
  if (ok && d.dryingTemperature >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_DRYING_TEMPERATURE) && cborWriteInt(buf, pos, cap, d.dryingTemperature);
  if (ok && d.dryingTime >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_DRYING_TIME) && cborWriteInt(buf, pos, cap, d.dryingTime);
  if (ok && d.td >= 0) ok = cborWriteUint(buf, pos, cap, OPT_K_TRANSMISSION_DISTANCE) && cborWriteFloat(buf, pos, cap, d.td);

  posOut = pos;
  return ok;
}

// Computes the Main-region size optInitialize WOULD lay out on a tag of this size,
// without touching the tag. Same two-pass short/long-form walk as optInitialize itself.
// Returns 0 when no layout fits at all.
inline int optPlannedMainSize(int tagUserBytes) {
  int mimeLen = (int)strlen(OPT_MIME_TYPE);
  for (int form = 0; form < 2; form++) {
    bool longForm = (form == 1);
    int tlvHeaderLen = longForm ? 4 : 2;
    int recordHeaderLen = (longForm ? 6 : 3) + mimeLen;
    int payloadBudget = tagUserBytes - 4 - tlvHeaderLen - recordHeaderLen - 1;
    int mainSize = payloadBudget - OPT_META_SIZE - OPT_AUX_SIZE;
    if (mainSize < 16) continue;
    int payloadLen = OPT_META_SIZE + mainSize + OPT_AUX_SIZE;
    int msgLen = recordHeaderLen + payloadLen;
    if (!longForm && (payloadLen > 255 || msgLen >= 255)) continue;
    return mainSize;
  }
  return 0;
}

inline bool optEncodeMain(const SpoolTagData &d, uint8_t *buf, int cap, int &posOut);

// How many bytes this spool's CBOR map actually needs. Encodes into a scratch buffer,
// so it answers the real question ("does THIS data fit") rather than an estimate.
// Returns -1 if it doesn't even fit the scratch buffer (far larger than any tag here).
inline int optRequiredMainBytes(const SpoolTagData &d) {
  static const int SCRATCH = 1024;
  uint8_t *tmp = (uint8_t *)calloc(SCRATCH, 1);
  if (!tmp) return -1;
  int pos = 0;
  bool ok = optEncodeMain(d, tmp, SCRATCH, pos);
  free(tmp);
  return ok ? pos : -1;
}

inline bool optWrite(const uint8_t uid[8], const SpoolTagData &d, String &errOut,
                     int *bytesWrittenOut = nullptr,
                     int *capAvailOut = nullptr, int *capNeededOut = nullptr) {
  errOut = "";
  if (bytesWrittenOut) *bytesWrittenOut = 0;
  OptLayout layout;
  if (!optDetect(uid, layout)) {
    // Not (yet) an OPT tag -- try to lay one out. optInitialize itself checks the CC
    // is virgin and refuses anything else (foreign non-OPT NDEF, unrelated data).
    int userBytes = pn5180NfcvUserBytes(uid);
    // Capacity check BEFORE optInitialize, not after. optInitialize writes the CC, the
    // NDEF/record header and empty Meta/Main/Aux maps onto the tag; only afterwards did
    // the CBOR encoding below discover the data didn't fit. The write then failed with a
    // clean error, but the tag was left carrying a half-built OpenPrintTag structure --
    // enough for occupancy to read it back as "foreign" and for the TFT to warn about
    // overwriting somebody else's tag, on a tag OctoScale itself had just written.
    // Checking first means a tag that cannot hold the data is left exactly as it was.
    int planned = optPlannedMainSize(userBytes);
    int needed = optRequiredMainBytes(d);
    if (capAvailOut) *capAvailOut = planned;
    if (capNeededOut) *capNeededOut = needed;
    if (planned <= 0 || (needed >= 0 && needed > planned)) {
      errOut = "spool data does not fit into this tag's OpenPrintTag Main region (" +
               String(planned) + " B available, " + String(needed) + " B needed) -- "
               "use a larger NFC-V tag or the Extended format";
      return false;
    }
    if (!optInitialize(uid, userBytes, layout, errOut)) return false;
  }

  uint8_t *buf = (uint8_t *)calloc(layout.mainSize, 1);
  if (!buf) { errOut = "out of memory"; return false; }
  int pos = 0;
  bool ok = optEncodeMain(d, buf, layout.mainSize, pos);
  if (!ok) {
    free(buf);
    // Reached only for a tag that was ALREADY an OPT tag (optDetect succeeded, so the
    // pre-flight check above was skipped). Nothing has been written at this point --
    // the encode happens entirely in RAM -- so the tag keeps its previous contents.
    if (capAvailOut) *capAvailOut = layout.mainSize;
    if (capNeededOut) *capNeededOut = optRequiredMainBytes(d);
    errOut = "spool data does not fit into this tag's OpenPrintTag Main region (" +
             String(layout.mainSize) + " B) -- use a larger NFC-V tag or the Extended format";
    return false;
  }

  // Zero the rest of the region (matches the reference record.py: "write zeroes to the
  // whole region, then write the encoded data" -- any bytes left over from a longer
  // previous write must not linger as trailing garbage after a shorter one).
  bool wrote = optWriteBytes(uid, layout.ndefPayloadStartBlock, layout.mainOffset, buf, layout.mainSize, errOut);
  if (wrote && bytesWrittenOut) *bytesWrittenOut = pos;  // actual CBOR-encoded bytes, not the whole (zero-padded) region
  free(buf);
  return wrote;
}

// --- Dispatch wrapper: OPT-aware pn5180WriteSpoolTag -----------------------------
// Mirrors pn5180WriteSpoolTag's own signature exactly, adding "openPrintTag" as a
// third nfcvFormat value (alongside "extended"/"openSpool"). Lives here rather than
// as a fourth case inside pn5180WriteSpoolTag itself because that function is defined
// in pn5180nfc.h, which openprinttag.h depends on (for SpoolTagData and the NFC-V
// block primitives) -- pn5180nfc.h can't depend back on openprinttag.h without a
// circular include, so the OPT branch is intercepted here, one layer up, instead.
// Only NFC-V tags can carry OPT (the format requires NFC-V, see openprinttag.h's top
// comment) -- Mifare/NTAG requests with nfcvFormat="openPrintTag" are silently treated
// as "not NFC-V, ignore the hint" and fall through to pn5180WriteSpoolTag's normal
// per-tag-type dispatch, same as an NFC-V-only preference already does for those tags.
inline bool pn5180WriteSpoolTagOpt(const SpoolTagData &d, String &formatOut,
                                   int &bytesWrittenOut, String &droppedFieldsOut, String &errOut,
                                   const String &nfcvFormat = "extended",
                                   const String &ntagFormat = "openSpool",
                                   int *capAvailOut = nullptr, int *capNeededOut = nullptr) {
  formatOut = ""; bytesWrittenOut = 0; droppedFieldsOut = ""; errOut = "";
  if (nfcvFormat == "openPrintTag") {
    String uidHex;
    uint8_t nfcvUid[8];
    PN5180WriteKind kind = pn5180ProbeWriteKind(uidHex, nfcvUid);
    if (kind == PN5180_WK_NFCV) {
      formatOut = "nfcvOpenPrintTag";
      // No legacy anchor write here -- OPT tags carry no OctoScale databaseId at all
      // (see this file's top comment); the load flow falls back to a UID lookup for
      // these tags, exactly like NTAG/NFC-V's own openSpool already does for the same
      // reason (NDEF occupies the blocks the legacy anchor would otherwise use).
      bool ok = optWrite(nfcvUid, d, errOut, &bytesWrittenOut,
                         capAvailOut, capNeededOut);  // reports the CBOR-encoded Main region size
      return ok;
    }
    // Not NFC-V -- fall through to the normal per-tag-type dispatch below, which will
    // pick octoscaleExtended (Mifare) or openSpool/ntagExtended (NTAG) as appropriate.
  }
  return pn5180WriteSpoolTag(d, formatOut, bytesWrittenOut, droppedFieldsOut, errOut, nfcvFormat, ntagFormat);
}

// --- Dispatch wrapper: OPT-aware pn5180ReadSpoolEx --------------------------------
// Mirrors pn5180ReadSpoolEx's signature. Tries optRead ONLY for NFC-V tags that
// nfcvExtended/nfcvOpenSpool didn't already recognize (pn5180ReadSpoolEx itself tries
// both of those first and sets hasExtended=true on success) -- OPT occupies the same
// block range those formats do (block 0 = CC in nfcvOpenSpool's scheme too), so it can
// only be tried once the other two have both declined, not run unconditionally
// alongside them the way pn5180ReadSpoolEx tries its own two candidates.
inline bool pn5180ReadSpoolExOpt(PN5180TagType &typeOut, long &idOut, String &uidHexOut,
                                 String &textOut, SpoolTagData &data, bool &hasExtended,
                                 String *formatOut = nullptr, int *capacityBytesOut = nullptr) {
  bool ok = pn5180ReadSpoolEx(typeOut, idOut, uidHexOut, textOut, data, hasExtended, formatOut, capacityBytesOut);
  if (!ok || hasExtended || typeOut != PN5180_TAG_NFCV) return ok;

  uint8_t rawUid[8] = {0};
  if (g_pn5180->getInventory(rawUid) != ISO15693_EC_OK) return ok;
  if (optRead(rawUid, data)) {
    hasExtended = true;
    // OPT carries no OctoScale databaseId -- idOut stays whatever pn5180ReadSpool
    // already found (typically -1 for a foreign tag), matching how nfcvOpenSpool/
    // openSpool leave idOut alone when their own JSON has no os_db_id either.
    if (formatOut) *formatOut = "nfcvOpenPrintTag";
  }
  g_pn5180->reset(); g_pn5180->setupRF();
  return ok;
}
