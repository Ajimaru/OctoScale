#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// octoprint.h — manages multiple OctoPrint instances (name/host/port/apikey) as ONE
// JSON array in NVS (namespace "octoscale", key "octoInstances"), plus the two API
// calls the load flow needs:
//   1. octoToolCount()  — how many tools/extruders the printer has (live)
//   2. octoLoadSpool()  — tell SpoolManagerExtended to load the spool into tool N
//      (endpoint from PR #59: selectSpoolByQRCode/<id>?tool=<N>)
//
// All calls run from the HTTP handler context (loop/core 1) with a short timeout, so
// the web UI stays responsive.

#define OCTO_MAX_INSTANCES 4

struct OctoInstance {
  String name;
  String host;
  uint16_t port = 80;
  String apikey;
  // DB identity, auto-detected via the SpoolManagerExtended /databaseInfo endpoint and cached
  // (persisted in NVS). Two instances with the same non-empty dbId share the same
  // external DB -> allowed to fall back for each other. Empty = local SQLite or not
  // yet known -> never a fallback for another instance.
  String dbId;
};

static OctoInstance g_octo[OCTO_MAX_INSTANCES];
static uint8_t g_octoCount = 0;

// --- Persistence -----------------------------------------------------------
inline void octoLoad() {
  Preferences p;
  p.begin("octoscale", true);
  String json = p.getString("octoInstances", "");
  p.end();
  g_octoCount = 0;
  if (json.length() == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (g_octoCount >= OCTO_MAX_INSTANCES) break;
    OctoInstance &inst = g_octo[g_octoCount];
    inst.name = o["name"] | "";
    inst.host = o["host"] | "";
    inst.port = o["port"] | 80;
    inst.apikey = o["apikey"] | "";
    inst.dbId = o["dbId"] | "";  // cached DB identity, can be empty
    if (inst.host.length()) g_octoCount++;
  }
}

inline void octoSave() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < g_octoCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = g_octo[i].name;
    o["host"] = g_octo[i].host;
    o["port"] = g_octo[i].port;
    o["apikey"] = g_octo[i].apikey;
    o["dbId"] = g_octo[i].dbId;
  }
  String json;
  serializeJson(doc, json);
  Preferences p;
  p.begin("octoscale", false);
  p.putString("octoInstances", json);
  p.end();
}

// true = added, false = full. dbId is resolved later (octoRefreshDbIds), so empty here.
inline bool octoAdd(const String &name, const String &host, uint16_t port,
                    const String &apikey) {
  if (g_octoCount >= OCTO_MAX_INSTANCES || host.length() == 0) return false;
  OctoInstance &inst = g_octo[g_octoCount++];
  inst.name = name;
  inst.host = host;
  inst.port = port ? port : 80;
  inst.apikey = apikey;
  inst.dbId = "";
  octoSave();
  return true;
}

// true = updated, false = idx invalid or host empty. apikey is left UNCHANGED when
// keepKey is true (the web UI's edit form never shows the existing key back to the
// user, so "leave the key field blank" has to mean "keep it", not "clear it").
inline bool octoUpdate(uint8_t idx, const String &name, const String &host, uint16_t port,
                       const String &apikey, bool keepKey) {
  if (idx >= g_octoCount || host.length() == 0) return false;
  OctoInstance &inst = g_octo[idx];
  inst.name = name;
  inst.host = host;
  inst.port = port ? port : 80;
  if (!keepKey) inst.apikey = apikey;
  octoSave();
  return true;
}

inline bool octoDel(uint8_t idx) {
  if (idx >= g_octoCount) return false;
  for (uint8_t i = idx; i + 1 < g_octoCount; i++) g_octo[i] = g_octo[i + 1];
  g_octoCount--;
  octoSave();
  return true;
}

