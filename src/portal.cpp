#include "portal.h"
#include "config.h"
#include "storage.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_netif.h>

#include "../data/portal_html.h"

// ── Portal state ────────────────────────────────────────────
bool portalActive    = false;
bool wifiConnected   = false;

static bool     wifiConnecting = false;
static String   lastWifiError  = "";
static bool     pendingRestart = false;
static unsigned long restartAtMillis = 0;

static WebServer webServer(80);
static DNSServer dnsServer;

// ── Input validation helpers ────────────────────────────────

static const int PORTAL_MAX_SSID   = 32;
static const int PORTAL_MAX_PASS = 64;

static String sanitizeInput(const String &input, int maxLen) {
    String result = input.substring(0, maxLen);
    result.trim();
    result.replace("<", "");
    result.replace(">", "");
    return result;
}

static String sanitizeTextInput(const String &input, int maxLen) {
    String result = sanitizeInput(input, maxLen);
    result.replace("\"", "");
    result.replace("'", "");
    result.replace("&", "");
    result.replace("\\", "");
    return result;
}

static String sanitizeSSID(const String &input) {
    String result = sanitizeTextInput(input, PORTAL_MAX_SSID);
    // Remove control characters (keep printable ASCII + UTF-8 multibyte)
    String cleaned;
    for (unsigned int i = 0; i < result.length(); i++) {
        char c = result.charAt(i);
        if (c >= 32 || (c & 0x80)) cleaned += c;
    }
    return cleaned;
}

// Escape a string for safe embedding in a JSON string literal.
static String jsonEscape(const String &s) {
    String out;
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

// Build {"networks":["a","b"],"max":N} from the saved WiFi list.
static String buildWiFiListJson() {
    String names[MAX_WIFI_NETWORKS];
    int count = 0;
    getWiFiSSIDList(names, count);
    String json = "{\"networks\":[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "\"" + jsonEscape(names[i]) + "\"";
    }
    json += "],\"max\":" + String(MAX_WIFI_NETWORKS) + "}";
    return json;
}

static bool findSavedWiFiPassword(const String &targetSsid, String &passOut) {
    int count = getWiFiCount();
    for (int i = 0; i < count; i++) {
        String savedSsid, savedPass;
        if (!getWiFiAt(i, savedSsid, savedPass)) continue;
        if (savedSsid == targetSsid) {
            passOut = savedPass;
            return true;
        }
    }
    return false;
}

static bool connectPortalWiFi(const String &ssid, const String &pass) {
    Serial.printf("Portal: connecting to %s\n", ssid.c_str());
    wifiConnecting = true;
    lastWifiError  = "";

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < (unsigned long)WIFI_TIMEOUT) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();

    wifiConnecting = false;

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        lastWifiError = "";
        Serial.printf("WiFi OK  IP=%s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    uint8_t reason = WiFi.status();
    Serial.printf("WiFi connection failed, status code: %d\n", reason);
    if (reason == WL_NO_SSID_AVAIL) {
        lastWifiError = "NO_SSID";
    } else if (reason == WL_CONNECT_FAILED) {
        lastWifiError = "AUTH_FAIL";
    } else {
        lastWifiError = "TIMEOUT";
    }
    WiFi.disconnect();
    WiFi.mode(WIFI_AP_STA); // 保持热点（以及它的 DHCP 服务器）存活，等用户重试
    return false;
}

static void sendPortalConnectSuccess() {
    String response = String("{\"ok\":true,\"list\":") + buildWiFiListJson() + "}";
    Serial.printf("Sending response: %s\n", response.c_str());
    webServer.send(200, "application/json", response);

    pendingRestart  = true;
    restartAtMillis = millis() + 5000;
    Serial.println("Restart scheduled in 5s (or earlier via /restart)");
}

static void resetPortalProvisioningState() {
    pendingRestart = false;
    restartAtMillis = 0;
    wifiConnected = false;
    wifiConnecting = false;
    lastWifiError = "";
    WiFi.disconnect();
    WiFi.mode(WIFI_AP_STA);
}

// ── Cached WiFi scan results ────────────────────────────────
// ESP32 只有一个射频：热点开着做全信道扫描时，射频会切走 2~2.4s，
// 已关联的手机直接掉线（配网页每次刷新都自动扫一次 => 手机反复连不上）。
// 所以进入配网时（热点还没起来）先扫一次缓存下来，网页加载只读缓存，
// 只有用户主动点"扫描"才现场重扫。

