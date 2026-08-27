#pragma once
#include <Arduino.h>

// cbor.h — minimal CBOR (RFC 8949) encoder/decoder, just enough for OpenPrintTag's
// Main/Aux region maps (openprinttag.h): unsigned/negative integers, float32, text
// strings, byte strings, and fixed-length maps. No indefinite-length items, no arrays,
// no tags -- OpenPrintTag doesn't need them for the fields OctoScale writes, and the
// reference tooling only recommends indefinite-length maps for Aux, which OctoScale
// doesn't populate. No third-party dependency: the encoding is a handful of straight
// bit-packing rules, not worth pulling in a library for.
//
// All functions take a fixed-size buffer + a running position (like pn5180nfc.h's own
// block-writing style) and fail closed: encoding stops and returns false the moment a
// write would overrun the buffer, so a too-small region is caught immediately rather
// than corrupting whatever follows it in NFC memory.

// Major types (top 3 bits of the initial byte), per RFC 8949 §3.
enum CborMajorType {
  CBOR_MT_UINT     = 0,  // unsigned integer
  CBOR_MT_NEGINT   = 1,  // negative integer (encoded as -1-n)
  CBOR_MT_BYTES    = 2,  // byte string
  CBOR_MT_TEXT     = 3,  // UTF-8 text string
  CBOR_MT_ARRAY    = 4,  // not used by OctoScale's OPT writer/reader
  CBOR_MT_MAP      = 5,  // map (key/value pairs)
  CBOR_MT_TAG      = 6,  // not used
  CBOR_MT_SIMPLE   = 7,  // simple/float/bool/null; only float32 (0xFA) used here
};

// --- Encoding ----------------------------------------------------------------
// Each writer appends at buf[pos], advances pos, and returns false (pos left
// unchanged... actually pos is advanced up to the point of failure -- callers must
// treat any false return as "encoding aborted, buffer contents from here on are
// undefined" and not attempt to use pos afterward) if it would exceed cap.

// Writes a major-type + argument header: for arg < 24 it's a single byte: (type<<5)|arg.
// For larger values, 1/2/4/8-byte length-extension per RFC 8949 -- OctoScale only ever
// needs the 1-byte (uint8) and 2-byte (uint16) extensions in practice (field counts,
// string lengths, and OPT's own integer fields all fit), but all four are implemented
// for correctness/future fields.
inline bool cborWriteHeader(uint8_t *buf, int &pos, int cap, uint8_t majorType, uint64_t arg) {
  uint8_t mt = (uint8_t)(majorType << 5);
  if (arg < 24) {
    if (pos + 1 > cap) return false;
    buf[pos++] = mt | (uint8_t)arg;
  } else if (arg <= 0xFF) {
    if (pos + 2 > cap) return false;
    buf[pos++] = mt | 24;
    buf[pos++] = (uint8_t)arg;
  } else if (arg <= 0xFFFF) {
    if (pos + 3 > cap) return false;
    buf[pos++] = mt | 25;
    buf[pos++] = (uint8_t)(arg >> 8);
    buf[pos++] = (uint8_t)arg;
  } else if (arg <= 0xFFFFFFFFUL) {
    if (pos + 5 > cap) return false;
    buf[pos++] = mt | 26;
    buf[pos++] = (uint8_t)(arg >> 24);
    buf[pos++] = (uint8_t)(arg >> 16);
    buf[pos++] = (uint8_t)(arg >> 8);
    buf[pos++] = (uint8_t)arg;
  } else {
    if (pos + 9 > cap) return false;
    buf[pos++] = mt | 27;
    for (int shift = 56; shift >= 0; shift -= 8) buf[pos++] = (uint8_t)(arg >> shift);
  }
  return true;
}

// Signed integer -- CBOR has separate major types for >=0 (uint) and <0 (negint,
// encoded as -1-n so -1 -> n=0, -2 -> n=1, ...). OpenPrintTag's temperature fields are
// the only signed values OctoScale writes (int, can't go below CBOR's negint range for
// any realistic temperature).
inline bool cborWriteInt(uint8_t *buf, int &pos, int cap, int64_t v) {
  if (v >= 0) return cborWriteHeader(buf, pos, cap, CBOR_MT_UINT, (uint64_t)v);
  return cborWriteHeader(buf, pos, cap, CBOR_MT_NEGINT, (uint64_t)(-1 - v));
}