// Instance list as JSON for the web UI (apikey masked).
inline String octoListJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < g_octoCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["idx"] = i;
    o["name"] = g_octo[i].name;
    o["host"] = g_octo[i].host;
    o["port"] = g_octo[i].port;
    o["hasKey"] = g_octo[i].apikey.length() > 0;  // never return the key itself
    o["dbId"] = g_octo[i].dbId;                    // "" = SQLite/unknown
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// --- API calls ---------------------------------------------------------------

inline String octoBaseUrl(const OctoInstance &inst) {
  return "http://" + inst.host + ":" + String(inst.port);
}

// Tool/extruder count of the current printer profile.
// Source: GET /api/printerprofiles -> profiles.*.current==true -> extruder.count.
// Returns the count (>=1) or 0 on error; errOut gets a short message.
inline int octoToolCount(uint8_t idx, String &errOut) {
  errOut = "";
  if (idx >= g_octoCount) {
    errOut = "Invalid instance";
    return 0;
  }
  const OctoInstance &inst = g_octo[idx];
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  String url = octoBaseUrl(inst) + "/api/printerprofiles";
  if (!http.begin(url)) {
    errOut = "HTTP begin failed";
    return 0;
  }
  http.addHeader("X-Api-Key", inst.apikey);
  dbgLogf("HTTP GET %s", url.c_str());
  int code = http.GET();
  dbgLogf("HTTP GET %s -> %d", url.c_str(), code);
  if (code != HTTP_CODE_OK) {
    http.end();
    errOut = "OctoPrint HTTP " + String(code);
    return 0;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, body);
  if (e) {
    errOut = "printerprofiles JSON error";
    return 0;
  }
  // Shape: { "profiles": { "<id>": { "current": bool, "extruder": {"count": N} } } }
  JsonObject profiles = doc["profiles"].as<JsonObject>();
  int count = 0;
  int firstCount = 0;
  for (JsonPair kv : profiles) {
    JsonObject prof = kv.value().as<JsonObject>();
    int c = prof["extruder"]["count"] | 0;
    if (firstCount == 0 && c > 0) firstCount = c;
    if (prof["current"] | false) {
      count = c;
      break;
    }
  }
  if (count == 0) count = firstCount;  // no profile marked "current" -> use the first
  if (count == 0) {
    errOut = "No tool count in the profile";
    return 0;
  }
  return count;
}

// Fetches an instance's DB identity (SpoolManagerExtended's databaseInfo endpoint).
// Endpoint: GET /plugin/SpoolManagerExtended/databaseInfo -> {external:bool, dbId:string|null}
// dbId identifies the external DB (type://host:port/name, no credentials). Two
// instances with the same non-empty dbId share a DB -> allowed to fall back for each
// other. external=false / dbId empty -> local SQLite, never shareable.
// Returns: 1=ok (dbIdOut/externalOut set), 0=endpoint missing (404, old plugin),
// -1=error (errOut set).
inline int octoDbInfo(uint8_t idx, String &dbIdOut, bool &externalOut, String &errOut) {
  dbIdOut = "";
  externalOut = false;
  errOut = "";
  if (idx >= g_octoCount) {
    errOut = "Invalid instance";
    return -1;
  }
  const OctoInstance &inst = g_octo[idx];
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  String url = octoBaseUrl(inst) + "/plugin/SpoolManagerExtended/databaseInfo";
  if (!http.begin(url)) {
    errOut = "HTTP begin failed";
    return -1;
  }
  http.addHeader("X-Api-Key", inst.apikey);
  dbgLogf("HTTP GET %s", url.c_str());
  int code = http.GET();
  dbgLogf("HTTP GET %s -> %d", url.c_str(), code);
  if (code == 404) {  // endpoint not present (older plugin version)
    http.end();
    return 0;
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    errOut = "databaseInfo HTTP " + String(code);
    return -1;
  }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    errOut = "databaseInfo JSON error";
    return -1;
  }
  externalOut = doc["external"] | false;
  dbIdOut = doc["dbId"] | "";  // null -> ""
  return 1;
}