struct PortalNet
{
    String ssid;
    int rssi;
    bool secure;
};

static PortalNet portalNets[32];
static int portalNetCount = 0;
static bool portalScanDone = false;

static void portalScanNetworks()
{
    Serial.printf("[PORTAL] scanning WiFi... heap=%u\n", (unsigned)ESP.getFreeHeap());
    unsigned long t0 = millis();
    // 每信道驻留 120ms（默认 300ms）：13 信道从 ~4s 降到 ~1.6s，
    // 这段时间是热点起来前的纯等待，直接影响"热点多久能看见"。
    int n = WiFi.scanNetworks(false, true, false, 120);
    Serial.printf("[PORTAL] found %d networks in %lums\n", n, millis() - t0);

    portalNetCount = 0;
    for (int i = 0; i < n; i++)
    {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0)
            continue;
        int rssi = WiFi.RSSI(i);
        bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        int found = -1;
        for (int j = 0; j < portalNetCount; j++)
            if (portalNets[j].ssid == ssid)
            {
                found = j;
                break;
            }
        if (found >= 0)
        {
            if (rssi > portalNets[found].rssi)
            { // 同名取信号最强的
                portalNets[found].rssi = rssi;
                portalNets[found].secure = secure;
            }
        }
        else if (portalNetCount < 32)
        {
            portalNets[portalNetCount].ssid = ssid;
            portalNets[portalNetCount].rssi = rssi;
            portalNets[portalNetCount].secure = secure;
            portalNetCount++;
        }
    }
    // 信号强的排前面
    for (int i = 0; i < portalNetCount - 1; i++)
        for (int j = i + 1; j < portalNetCount; j++)
            if (portalNets[j].rssi > portalNets[i].rssi)
            {
                PortalNet tmp = portalNets[i];
                portalNets[i] = portalNets[j];
                portalNets[j] = tmp;
            }

    WiFi.scanDelete();
    portalScanDone = true;
}

static String buildScanJson()
{
    String json = "{\"networks\":[";
    for (int i = 0; i < portalNetCount; i++)
    {
        if (i > 0)
            json += ",";
        json += "{\"ssid\":\"" + jsonEscape(portalNets[i].ssid) + "\",";
        json += "\"rssi\":" + String(portalNets[i].rssi) + ",";
        json += "\"secure\":" + String(portalNets[i].secure ? "true" : "false") + "}";
    }
    json += "]}";
    return json;
}

// 进入配网时调用：此刻热点还没起来，扫描不会踢掉任何已连接的设备
void portalPreScan()
{
    if (portalScanDone)
        return; // 本轮已有扫描结果，别再把热点推迟 1.5s
    WiFi.mode(WIFI_STA);
    delay(80);
    portalScanNetworks();
    WiFi.mode(WIFI_OFF); // 恢复成进 startCaptivePortal 前的状态
    delay(150);          // 扫描后多留一点时间给射频停稳，别紧接着起 AP
}

// ── Start captive portal ────────────────────────────────────

