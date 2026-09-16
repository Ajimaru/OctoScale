#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/aes.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <esp_random.h>
#include "octoprint.h"
#include "spooldb.h"

// Defined in main.cpp (config scalars that get backed up too).
extern float g_calFactor;
extern uint8_t g_flowAskTimeoutSec;

// backup.h — export/import of the full OctoScale config (NVS namespace "octoscale")
// as portable JSON.
//
// SENSITIVE: the OctoPrint API keys. Never exported in plaintext — each is encrypted
// individually with AES-256-CBC. The key is derived via PBKDF2-HMAC-SHA256 from a
// user-supplied passphrase + a random salt (10000 iterations). Without the passphrase
// the backup can't be restored.
//
// The rest of the config (names, hosts, ports, dbId, calFactor, dbInstance,
// askTimeout, and the device settings below) isn't secret and stays plaintext, so the
// backup remains readable — only the keys are protected.
//
// Backup format (JSON):
//   { "v":1, "enc":"aes256cbc-pbkdf2", "salt":"<b64>", "iter":10000,
//     "calFactor":..., "dbInstance":..., "askTimeout":...,
//     "settings": { "blActive":..., "ssTimeout":..., ... },
//     "octo":[ {name,host,port,dbId, "apikey_enc":"<b64: iv||ciphertext>"} ] }
//
// "settings" carries every remaining NVS key in the "octoscale" namespace: display
// brightness/idle stages, buzzer, LED, TFT theme, debug log, firmware check. It is
// addressed by NVS key name rather than by the matching global, because g_menuDark is
// file-static in menu.h, which this header is included long before — going through NVS
// keeps one code path for all of them instead of two.
//
// Version stays 1. A "v":1 reader that predates this field ignores the unknown
// "settings" object (ArduinoJson returns null for a missing key), and this reader
// treats a backup without it as "nothing to restore" rather than writing defaults —
// see bkRestoreSettings(). Bumping the version would have made old backups
// unrestorable for no gain.

#define BK_SALT_LEN   16
#define BK_ITER       10000
#define BK_KEY_LEN    32   // AES-256
#define BK_IV_LEN     16

// --- Base64 helpers ----------------------------------------------------------
inline String bkB64enc(const uint8_t *in, size_t len) {
  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen, in, len);  // query required size
  std::unique_ptr<uint8_t[]> buf(new uint8_t[olen + 1]);
  if (mbedtls_base64_encode(buf.get(), olen + 1, &olen, in, len) != 0) return "";
  buf[olen] = 0;
  return String((char *)buf.get());
}

// Returns true and fills out on success; out is cleared on failure.
inline bool bkB64dec(const String &in, std::vector<uint8_t> &out) {
  out.clear();
  size_t olen = 0;
  const uint8_t *src = (const uint8_t *)in.c_str();
  size_t slen = in.length();
  mbedtls_base64_decode(nullptr, 0, &olen, src, slen);
  out.resize(olen ? olen : 1);
  if (mbedtls_base64_decode(out.data(), out.size(), &olen, src, slen) != 0) {
    out.clear();
    return false;
  }
  out.resize(olen);
  return true;
}

// --- Key derivation (PBKDF2-HMAC-SHA256) --------------------------------------
inline bool bkDeriveKey(const String &pw, const uint8_t *salt, size_t saltLen,
                        int iter, uint8_t keyOut[BK_KEY_LEN]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok = false;
  if (mbedtls_md_setup(&ctx, info, 1) == 0) {
    ok = mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const uint8_t *)pw.c_str(), pw.length(),
                                   salt, saltLen, iter, BK_KEY_LEN, keyOut) == 0;
  }
  mbedtls_md_free(&ctx);
  return ok;
}

// --- AES-256-CBC with PKCS#7 padding -------------------------------------------
// Encrypts plain -> "iv||ciphertext", base64-encoded. Fresh random IV per field.
inline String bkEncField(const uint8_t key[BK_KEY_LEN], const String &plain) {
  size_t plen = plain.length();
  size_t pad = BK_IV_LEN - (plen % BK_IV_LEN);  // 1..16, always >=1 (PKCS#7)
  size_t clen = plen + pad;
  std::vector<uint8_t> in(clen);
  memcpy(in.data(), plain.c_str(), plen);
  for (size_t i = plen; i < clen; i++) in[i] = (uint8_t)pad;

  uint8_t iv[BK_IV_LEN];
  esp_fill_random(iv, BK_IV_LEN);
  uint8_t ivWork[BK_IV_LEN];
  memcpy(ivWork, iv, BK_IV_LEN);

  std::vector<uint8_t> out(clen);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 256);
  int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, clen, ivWork,
                                 in.data(), out.data());
  mbedtls_aes_free(&aes);
  if (rc != 0) return "";

  std::vector<uint8_t> blob(BK_IV_LEN + clen);  // iv || ciphertext
  memcpy(blob.data(), iv, BK_IV_LEN);
  memcpy(blob.data() + BK_IV_LEN, out.data(), clen);
  return bkB64enc(blob.data(), blob.size());
}

