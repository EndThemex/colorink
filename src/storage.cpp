#include "storage.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

// Config version — bump when NVS schema changes
static const int CONFIG_VERSION = 1;

// ── Multi-WiFi credential list (in-memory, mirrors NVS) ─────
static String g_wifiSsids[MAX_WIFI_NETWORKS];
static String g_wifiPass[MAX_WIFI_NETWORKS];
static int    g_wifiCount = 0;

// Build NVS key "wifi_sN" / "wifi_pN" (kind = 's' or 'p').
static String wifiKey(char kind, int idx) {
    char buf[12];
    snprintf(buf, sizeof(buf), "wifi_%c%d", kind, idx);
    return String(buf);
}

// Persist the in-memory list to NVS (assumes prefs already open read-write).
static void persistWiFiList(Preferences &p) {
    p.putInt("wifi_n", g_wifiCount);
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (i < g_wifiCount) {
            p.putString(wifiKey('s', i).c_str(), g_wifiSsids[i]);
            p.putString(wifiKey('p', i).c_str(), g_wifiPass[i]);
        } else {
            p.remove(wifiKey('s', i).c_str());
            p.remove(wifiKey('p', i).c_str());
        }
    }
    // Keep legacy single-SSID keys in sync with slot 0 (back-compat).
    if (g_wifiCount > 0) {
        p.putString("ssid", g_wifiSsids[0]);
        p.putString("pass", g_wifiPass[0]);
    } else {
        p.remove("ssid");
        p.remove("pass");
    }
}

// ── Load config from NVS ────────────────────────────────────

void loadConfig() {
    prefs.begin("inksight", true);  // read-only

    int version = prefs.getInt("cfg_version", 0);
    if (version != CONFIG_VERSION) {
        // Config version mismatch — still try legacy single-SSID keys so the
        // user does not have to re-provision after an upgrade.
        String ssid = prefs.getString("ssid", "");
        String pass = prefs.getString("pass", "");
        prefs.end();
        g_wifiCount = 0;
        if (ssid.length() > 0) {
            g_wifiSsids[0] = ssid;
            g_wifiPass[0]  = pass;
            g_wifiCount = 1;
            prefs.begin("inksight", false);
            prefs.putInt("cfg_version", CONFIG_VERSION);
            persistWiFiList(prefs);
            prefs.end();
        }
        Serial.printf("Config migrated to v%d (%d networks)\n", CONFIG_VERSION, g_wifiCount);
        return;
    }

    int wifiN = prefs.getInt("wifi_n", -1);
    if (wifiN < 0) {
        // Legacy single-SSID keys
        String ssid = prefs.getString("ssid", "");
        if (ssid.length() > 0) {
            g_wifiSsids[0] = ssid;
            g_wifiPass[0]  = prefs.getString("pass", "");
            g_wifiCount = 1;
        }
    } else {
        if (wifiN > MAX_WIFI_NETWORKS) wifiN = MAX_WIFI_NETWORKS;
        for (int i = 0; i < wifiN; i++) {
            String s = prefs.getString(wifiKey('s', i).c_str(), "");
            if (s.length() == 0) continue;  // skip corrupt/empty slot
            g_wifiSsids[g_wifiCount] = s;
            g_wifiPass[g_wifiCount]  = prefs.getString(wifiKey('p', i).c_str(), "");
            g_wifiCount++;
        }
    }
    prefs.end();

    Serial.printf("Config loaded: %d WiFi network(s)\n", g_wifiCount);
}

// ── Save WiFi credentials ───────────────────────────────────

// Set as the primary network (slot 0).
void saveWiFiConfig(const String &ssid, const String &pass) {
    addWiFiConfig(ssid, pass);
}

// ── Multi-WiFi credential list ──────────────────────────────

int getWiFiCount() {
    return g_wifiCount;
}

bool getWiFiAt(int idx, String &ssid, String &pass) {
    if (idx < 0 || idx >= g_wifiCount) return false;
    ssid = g_wifiSsids[idx];
    pass = g_wifiPass[idx];
    return true;
}

void getWiFiSSIDList(String out[], int &count) {
    count = g_wifiCount;
    for (int i = 0; i < g_wifiCount; i++) {
        out[i] = g_wifiSsids[i];
    }
}

bool addWiFiConfig(const String &ssid, const String &pass) {
    if (ssid.length() == 0) return false;

    // Existing SSID: update password and move to front (slot 0).
    int existing = -1;
    for (int i = 0; i < g_wifiCount; i++) {
        if (g_wifiSsids[i] == ssid) { existing = i; break; }
    }
    if (existing >= 0) {
        for (int i = existing; i > 0; i--) {
            g_wifiSsids[i] = g_wifiSsids[i - 1];
            g_wifiPass[i]  = g_wifiPass[i - 1];
        }
        g_wifiSsids[0] = ssid;
        g_wifiPass[0]  = pass;
    } else {
        if (g_wifiCount >= MAX_WIFI_NETWORKS) return false;  // list full
        // Append new network to the end; tried after earlier-saved ones.
        g_wifiSsids[g_wifiCount] = ssid;
        g_wifiPass[g_wifiCount]  = pass;
        g_wifiCount++;
    }

    prefs.begin("inksight", false);
    prefs.putInt("cfg_version", CONFIG_VERSION);
    persistWiFiList(prefs);
    prefs.end();
    return true;
}

bool deleteWiFiBySSID(const String &ssid) {
    int idx = -1;
    for (int i = 0; i < g_wifiCount; i++) {
        if (g_wifiSsids[i] == ssid) { idx = i; break; }
    }
    if (idx < 0) return false;
    for (int i = idx; i < g_wifiCount - 1; i++) {
        g_wifiSsids[i] = g_wifiSsids[i + 1];
        g_wifiPass[i]  = g_wifiPass[i + 1];
    }
    g_wifiCount--;
    g_wifiSsids[g_wifiCount] = "";
    g_wifiPass[g_wifiCount]  = "";

    prefs.begin("inksight", false);
    prefs.putInt("cfg_version", CONFIG_VERSION);
    persistWiFiList(prefs);
    prefs.end();
    return true;
}
