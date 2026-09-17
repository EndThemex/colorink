#include "webapp.h"
#include "config.h"
#include "storage.h"
#include "gallery.h"
#include "epd_driver.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#include "../data/webapp_html.h"

static WebServer webServer(80);
static bool serverRunning = false;

// Upload state (multipart handler runs across several callbacks)
static int uploadId = -1;
static String uploadErr;

// ── WiFi station connection ─────────────────────────────────

bool connectWiFiSTA(unsigned long perNetTimeoutMs, unsigned long budgetMs) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);  // modem sleep: lower power/heat while idle

    int count = getWiFiCount();
    if (count == 0) return false;

    unsigned long budgetStart = millis();
    for (int i = 0; i < count; i++) {
        String ssid, pass;
        if (!getWiFiAt(i, ssid, pass)) continue;

        long left = (long)budgetMs - (long)(millis() - budgetStart);
        if (left <= 0) {
            Serial.printf("[NET] budget %lums spent, %d network(s) untried\n",
                          budgetMs, count - i);
            break;
        }
        unsigned long timeout = perNetTimeoutMs < (unsigned long)left
                              ? perNetTimeoutMs : (unsigned long)left;

        Serial.printf("[NET] connecting to %s (%d/%d, %lums, t=%lums)\n",
                      ssid.c_str(), i + 1, count, timeout, millis() - budgetStart);
        WiFi.begin(ssid.c_str(), pass.c_str());

        unsigned long t0 = millis();
        while (millis() - t0 < timeout) {
            wl_status_t st = WiFi.status();
            if (st == WL_CONNECTED) {
                Serial.printf("[NET] WiFi OK  IP=%s  RSSI=%d  (%lums)\n",
                              WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                              millis() - budgetStart);
                return true;
            }
            // AP 不在范围 / 认证失败：继续等满超时也没意义，立刻换下一个。
            // 前 400ms 可能还是上一次连接残留的状态，忽略以免误判。
            if (millis() - t0 > 400 && (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED)) {
                break;
            }
            delay(100);
        }
        Serial.printf("[NET] %s failed (status %d, %lums)\n",
                      ssid.c_str(), (int)WiFi.status(), millis() - t0);
        WiFi.disconnect();
    }
    Serial.printf("[NET] STA connect gave up after %lums\n", millis() - budgetStart);
    return false;
}

// ── JSON helpers ────────────────────────────────────────────

static String jsonEscape(const String &s) {
    String out;
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else out += c;
    }
    return out;
}

static void sendJson(int code, const String &json) {
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(code, "application/json", json);
}

static void sendError(const String &msg) {
    sendJson(200, "{\"ok\":false,\"msg\":\"" + jsonEscape(msg) + "\"}");
}

// ── Route handlers ──────────────────────────────────────────

static void handleRoot() {
    webServer.sendHeader("Cache-Control", "no-cache");
    webServer.send_P(200, "text/html", WEBAPP_HTML);
}

static void handleImages() {
    sendJson(200, galleryListJson());
}

static void handleStatus() {
    float v = readBatteryVoltage();
    String json = "{\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"mdns\":\"inksight.local\"";
    json += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
    json += ",\"rssi\":" + String(WiFi.RSSI());
    json += ",\"heap\":" + String((unsigned)ESP.getFreeHeap());
    json += ",\"battery\":\"" + String(v, 2) + "V\"";
    json += ",\"images\":" + String(galleryCount());
    json += ",\"current\":" + String(galleryCurrentId());
    json += ",\"free_fs\":" + String((unsigned)galleryFreeBytes());
    json += "}";
    sendJson(200, json);
}