// Decrypts base64("iv||ciphertext") -> plaintext. Returns false on failure (a wrong
// password shows up as broken PKCS#7 padding).
inline bool bkDecField(const uint8_t key[BK_KEY_LEN], const String &b64, String &out) {
  out = "";
  std::vector<uint8_t> blob;
  if (!bkB64dec(b64, blob)) return false;
  if (blob.size() < BK_IV_LEN + BK_IV_LEN || (blob.size() - BK_IV_LEN) % BK_IV_LEN != 0)
    return false;
  uint8_t iv[BK_IV_LEN];
  memcpy(iv, blob.data(), BK_IV_LEN);
  size_t clen = blob.size() - BK_IV_LEN;
  std::vector<uint8_t> dec(clen);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key, 256);
  int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, clen, iv,
                                 blob.data() + BK_IV_LEN, dec.data());
  mbedtls_aes_free(&aes);
  if (rc != 0) return false;

  uint8_t pad = dec[clen - 1];
  if (pad == 0 || pad > BK_IV_LEN) return false;  // invalid padding -> wrong password
  for (size_t i = clen - pad; i < clen; i++) if (dec[i] != pad) return false;
  for (size_t i = 0; i < clen - pad; i++) out += (char)dec[i];  // byte-wise: NUL-safe
  return true;
}

// --- Device settings table ------------------------------------------------------
// Every NVS key in the "octoscale" namespace that is NOT already covered by a
// dedicated field above (calFactor, dbInstance, askTimeout) or by octoSave()
// (octoInstances). Keeping it as one table means adding a setting is a single line
// here, and export and import cannot drift apart.
//
// The type tag matters: Preferences stores the width it was written with, and reading
// a uint8 back as uint16 returns 0 rather than the value. So each entry records how
// main.cpp writes it.
enum BkType : uint8_t { BK_U8, BK_U16, BK_BOOL };

struct BkSetting {
  const char *key;
  BkType      type;
};

static const BkSetting BK_SETTINGS[] = {
  // Display: active/dim backlight and the three idle stages.
  { "blActive",   BK_U8   },
  { "blDim",      BK_U8   },
  { "blTimeout",  BK_U16  },
  { "ssEnabled",  BK_BOOL },
  { "ssTimeout",  BK_U16  },
  { "offEnabled", BK_BOOL },
  { "offTimeout", BK_U16  },
  { "menuDark",   BK_BOOL },
  // Buzzer.
  { "buzEn",      BK_BOOL },
  { "buzMode",    BK_U8   },
  { "buzVol",     BK_U8   },
  { "buzFreq",    BK_U16  },
  // LED.
  { "ledBright",  BK_U8   },
  { "ledOnboard", BK_BOOL },
  { "ledPin2",    BK_BOOL },
  // System.
  { "dbgLogEn",   BK_BOOL },
  { "fwOnline",   BK_BOOL },
};
static const size_t BK_SETTINGS_COUNT = sizeof(BK_SETTINGS) / sizeof(BK_SETTINGS[0]);

// Reads the settings straight out of NVS into the JSON object. A key that was never
// written is skipped entirely instead of being exported as its default — otherwise a
// restore would freeze today's defaults into the backup, and a later firmware could
// not change them for a device that had never touched the setting.
inline void bkExportSettings(JsonObject dst) {
  Preferences p;
  p.begin("octoscale", true);
  for (size_t i = 0; i < BK_SETTINGS_COUNT; i++) {
    const BkSetting &s = BK_SETTINGS[i];
    if (!p.isKey(s.key)) continue;
    switch (s.type) {
      case BK_U8:   dst[s.key] = p.getUChar(s.key);  break;
      case BK_U16:  dst[s.key] = p.getUShort(s.key); break;
      case BK_BOOL: dst[s.key] = p.getBool(s.key);   break;
    }
  }
  p.end();
}

// Writes the settings back into NVS. Keys missing from the backup are left untouched,
// so restoring an older backup keeps whatever the device already has rather than
// resetting it. Returns the number of keys written.
//
// The caller has to re-read NVS into the globals afterwards (and re-apply theme/LED),
// because the running firmware holds these values in RAM — see the /restore handler.
inline size_t bkRestoreSettings(JsonVariantConst src) {
  if (!src.is<JsonObjectConst>()) return 0;
  JsonObjectConst obj = src.as<JsonObjectConst>();
  Preferences p;
  p.begin("octoscale", false);
  size_t n = 0;
  for (size_t i = 0; i < BK_SETTINGS_COUNT; i++) {
    const BkSetting &s = BK_SETTINGS[i];
    JsonVariantConst v = obj[s.key];
    if (v.isNull()) continue;
    switch (s.type) {
      // Clamped to the field width on purpose: the JSON is a file the user can edit,
      // and an out-of-range number would otherwise wrap silently.
      case BK_U8:   p.putUChar(s.key, (uint8_t)constrain(v.as<int>(), 0, 255));      break;
      case BK_U16:  p.putUShort(s.key, (uint16_t)constrain(v.as<long>(), 0L, 65535L)); break;
      case BK_BOOL: p.putBool(s.key, v.as<bool>());                                  break;
    }
    n++;
  }
  p.end();
  return n;
}

