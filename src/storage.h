#ifndef INKSIGHT_STORAGE_H
#define INKSIGHT_STORAGE_H

#include <Arduino.h>

// ── NVS operations ──────────────────────────────────────────

// Load saved WiFi credentials from NVS
void loadConfig();

// Save WiFi credentials to NVS (set as primary network)
void saveWiFiConfig(const String &ssid, const String &pass);

// ── Multi-WiFi credential list (up to MAX_WIFI_NETWORKS) ────
// Networks are tried in index order (0 first) on boot.

// Number of saved WiFi networks.
int  getWiFiCount();

// Read credentials at index. Returns false if idx out of range.
bool getWiFiAt(int idx, String &ssid, String &pass);

// Add or update a network. If the SSID already exists, its password is
// updated and it is moved to the front (slot 0). Otherwise it is appended.
// Returns false if the list is full and the SSID is new.
bool addWiFiConfig(const String &ssid, const String &pass);

// Delete a network by SSID and compact the list. Returns true if removed.
bool deleteWiFiBySSID(const String &ssid);

// Fill out[] (length >= MAX_WIFI_NETWORKS) with saved SSID names (no passwords).
// count is set to the number written.
void getWiFiSSIDList(String out[], int &count);

#endif // INKSIGHT_STORAGE_H
