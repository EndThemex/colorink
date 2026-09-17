#ifndef INKSIGHT_PORTAL_H
#define INKSIGHT_PORTAL_H

#include <Arduino.h>

// Portal state
extern bool portalActive;
extern bool wifiConnected;

// Scan nearby APs before the hotspot is up (scanning with the AP running kicks
// connected phones off). Results are cached and served by the portal page.
void portalPreScan();

// Start the captive portal (AP mode + web server)
// apName is passed in so the SSID and the on-screen name are always identical.
void startCaptivePortal(const char *apName);

// Process pending portal HTTP/DNS requests (call in loop)
void handlePortalClients();

#endif // INKSIGHT_PORTAL_H