void startCaptivePortal(const char *apName)
{
    // 回到最初能正常配网的写法：AP_STA + 裸 softAP()。
    // 之前改成 WIFI_AP + softAPConfig + 手工拉 dhcps，结果手机一直"无法加入网络"。
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false); // 关掉 modem sleep：热点与 DHCP 响应才及时，手机不会因丢 beacon 掉线
    bool apOk = WiFi.softAP(apName);
    delay(150);

    // DHCP 服务器没起来的话手机同样会"关联上但拿不到 IP"，这里兜底重启一次
    esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_dhcp_status_t dhcps = ESP_NETIF_DHCP_INIT;
    if (apNetif && esp_netif_dhcps_get_status(apNetif, &dhcps) == ESP_OK)
    {
        if (dhcps != ESP_NETIF_DHCP_STARTED)
        {
            esp_err_t err = esp_netif_dhcps_start(apNetif);
            Serial.printf("[PORTAL] dhcps was not started (%d), restart -> %d\n", (int)dhcps, (int)err);
        }
        else
        {
            Serial.println("[PORTAL] dhcps started");
        }
    }

    Serial.printf("AP started: %s  ok=%d  IP: %s  heap: %u\n",
                  apName, apOk, WiFi.softAPIP().toString().c_str(),
                  (unsigned)ESP.getFreeHeap());

    // AP 侧事件日志（带时间戳）：区分"手机加入失败"是关联被拒、掉线，还是 DHCP 拿不到 IP
    static bool apEventsRegistered = false;
    if (!apEventsRegistered)
    {
        apEventsRegistered = true;
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info)
                     { Serial.printf("[WIFI-EV %lus] client joined %02X:%02X:%02X:%02X:%02X:%02X aid=%u\n",
                                     millis() / 1000,
                                     info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
                                     info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
                                     info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5],
                                     info.wifi_ap_staconnected.aid); }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info)
                     { Serial.printf("[WIFI-EV %lus] client left %02X:%02X:%02X:%02X:%02X:%02X aid=%u\n",
                                     millis() / 1000,
                                     info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
                                     info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
                                     info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5],
                                     info.wifi_ap_stadisconnected.aid); }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info)
                     {
            uint32_t ip = info.wifi_ap_staipassigned.ip.addr;
            Serial.printf("[WIFI-EV %lus] client got IP %u.%u.%u.%u\n", millis() / 1000,
                          ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF); }, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
    }

    dnsServer.start(53, "*", WiFi.softAPIP());

    // ── Route: Portal home page ─────────────────────────────
    webServer.on("/", HTTP_GET, []()
                 {
                     webServer.send_P(200, "text/html", PORTAL_HTML); // 大 HTML 必须用 send_P，send 的 String 转换会因堆不足失败
                 });

    // ── Route: WiFi network scan ────────────────────────────
    // 默认返回进入配网时的缓存结果（热点开着扫描会把手机踢掉）；
    // 只有 ?refresh=1（用户点"扫描"）才现场重扫。
    webServer.on("/scan", HTTP_GET, []()
                 {
        if (webServer.hasArg("refresh") || !portalScanDone) {
            portalScanNetworks();
        }
        webServer.sendHeader("Access-Control-Allow-Origin", "*");
        webServer.send(200, "application/json", buildScanJson());
        Serial.printf("Scan response sent (%d unique networks)\n", portalNetCount); });

    // ── Route: Device info ──────────────────────────────────
    webServer.on("/info", HTTP_GET, []() {
        float v = readBatteryVoltage();
        String json = "{\"mac\":\"" + WiFi.macAddress() + "\",";
        json += "\"battery\":\"" + String(v, 2) + "V\"}";
        webServer.send(200, "application/json", json);
    });

    // ── Route: WiFi connection status ──────────────────────────
    webServer.on("/status", HTTP_GET, []() {
        String json = "{\"state\":\"";
        if (WiFi.status() == WL_CONNECTED) {
            json += "connected\",\"ip\":\"" + WiFi.localIP().toString() + "\"";
        } else if (wifiConnecting) {
            json += "connecting\"";
        } else if (lastWifiError.length() > 0) {
            json += "failed\",\"error\":\"" + lastWifiError + "\"";
        } else {
            json += "idle\"";
        }
        json += "}";
        webServer.sendHeader("Access-Control-Allow-Origin", "*");
        webServer.send(200, "application/json", json);
    });

    // ── Route: Save WiFi credentials (async with polling) ────
    webServer.on("/save_wifi", HTTP_POST, []() {
        String ssid = sanitizeSSID(webServer.arg("ssid"));
        String pass = sanitizeTextInput(webServer.arg("pass"), PORTAL_MAX_PASS);

        Serial.printf("\n--- /save_wifi Request ---\n");
        Serial.printf("SSID: %s\n", ssid.c_str());

        if (ssid.length() == 0) {
            Serial.println("Error: SSID empty");
            webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"SSID empty\"}");
            return;
        }

        if (connectPortalWiFi(ssid, pass)) {
            saveWiFiConfig(ssid, pass);
            sendPortalConnectSuccess();
        } else {
            String msg;
            if (lastWifiError == "NO_SSID")    msg = "找不到该网络";
            else if (lastWifiError == "AUTH_FAIL") msg = "密码错误";
            else                                   msg = "连接超时，请重试";

            Serial.printf("Sending error response: %s\n", msg.c_str());
            webServer.send(200, "application/json",
                           "{\"ok\":false,\"msg\":\"" + msg + "\"}");
        }
    });

    // ── Route: Connect using a saved network password ───────
    webServer.on("/connect_saved", HTTP_POST, []()
                 {
        String ssid = sanitizeSSID(webServer.arg("ssid"));
        webServer.sendHeader("Access-Control-Allow-Origin", "*");

        Serial.printf("\n--- /connect_saved Request ---\n");
        Serial.printf("SSID: %s\n", ssid.c_str());

        if (ssid.length() == 0) {
            webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"SSID empty\"}");
            return;
        }

        String pass;
        if (!findSavedWiFiPassword(ssid, pass)) {
            webServer.send(200, "application/json",
                           "{\"ok\":false,\"msg\":\"NOT_FOUND\",\"list\":" + buildWiFiListJson() + "}");
            return;
        }

        if (connectPortalWiFi(ssid, pass)) {
            // Re-saving an existing SSID promotes it to slot 0.
            addWiFiConfig(ssid, pass);
            sendPortalConnectSuccess();
        } else {
            Serial.println("[PORTAL] Saved network connect failed");
            webServer.send(200, "application/json",
                           "{\"ok\":false,\"msg\":\"SAVED_CONNECT_FAILED\",\"list\":" + buildWiFiListJson() + "}");
        } });

    // ── Route: List saved WiFi networks (names only) ────────
    webServer.on("/wifi_list", HTTP_GET, []() {
        webServer.sendHeader("Access-Control-Allow-Origin", "*");
        webServer.send(200, "application/json", buildWiFiListJson());
    });

    // ── Route: Add a network to the saved list (no connect) ──
    // Used to pre-register secondary networks (office / phone hotspot) that
    // are not reachable from the current location. No restart is triggered.
    webServer.on("/add_wifi", HTTP_POST, []() {
        String ssid = sanitizeSSID(webServer.arg("ssid"));
        String pass = sanitizeTextInput(webServer.arg("pass"), PORTAL_MAX_PASS);
        webServer.sendHeader("Access-Control-Allow-Origin", "*");
        if (ssid.length() == 0) {
            webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"SSID empty\"}");
            return;
        }
        if (addWiFiConfig(ssid, pass)) {
            Serial.printf("[PORTAL] Added saved network: %s\n", ssid.c_str());
            webServer.send(200, "application/json",
                           "{\"ok\":true,\"list\":" + buildWiFiListJson() + "}");
        } else {
            webServer.send(200, "application/json",
                           "{\"ok\":false,\"msg\":\"FULL\"}");
        }
    });

    // ── Route: Delete a saved network by SSID ───────────────
    webServer.on("/delete_wifi", HTTP_POST, []() {
        String ssid = sanitizeSSID(webServer.arg("ssid"));
        bool ok = deleteWiFiBySSID(ssid);
        Serial.printf("[PORTAL] Delete network '%s' -> %s\n", ssid.c_str(), ok ? "ok" : "not found");
        webServer.sendHeader("Access-Control-Allow-Origin", "*");
        webServer.send(200, "application/json",
                       String("{\"ok\":") + (ok ? "true" : "false") +
                       ",\"list\":" + buildWiFiListJson() + "}");
    });

    // ── Route: Manual restart ───────────────────────────────
    webServer.on("/restart", HTTP_POST, []() {
        Serial.println("\n--- /restart Request Received ---");
        webServer.send(200, "application/json", "{\"ok\":true}");
        Serial.println("Manual restart requested, restarting in 1 second...");
        delay(1000);
        ESP.restart();
    });

    webServer.on("/reset_portal", HTTP_POST, []() {
        Serial.println("\n--- /reset_portal Request Received ---");
        resetPortalProvisioningState();
        webServer.send(200, "application/json", "{\"ok\":true}");
        Serial.println("Portal reset requested, staying in provisioning mode");
    });

    // ── Captive portal redirect for all other requests ──────
    webServer.onNotFound([]() {
        String path = webServer.uri();

        // Silently handle captive portal detection URLs
        if (path == "/generate_204" || path == "/gen_204" ||
            path == "/hotspot-detect.html" || path == "/canonical.html" ||
            path == "/success.txt" || path == "/ncsi.txt") {
            webServer.send(204);
            return;
        }

        // Ignore common resource requests
        if (path.endsWith(".ico") || path.endsWith(".png") || path.endsWith(".jpg")) {
            webServer.send(404);
            return;
        }

        // Redirect everything else to portal
        webServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString());
        webServer.send(302, "text/plain", "");
    });

    webServer.begin();
    portalActive = true;
    Serial.println("Captive portal started");
}

// ── Handle pending requests ─────────────────────────────────

void handlePortalClients() {
    dnsServer.processNextRequest();
    webServer.handleClient();

    // Deferred restart after config save
    if (pendingRestart && millis() >= restartAtMillis) {
        Serial.println("Deferred restart triggered");
        delay(200);
        ESP.restart();
    }
}