inline bool cborWriteUint(uint8_t *buf, int &pos, int cap, uint64_t v) {
  return cborWriteHeader(buf, pos, cap, CBOR_MT_UINT, v);
}

// float32 (major type 7, additional info 26 = 0xFA), big-endian IEEE 754 bit pattern.
// OpenPrintTag's `number` fields (density, diameter, weights) are specified as plain
// numbers with no fixed scale, so a float is the natural fit here -- unlike OctoScale's
// own binary formats, which scale to a fixed-point uint16 to save bytes.
inline bool cborWriteFloat(uint8_t *buf, int &pos, int cap, float v) {
  if (pos + 5 > cap) return false;
  uint32_t bits;
  memcpy(&bits, &v, 4);
  buf[pos++] = 0xFA;
  buf[pos++] = (uint8_t)(bits >> 24);
  buf[pos++] = (uint8_t)(bits >> 16);
  buf[pos++] = (uint8_t)(bits >> 8);
  buf[pos++] = (uint8_t)bits;
  return true;
}

inline bool cborWriteText(uint8_t *buf, int &pos, int cap, const String &s) {
  if (!cborWriteHeader(buf, pos, cap, CBOR_MT_TEXT, (uint64_t)s.length())) return false;
  if (pos + (int)s.length() > cap) return false;
  for (int i = 0; i < (int)s.length(); i++) buf[pos++] = (uint8_t)s[i];
  return true;
}

inline bool cborWriteBytes(uint8_t *buf, int &pos, int cap, const uint8_t *data, int len) {
  if (!cborWriteHeader(buf, pos, cap, CBOR_MT_BYTES, (uint64_t)len)) return false;
  if (pos + len > cap) return false;
  memcpy(buf + pos, data, len);
  pos += len;
  return true;
}

// Fixed-length map header ONLY (definite-length, per RFC 8949 -- the reference
// implementation uses these for Main; indefinite-length is only recommended for Aux,
// which OctoScale's writer never populates). Caller writes exactly n key/value pairs
// after this call -- key/value writers are the functions above, called in pairs.
inline bool cborWriteMapHeader(uint8_t *buf, int &pos, int cap, uint32_t n) {
  return cborWriteHeader(buf, pos, cap, CBOR_MT_MAP, n);
}

// --- Decoding ------------------------------------------------------------------
// Reads one item's header at buf[pos]: major type, and the raw "argument" (length for
// strings/maps, the value itself for [u]ints, the additional-info byte for floats/
// simple). Advances pos past the header. Returns false on truncation/malformed input.
struct CborHeader {
  uint8_t majorType;
  uint8_t additionalInfo;  // low 5 bits of the initial byte (needed to tell float32 from other simple values)
  uint64_t arg;
};

inline bool cborReadHeader(const uint8_t *buf, int &pos, int len, CborHeader &out) {
  if (pos + 1 > len) return false;
  uint8_t ib = buf[pos++];
  out.majorType = ib >> 5;
  out.additionalInfo = ib & 0x1F;
  // Major type 7 (simple/float) overloads additionalInfo 25/26/27 as the WIDTH of a
  // following float16/32/64, not an integer argument -- the bytes there are IEEE 754
  // bit patterns, not a length or a value to read as one. Reading them as arg here
  // (like every other major type does) would both misinterpret the bits AND advance
  // pos past them, leaving cborReadFloat nothing to read. So for type 7, arg is left
  // as additionalInfo itself and pos stops right after the initial byte -- the
  // specialized reader (cborReadFloat) consumes the payload bytes itself.
  if (out.majorType == CBOR_MT_SIMPLE) {
    out.arg = out.additionalInfo;
    return true;
  }
  if (out.additionalInfo < 24) {
    out.arg = out.additionalInfo;
  } else if (out.additionalInfo == 24) {
    if (pos + 1 > len) return false;
    out.arg = buf[pos++];
  } else if (out.additionalInfo == 25) {
    if (pos + 2 > len) return false;
    out.arg = ((uint64_t)buf[pos] << 8) | buf[pos + 1]; pos += 2;
  } else if (out.additionalInfo == 26) {
    if (pos + 4 > len) return false;
    out.arg = ((uint64_t)buf[pos] << 24) | ((uint64_t)buf[pos+1] << 16) | ((uint64_t)buf[pos+2] << 8) | buf[pos+3];
    pos += 4;
  } else if (out.additionalInfo == 27) {
    if (pos + 8 > len) return false;
    out.arg = 0;
    for (int i = 0; i < 8; i++) out.arg = (out.arg << 8) | buf[pos + i];
    pos += 8;
  } else {
    return false;  // additionalInfo 28-31: reserved/indefinite-length, not supported
  }
  return true;
}