// Fetches the dbId of every reachable instance and caches it. Only updates the dbId
// on a successful response (r==1); an offline instance or one without databaseInfo
// (old plugin) keeps its previously cached dbId. Persists to NVS afterward.
// Returns the number of instances successfully updated.
inline int octoRefreshDbIds() {
  int updated = 0;
  for (uint8_t i = 0; i < g_octoCount; i++) {
    String dbId, err;
    bool ext = false;
    int r = octoDbInfo(i, dbId, ext, err);
    if (r == 1) {
      g_octo[i].dbId = dbId;  // "" for SQLite -> never shareable, by design
      updated++;
    }
    // r==0 (old plugin) or r==-1 (offline): keep the cached value
  }
  if (updated > 0) octoSave();
  return updated;
}

// Checks via SpoolManagerExtended whether a spool (databaseId) exists.
// Endpoint: GET /plugin/SpoolManagerExtended/spool/<id> (API-key protected).
//   200 -> exists, displayName from JSON {spool:{displayName:...}}
//   404 -> unknown
// Returns: 1=found, 0=not found, -1=error (errOut set).
//
// spoolWeightOut/totalWeightOut are optional and provide the reference values for the
// weigh flow: without the empty weight, the remaining filament can't be derived from
// the scale's gross reading. -1 means "not set in the record".
// NOTE: SpoolManagerExtended returns weights as *strings* ("1000.0"), not JSON numbers -> read
// via `| "0"` as a const char* and convert manually, otherwise ArduinoJson silently
// returns the default for `| -1.0f`.
inline int octoSpoolInfo(uint8_t idx, long databaseId, String &nameOut,
                         String &errOut, float *spoolWeightOut = nullptr,
                         float *totalWeightOut = nullptr,
                         String *vendorOut = nullptr, String *materialOut = nullptr,
                         float *remainingOut = nullptr, String *colorOut = nullptr,
                         String *colorNameOut = nullptr) {
  nameOut = "";
  errOut = "";
  if (spoolWeightOut) *spoolWeightOut = -1.0f;
  if (totalWeightOut) *totalWeightOut = -1.0f;
  if (vendorOut) *vendorOut = "";
  if (materialOut) *materialOut = "";
  if (remainingOut) *remainingOut = -1.0f;
  if (colorOut) *colorOut = "";
  if (colorNameOut) *colorNameOut = "";
  if (idx >= g_octoCount) {
    errOut = "Invalid instance";
    return -1;
  }
  const OctoInstance &inst = g_octo[idx];
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  String url = octoBaseUrl(inst) + "/plugin/SpoolManagerExtended/spool/" + String(databaseId);
  if (!http.begin(url)) {
    errOut = "HTTP begin failed";
    return -1;
  }
  http.addHeader("X-Api-Key", inst.apikey);
  dbgLogf("HTTP GET %s", url.c_str());
  int code = http.GET();
  dbgLogf("HTTP GET %s -> %d", url.c_str(), code);
  if (code == 404) {
    http.end();
    return 0;  // spool not in the DB
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    errOut = "SpoolManagerExtended HTTP " + String(code);
    return -1;
  }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    errOut = "spool JSON error";
    return -1;
  }
  nameOut = doc["spool"]["displayName"] | "";
  // Weights arrive as strings ("200.0"); empty/missing -> -1 = not set
  if (spoolWeightOut) {
    const char *sw = doc["spool"]["spoolWeight"] | "";
    if (sw && sw[0]) *spoolWeightOut = atof(sw);
  }
  if (totalWeightOut) {
    const char *tw = doc["spool"]["totalWeight"] | "";
    if (tw && tw[0]) *totalWeightOut = atof(tw);
  }
  // Display extras (vendor/material/remaining weight). remainingWeight is computed by
  // the plugin itself (Transformer) and returned as a string ("183.0").
  if (vendorOut)   *vendorOut   = String((const char *)(doc["spool"]["vendor"]   | ""));
  if (materialOut) *materialOut = String((const char *)(doc["spool"]["material"] | ""));
  if (remainingOut) {
    const char *rw = doc["spool"]["remainingWeight"] | "";
    if (rw && rw[0]) *remainingOut = atof(rw);
  }
  // Color: color = code string ("#rrggbb", up to 3 ";"-separated, "rainbow",
  // "transparent[:#hex..]"), colorName = display text (possibly localized).
  if (colorOut)     *colorOut     = String((const char *)(doc["spool"]["color"]     | ""));
  if (colorNameOut) *colorNameOut = String((const char *)(doc["spool"]["colorName"] | ""));
  return 1;
}