// Streamed multipart upload: the file part is written straight to LittleFS.
static void handleImageUpload() {
    HTTPUpload &up = webServer.upload();
    switch (up.status) {
        case UPLOAD_FILE_START: {
            uploadId = -1;
            uploadErr = "";
            String name = webServer.hasArg("name") ? webServer.arg("name") : String(up.filename.c_str());
            // Strip a trailing ".png/.jpg/..." if the filename was used
            int dot = name.lastIndexOf('.');
            if (dot > 0 && name.length() - dot <= 5) name = name.substring(0, dot);
            int id = -1;
            if (!galleryUploadStart(id, name, &uploadErr)) {
                Serial.printf("[WEB] upload rejected: %s\n", uploadErr.c_str());
            } else {
                uploadId = id;
            }
            break;
        }
        case UPLOAD_FILE_WRITE:
            if (uploadId >= 0) {
                galleryUploadData(up.buf, up.currentSize);
            }
            break;
        case UPLOAD_FILE_END:
            if (uploadId >= 0) {
                String name = webServer.hasArg("name") ? webServer.arg("name") : String(up.filename.c_str());
                int id = galleryUploadEnd(name, &uploadErr);
                uploadId = (id >= 0) ? id : -1;
            }
            break;
        default:
            break;
    }
}

static void handleImagePost() {
    // Final handler: runs after all multipart parts are parsed.
    if (uploadId >= 0) {
        Serial.printf("[WEB] upload ok id=%d\n", uploadId);
        sendJson(200, "{\"ok\":true,\"id\":" + String(uploadId) + "}");
    } else {
        sendError(uploadErr.length() ? uploadErr : "上传失败");
    }
    uploadId = -1;
    uploadErr = "";
}

static void handleDisplay() {
    if (!webServer.hasArg("id")) {
        sendError("缺少 id 参数");
        return;
    }
    int id = webServer.arg("id").toInt();
    if (!galleryExists(id)) {
        sendError("图片不存在");
        return;
    }
    // A refresh may already be running (e.g. the boot-time repaint) — nesting
    // two panel refreshes would corrupt the panel state.
    if (epdIsRendering()) {
        sendError("屏幕正在刷新，请稍候再试");
        return;
    }
    // Respond first — the panel refresh blocks for ~15s
    sendJson(200, "{\"ok\":true,\"refreshing\":true,\"id\":" + String(id) + "}");
    delay(150);
    netPumpBlocked = true;   // we are inside a handler: no re-entrant HTTP
    galleryDisplayById(id);
    netPumpBlocked = false;
}

static void handleImageDelete() {
    if (!webServer.hasArg("id")) {
        sendError("缺少 id 参数");
        return;
    }
    int id = webServer.arg("id").toInt();
    if (galleryDeleteById(id)) {
        sendJson(200, "{\"ok\":true,\"deleted\":" + String(id) + "}");
    } else {
        sendError("删除失败：图片不存在");
    }
}

static void handleClear() {
    if (epdIsRendering()) {
        sendError("屏幕正在刷新，请稍候再试");
        return;
    }
    sendJson(200, "{\"ok\":true,\"refreshing\":true}");
    delay(150);
    netPumpBlocked = true;
    galleryClearScreen();
    netPumpBlocked = false;
}

// ── Lifecycle ───────────────────────────────────────────────

void webappStart() {
    if (serverRunning) return;

    if (!MDNS.begin("inksight")) {
        Serial.println("[WEB] mDNS start failed (IP access still works)");
    } else {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[WEB] mDNS: http://inksight.local");
    }

    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/api/images", HTTP_GET, handleImages);
    webServer.on("/api/status", HTTP_GET, handleStatus);
    webServer.on("/api/image", HTTP_POST, handleImagePost, handleImageUpload);
    webServer.on("/api/display", HTTP_POST, handleDisplay);
    webServer.on("/api/image", HTTP_DELETE, handleImageDelete);
    webServer.on("/api/clear", HTTP_POST, handleClear);

    webServer.onNotFound([]() {
        Serial.printf("[WEB] 404: %s %s\n",
                      webServer.method() == HTTP_GET ? "GET" : webServer.method() == HTTP_DELETE ? "DELETE" : "POST",
                      webServer.uri().c_str());
        sendError("未找到接口: " + webServer.uri());
    });

    webServer.begin();
    serverRunning = true;
    Serial.printf("[WEB] HTTP server on port 80, http://%s\n",
                  WiFi.localIP().toString().c_str());
}

void webappStop() {
    if (!serverRunning) return;
    webServer.close();
    serverRunning = false;
    MDNS.end();
    Serial.println("[WEB] HTTP server stopped");
}

bool webappRunning() {
    return serverRunning;
}

void webappHandle() {
    if (serverRunning) {
        webServer.handleClient();
    }
}
