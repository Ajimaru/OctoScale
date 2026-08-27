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
// askTimeout) isn't secret and stays plaintext, so the backup remains readable —
// only the keys are protected.
//
// Backup format (JSON):
//   { "v":1, "enc":"aes256cbc-pbkdf2", "salt":"<b64>", "iter":10000,
//     "calFactor":..., "dbInstance":..., "askTimeout":...,
//     "octo":[ {name,host,port,dbId, "apikey_enc":"<b64: iv||ciphertext>"} ] }

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
  return true;
}