// Resolves a spool by its NFC tag UID instead of databaseId — for foreign/manufacturer
// tags (e.g. Snapmaker U1) that carry no OctoScale databaseId payload, only a raw UID.
// Endpoint: GET /plugin/SpoolManagerExtended/spool/byCode/<uid> (mirrors octoSpoolInfo's
// /spool/<id>; matches SpoolManagerExtended's `code` field, same string SpoolManagerExtended's own
// U1RfidManager.normalizeCardUid() produces — uppercase hex, no separators).
// Returns: 1=found (databaseIdOut set), 0=not found, -1=error (errOut set).
inline int octoSpoolInfoByCode(uint8_t idx, const String &uid, long &databaseIdOut,
                               String &nameOut, String &errOut,
                               float *spoolWeightOut = nullptr,
                               float *totalWeightOut = nullptr,
                               String *vendorOut = nullptr, String *materialOut = nullptr,
                               float *remainingOut = nullptr, String *colorOut = nullptr,
                               String *colorNameOut = nullptr) {
  databaseIdOut = -1;
  nameOut = "";
  errOut = "";
  if (spoolWeightOut) *spoolWeightOut = -1.0f;
  if (totalWeightOut) *totalWeightOut = -1.0f;
  if (vendorOut) *vendorOut = "";
  if (materialOut) *materialOut = "";
  if (remainingOut) *remainingOut = -1.0f;
  if (colorOut) *colorOut = "";
  if (colorNameOut) *colorNameOut = "";
  if (idx >= g_octoCount) {
    errOut = "Invalid instance";
    return -1;
  }
  const OctoInstance &inst = g_octo[idx];
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  String url = octoBaseUrl(inst) + "/plugin/SpoolManagerExtended/spool/byCode/" + uid;
  if (!http.begin(url)) {
    errOut = "HTTP begin failed";
    return -1;
  }
  http.addHeader("X-Api-Key", inst.apikey);
  dbgLogf("HTTP GET %s", url.c_str());
  int code = http.GET();
  dbgLogf("HTTP GET %s -> %d", url.c_str(), code);
  if (code == 404) {
    http.end();
    return 0;  // no spool tagged with this UID
  }
  if (code != HTTP_CODE_OK) {
    http.end();
    errOut = "SpoolManagerExtended HTTP " + String(code);
    return -1;
  }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    errOut = "spool JSON error";
    return -1;
  }
  databaseIdOut = doc["spool"]["databaseId"] | -1L;
  nameOut = doc["spool"]["displayName"] | "";
  if (spoolWeightOut) {
    const char *sw = doc["spool"]["spoolWeight"] | "";
    if (sw && sw[0]) *spoolWeightOut = atof(sw);
  }
  if (totalWeightOut) {
    const char *tw = doc["spool"]["totalWeight"] | "";
    if (tw && tw[0]) *totalWeightOut = atof(tw);
  }
  if (vendorOut)   *vendorOut   = String((const char *)(doc["spool"]["vendor"]   | ""));
  if (materialOut) *materialOut = String((const char *)(doc["spool"]["material"] | ""));
  if (remainingOut) {
    const char *rw = doc["spool"]["remainingWeight"] | "";
    if (rw && rw[0]) *remainingOut = atof(rw);
  }
  if (colorOut)     *colorOut     = String((const char *)(doc["spool"]["color"]     | ""));
  if (colorNameOut) *colorNameOut = String((const char *)(doc["spool"]["colorName"] | ""));
  return 1;
}