// Reads a signed integer value from an already-parsed header (uint or negint major type
// only). Returns false for any other major type -- caller should treat that as "not the
// type I expected" and skip the item instead (see cborSkipValue below).
inline bool cborHeaderToInt(const CborHeader &h, int64_t &out) {
  if (h.majorType == CBOR_MT_UINT) { out = (int64_t)h.arg; return true; }
  if (h.majorType == CBOR_MT_NEGINT) { out = -1 - (int64_t)h.arg; return true; }
  return false;
}

// float32 only (additionalInfo 26 within major type 7) -- OpenPrintTag's number fields
// are always written as float32 by this codec, so that's the only float width the
// reader needs to handle for round-tripping OctoScale's own writes. Reading a tag
// written by someone else's float64 number would fail closed (returns false) rather
// than silently truncating -- caller skips the field, same as any other mismatch.
inline bool cborReadFloat(const uint8_t *buf, int &pos, int len, uint8_t additionalInfo, float &out) {
  if (additionalInfo != 26) return false;
  if (pos + 4 > len) return false;
  uint32_t bits = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16) | ((uint32_t)buf[pos+2] << 8) | buf[pos+3];
  pos += 4;
  memcpy(&out, &bits, 4);
  return true;
}

// Reads a text string's bytes into a String, given the header's arg as the byte length.
// pos must point right after the header (i.e. at the string bytes) on entry.
inline bool cborReadTextBody(const uint8_t *buf, int &pos, int len, uint64_t byteLen, String &out) {
  if (pos + (int)byteLen > len) return false;
  out = "";
  out.reserve((unsigned int)byteLen);
  for (uint64_t i = 0; i < byteLen; i++) out += (char)buf[pos + i];
  pos += (int)byteLen;
  return true;
}

// Skips a fully-parsed-header value's body (or, for containers, skips its members too).
// Used when a decoder finds an unexpected/unknown type or key -- must consume the exact
// number of bytes the value occupies, or every subsequent map entry misparses. Only
// definite-length containers are supported (matches what this codec ever writes);
// finding an indefinite-length item mid-stream is treated as unsupported/unparseable.
inline bool cborSkipValue(const uint8_t *buf, int &pos, int len, const CborHeader &h) {
  switch (h.majorType) {
    case CBOR_MT_UINT: case CBOR_MT_NEGINT:
      return true;  // value was entirely in the header (arg)
    case CBOR_MT_BYTES: case CBOR_MT_TEXT:
      if (pos + (int)h.arg > len) return false;
      pos += (int)h.arg;
      return true;
    case CBOR_MT_ARRAY: {
      for (uint64_t i = 0; i < h.arg; i++) {
        CborHeader eh;
        if (!cborReadHeader(buf, pos, len, eh)) return false;
        if (!cborSkipValue(buf, pos, len, eh)) return false;
      }
      return true;
    }
    case CBOR_MT_MAP: {
      for (uint64_t i = 0; i < h.arg; i++) {
        CborHeader kh, vh;
        if (!cborReadHeader(buf, pos, len, kh) || !cborSkipValue(buf, pos, len, kh)) return false;
        if (!cborReadHeader(buf, pos, len, vh) || !cborSkipValue(buf, pos, len, vh)) return false;
      }
      return true;
    }
    case CBOR_MT_SIMPLE:
      if (h.additionalInfo == 25) { pos += 2; return pos <= len; }  // float16
      if (h.additionalInfo == 26) { pos += 4; return pos <= len; }  // float32
      if (h.additionalInfo == 27) { pos += 8; return pos <= len; }  // float64
      return true;  // false/true/null/undefined -- value was entirely in the header
    default:
      return false;  // CBOR_MT_TAG or reserved -- not produced/expected here
  }
}
