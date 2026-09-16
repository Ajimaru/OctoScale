#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "octoprint.h"

// spooldb.h — checks whether a spool (databaseId) is known.
//
// Uses an HTTP bridge through SpoolManagerExtended instead of direct DB access: the existing,
// API-key-protected endpoint GET /plugin/SpoolManagerExtended/spool/<id> returns the spool
// (200) or 404. SpoolManagerExtended handles the DB access itself — no DB driver or password
// on the ESP (see HARDWARE.md for why direct MySQL access was abandoned).
//
// The DB source is one of the configured OctoPrint instances (g_dbInstance, default
// 0). Since all instances typically share the same SpoolManagerExtended DB, one is enough.

static uint8_t g_dbInstance = 0;  // index of the OctoPrint instance used as the DB source

inline void dbLoadCfg() {
  Preferences p;
  p.begin("octoscale", true);
  g_dbInstance = p.getUChar("dbInstance", 0);
  p.end();
}

inline void dbSetInstance(uint8_t idx) {
  g_dbInstance = idx;
  Preferences p;
  p.begin("octoscale", false);
  p.putUChar("dbInstance", idx);
  p.end();
}

inline String dbCfgJson() {
  JsonDocument doc;
  doc["dbInstance"] = g_dbInstance;
  doc["instanceCount"] = g_octoCount;
  String out;
  serializeJson(doc, out);
  return out;
}

// Which instance actually answered the last spoolExists() call (-1 = none), for
// diagnostics/UI.
static int g_dbUsedInstance = -1;

// Checks whether a spool with the given databaseId exists (via the SpoolManagerExtended HTTP
// bridge). On a hit, displayName is written to nameOut.
//
// FALLBACK LOGIC (against an offline DB source): try the primary instance
// (g_dbInstance) first. If it's UNREACHABLE (octoSpoolInfo == -1) and marked as
// sharing the DB, the other sharedDb instances are tried in order — they serve the
// same spools. Instances WITHOUT a shared DB (local SQLite) never stand in for
// another instance (their IDs could belong to unrelated spools).
//
// IMPORTANT: a clean "404 = spool not in DB" (octoSpoolInfo == 0) does NOT trigger a
// fallback — the (reachable) DB simply doesn't have it, and an equivalent DB would say
// the same.
//
// Returns true = known. errOut carries a message for real errors (empty for a clean
// "not found"). spoolWeightOut/totalWeightOut etc. are optional.
inline bool spoolExists(long id, String &nameOut, String &errOut,
                        float *spoolWeightOut = nullptr,
                        float *totalWeightOut = nullptr,
                        String *vendorOut = nullptr, String *materialOut = nullptr,
                        float *remainingOut = nullptr, String *colorOut = nullptr,
                        String *colorNameOut = nullptr) {
  nameOut = "";
  errOut = "";
  g_dbUsedInstance = -1;
  if (g_octoCount == 0) {
    errOut = "No OctoPrint instance configured";
    return false;
  }
  uint8_t primary = g_dbInstance < g_octoCount ? g_dbInstance : 0;

  // 1. Try the primary instance.
  int r = octoSpoolInfo(primary, id, nameOut, errOut, spoolWeightOut, totalWeightOut,
                        vendorOut, materialOut, remainingOut, colorOut, colorNameOut);
  if (r == 1) { g_dbUsedInstance = primary; return true; }
  if (r == 0) { g_dbUsedInstance = primary; return false; }  // reachable, not there

  // 2. Primary is unreachable (r == -1). Fall back only to instances with an
  //    IDENTICAL, non-empty dbId (same external DB, from databaseInfo, cached). An
  //    empty dbId (local SQLite/unknown) has no valid fallback.
  String pdb = g_octo[primary].dbId;
  if (pdb.length() == 0) {
    errOut = "DB source (" + (g_octo[primary].name.length() ? g_octo[primary].name
             : g_octo[primary].host) + ") unreachable (local DB, no fallback)";
    return false;
  }

  for (uint8_t i = 0; i < g_octoCount; i++) {
    if (i == primary) continue;
    if (g_octo[i].dbId != pdb) continue;  // only the same DB (matching dbId)
    String e2;
    int r2 = octoSpoolInfo(i, id, nameOut, e2, spoolWeightOut, totalWeightOut,
                           vendorOut, materialOut, remainingOut, colorOut, colorNameOut);
    if (r2 == 1) { g_dbUsedInstance = i; errOut = ""; return true; }
    if (r2 == 0) { g_dbUsedInstance = i; errOut = ""; return false; }  // reachable, not there
    // r2 == -1: also offline -> try next
  }

  errOut = "No reachable DB source (all instances sharing this DB are offline)";
  return false;
}

// Same as spoolExists(), but resolves by NFC tag UID (SpoolManagerExtended's `code` field)
// instead of databaseId — for foreign/manufacturer tags (e.g. Snapmaker) that carry
// no OctoScale databaseId payload. On a hit, idOut carries the spool's real databaseId
// so the rest of the flow (load/tool/weigh) can proceed exactly as with a native tag.
inline bool spoolExistsByCode(const String &uid, long &idOut, String &nameOut,
                              String &errOut, float *spoolWeightOut = nullptr,
                              float *totalWeightOut = nullptr,
                              String *vendorOut = nullptr, String *materialOut = nullptr,
                              float *remainingOut = nullptr, String *colorOut = nullptr,
                              String *colorNameOut = nullptr) {
  idOut = -1;
  nameOut = "";
  errOut = "";
  g_dbUsedInstance = -1;
  if (g_octoCount == 0) {
    errOut = "No OctoPrint instance configured";
    return false;
  }
  uint8_t primary = g_dbInstance < g_octoCount ? g_dbInstance : 0;

  int r = octoSpoolInfoByCode(primary, uid, idOut, nameOut, errOut, spoolWeightOut,
                              totalWeightOut, vendorOut, materialOut, remainingOut,
                              colorOut, colorNameOut);
  if (r == 1) { g_dbUsedInstance = primary; return true; }
  if (r == 0) { g_dbUsedInstance = primary; return false; }

  String pdb = g_octo[primary].dbId;
  if (pdb.length() == 0) {
    errOut = "DB source (" + (g_octo[primary].name.length() ? g_octo[primary].name
             : g_octo[primary].host) + ") unreachable (local DB, no fallback)";
    return false;
  }

  for (uint8_t i = 0; i < g_octoCount; i++) {
    if (i == primary) continue;
    if (g_octo[i].dbId != pdb) continue;
    String e2;
    int r2 = octoSpoolInfoByCode(i, uid, idOut, nameOut, e2, spoolWeightOut,
                                 totalWeightOut, vendorOut, materialOut, remainingOut,
                                 colorOut, colorNameOut);
    if (r2 == 1) { g_dbUsedInstance = i; errOut = ""; return true; }
    if (r2 == 0) { g_dbUsedInstance = i; errOut = ""; return false; }
  }

  errOut = "No reachable DB source (all instances sharing this DB are offline)";
  return false;
}