// Writes back a gross weight (spool + filament) measured on the scale.
// Endpoint: PUT /plugin/SpoolManagerExtended/spool/<id>/measuredWeight  {"grossWeight": <g>}
// The plugin subtracts the empty weight itself and converts to usedWeight; the scale
// doesn't need to know anything about the spool for this.
// Returns true = saved. On 400 the reasons are in the body as validationErrors and are
// surfaced, otherwise the device would only see "HTTP 400". 409 = the spool was
// changed elsewhere in the meantime (nothing written).
inline bool octoSetMeasuredWeight(uint8_t idx, long databaseId, float grossWeight,
                                  String &errOut) {
  errOut = "";
  if (idx >= g_octoCount) {
    errOut = "Invalid instance";
    return false;
  }
  const OctoInstance &inst = g_octo[idx];
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  String url = octoBaseUrl(inst) + "/plugin/SpoolManagerExtended/spool/" +
               String(databaseId) + "/measuredWeight";
  if (!http.begin(url)) {
    errOut = "HTTP begin failed";
    return false;
  }
  http.addHeader("X-Api-Key", inst.apikey);
  http.addHeader("Content-Type", "application/json");

  JsonDocument body;
  body["grossWeight"] = grossWeight;
  String payload;
  serializeJson(body, payload);

  dbgLogf("HTTP PUT %s body=%s", url.c_str(), payload.c_str());
  int code = http.PUT(payload);
  String resp = http.getString();
  http.end();
  dbgLogf("HTTP PUT %s -> %d", url.c_str(), code);

  if (code == HTTP_CODE_OK) return true;

  if (code == 400 || code == 409) {
    JsonDocument doc;
    if (!deserializeJson(doc, resp)) {
      JsonArray errs = doc["validationErrors"].as<JsonArray>();
      if (!errs.isNull() && errs.size() > 0) {
        for (JsonVariant v : errs) {
          if (errOut.length()) errOut += "; ";
          errOut += v.as<const char *>();
        }
        return false;
      }
      const char *msg = doc["error"] | "";
      if (msg && msg[0]) {
        errOut = msg;
        return false;
      }
    }
  }
  errOut = "SpoolManagerExtended HTTP " + String(code);
  return false;
}

// Tells SpoolManagerExtended to load the spool (databaseId) into tool N.
// Endpoint (PR #59): GET /plugin/SpoolManagerExtended/selectSpoolByQRCode/<id>?tool=<N>
// This is a server-side redirect GET; 2xx OR 3xx counts as success.
inline bool octoLoadSpool(uint8_t idx, long databaseId, int tool, String &errOut) {
  errOut = "";
  if (idx >= g_octoCount) {
    errOut = "Invalid instance";
    return false;
  }
  const OctoInstance &inst = g_octo[idx];
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // redirect = success
  String url = octoBaseUrl(inst) + "/plugin/SpoolManagerExtended/selectSpoolByQRCode/" +
               String(databaseId) + "?tool=" + String(tool);
  if (!http.begin(url)) {
    errOut = "HTTP begin failed";
    return false;
  }
  http.addHeader("X-Api-Key", inst.apikey);
  dbgLogf("HTTP GET %s", url.c_str());
  int code = http.GET();
  http.end();
  dbgLogf("HTTP GET %s -> %d", url.c_str(), code);
  if (code >= 200 && code < 400) return true;
  errOut = "SpoolManagerExtended HTTP " + String(code);
  return false;
}
