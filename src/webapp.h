#ifndef INKSIGHT_WEBAPP_H
#define INKSIGHT_WEBAPP_H

#include <Arduino.h>
#include "config.h"

// Connect to a saved WiFi network in STA mode.
// Tries each saved credential in order; returns true on first success.
// Aborts early on a hard failure (AP not found / auth failed) and stops
// altogether once WIFI_STA_BUDGET_MS is spent, so a dead router can no longer
// stall the boot for a minute before the provisioning AP appears.
bool connectWiFiSTA(unsigned long perNetTimeoutMs = WIFI_STA_ATTEMPT_MS,
                    unsigned long budgetMs = WIFI_STA_BUDGET_MS);

// Pump whichever network service is currently live (LAN HTTP server or the
// provisioning portal). Called from the blocking EPD waits so the page keeps
// answering while the panel refreshes (~15s).
void netServicePump();

// Start the LAN web service (mDNS inksight.local + HTTP server on port 80).
// Requires an active STA connection.
void webappStart();

// Stop the web service and mDNS (before switching to the provisioning portal).
void webappStop();

// True while the web service is running.
bool webappRunning();

// Process pending HTTP requests (call in loop).
void webappHandle();

// Run panel refresh/clear operations queued by the API handlers (call in loop).
// The ~15s refresh must never run inside an HTTP handler: executing it here
// keeps request responses instant and lets the EPD waits keep serving the page.
void webappProcessPending();

#endif // INKSIGHT_WEBAPP_H