// --- Export --------------------------------------------------------------------
// Builds the backup JSON. pw != "" => API keys encrypted; pw == "" => keys are
// OMITTED, so a plaintext key is never exported.
inline String bkExport(const String &pw) {
  JsonDocument doc;
  doc["v"] = 1;

  uint8_t salt[BK_SALT_LEN];
  uint8_t key[BK_KEY_LEN];
  bool haveKey = false;
  if (pw.length()) {
    esp_fill_random(salt, BK_SALT_LEN);
    haveKey = bkDeriveKey(pw, salt, BK_SALT_LEN, BK_ITER, key);
  }
  if (haveKey) {
    doc["enc"] = "aes256cbc-pbkdf2";
    doc["salt"] = bkB64enc(salt, BK_SALT_LEN);
    doc["iter"] = BK_ITER;
  } else {
    doc["enc"] = "none";  // no keys in the backup
  }

  doc["calFactor"] = g_calFactor;
  doc["dbInstance"] = g_dbInstance;
  doc["askTimeout"] = g_flowAskTimeoutSec;
  bkExportSettings(doc["settings"].to<JsonObject>());

  JsonArray arr = doc["octo"].to<JsonArray>();
  for (uint8_t i = 0; i < g_octoCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = g_octo[i].name;
    o["host"] = g_octo[i].host;
    o["port"] = g_octo[i].port;
    o["dbId"] = g_octo[i].dbId;
    if (haveKey && g_octo[i].apikey.length()) {
      o["apikey_enc"] = bkEncField(key, g_octo[i].apikey);
    }
  }

  String out;
  serializeJsonPretty(doc, out);
  return out;
}

// --- Import --------------------------------------------------------------------
// Applies a backup JSON (overwrites the current config + NVS). Returns true = ok;
// errOut carries a message on failure. A reboot afterward is recommended (flow
// state/running tasks).
inline bool bkImport(const String &json, const String &pw, String &errOut) {
  errOut = "";
  JsonDocument doc;
  if (deserializeJson(doc, json)) { errOut = "Invalid JSON"; return false; }
  if ((int)doc["v"] != 1) { errOut = "Unknown backup version"; return false; }

  String enc = doc["enc"] | "none";
  bool encrypted = enc == "aes256cbc-pbkdf2";
  uint8_t key[BK_KEY_LEN];
  bool haveKey = false;
  if (encrypted) {
    if (pw.length() == 0) { errOut = "Password required (backup is encrypted)"; return false; }
    std::vector<uint8_t> salt;
    if (!bkB64dec(doc["salt"] | "", salt) || salt.empty()) { errOut = "Salt missing/invalid"; return false; }
    int iter = doc["iter"] | BK_ITER;
    if (!bkDeriveKey(pw, salt.data(), salt.size(), iter, key)) { errOut = "Key derivation failed"; return false; }
    haveKey = true;
  }

  // Build the instance list and validate BEFORE overwriting NVS.
  OctoInstance tmp[OCTO_MAX_INSTANCES];
  uint8_t tmpCount = 0;
  for (JsonObject o : doc["octo"].as<JsonArray>()) {
    if (tmpCount >= OCTO_MAX_INSTANCES) break;
    String host = o["host"] | "";
    if (host.length() == 0) continue;
    OctoInstance &inst = tmp[tmpCount];
    inst.name = o["name"] | "";
    inst.host = host;
    inst.port = o["port"] | 80;
    inst.dbId = o["dbId"] | "";
    inst.apikey = "";
    if (o["apikey_enc"].is<const char *>()) {
      if (!haveKey) { errOut = "Encrypted key present, but no password given"; return false; }
      String plain;
      if (!bkDecField(key, o["apikey_enc"].as<String>(), plain)) {
        errOut = "Decryption failed (wrong password?)";
        return false;
      }
      inst.apikey = plain;
    }
    tmpCount++;
  }

  // Everything validated -> apply it.
  for (uint8_t i = 0; i < tmpCount; i++) g_octo[i] = tmp[i];
  g_octoCount = tmpCount;
  octoSave();

  if (doc["calFactor"].is<float>() || doc["calFactor"].is<double>()) {
    g_calFactor = doc["calFactor"].as<float>();
    if (g_calFactor > 0) {
      Preferences p; p.begin("octoscale", false);
      p.putFloat("calFactor", g_calFactor); p.end();
    }
  }
  {
    Preferences p; p.begin("octoscale", false);
    if (!doc["dbInstance"].isNull()) {
      g_dbInstance = doc["dbInstance"].as<uint8_t>();
      p.putUChar("dbInstance", g_dbInstance);
    }
    if (!doc["askTimeout"].isNull()) {
      g_flowAskTimeoutSec = doc["askTimeout"].as<uint8_t>();
      p.putUChar("askTimeout", g_flowAskTimeoutSec);
    }
    p.end();
  }

  // Display/buzzer/LED/system settings. Absent in backups written before this field
  // existed, in which case nothing is touched.
  bkRestoreSettings(doc["settings"]);
  return true;
}
