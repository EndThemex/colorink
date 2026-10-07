// ColorInk — LAN picture-frame firmware
// Boot: load saved WiFi -> start LAN web service (http://ColorInk.local).
//       The panel is NOT repainted at boot; it keeps whatever it last showed.
// Web:  upload 2bpp (BWRY) framebuffers from the browser, list / display / delete.
//       The panel is repainted only on a web-page action (display / clear).
// Button: short press = next image, long press (2s) = provisioning portal.
// No cloud services: all previous remote API / OTA / AI-voice code removed.
//
// 两套固件，编译期二选一（见 platformio.ini）：
//   * 默认（WiFi）：STA 模式自带 http 网页，浏览器直传图片。走这个文件的下半段。
//   * -DINKSIGHT_BLE：完全不启动 WiFi，只跑 NimBLE；上位机是托管在 HTTPS 的
//     静态页 web/ble_test.html，用 Web Bluetooth 直连。网络相关的代码全部编译掉。

#include <Arduino.h>

#include "config.h"
#include "storage.h"
#include "gallery.h"
#include "epd_driver.h"

#ifdef INKSIGHT_BLE
#include "ble_service.h"
#else
#include <WiFi.h>
#include "portal.h"
#include "webapp.h"
#endif

// ── Shared framebuffers (referenced by other modules via extern) ──
uint8_t imgBuf[IMG_BUF_LEN];
#if EPD_BPP >= 2
uint8_t *colorBuf = nullptr;
bool useColorBuf = false;

bool ensureColorBuf()
{
    if (colorBuf)
        return true;
    colorBuf = (uint8_t *)malloc(COLOR_BUF_LEN);
    if (!colorBuf)
    {
        Serial.println("[MEM] colorBuf alloc failed");
        return false;
    }
    Serial.printf("[MEM] colorBuf allocated %d bytes on heap\n", COLOR_BUF_LEN);
    return true;
}

void freeColorBuf()
{
    if (colorBuf)
    {
        free(colorBuf);
        colorBuf = nullptr;
        Serial.println("[MEM] colorBuf freed");
    }
}
#endif

void netServicePump()
{
#ifdef INKSIGHT_BLE
    // 蓝牙固件没有网页/配网服务，但要在这里把 GATT 回包发出去：epd_driver 的
    // 刷新等待（~15s）里会无条件调用本函数，不泵的话页面在刷新期间收不到
    // display:done，只能干等到超时。
    blePumpReplies();
#else
    if (portalActive)
        handlePortalClients();
    else if (webappRunning())
        webappHandle();
#endif
}

// ── Battery voltage ─────────────────────────────────────────

float readBatteryVoltage()
{
    const int SAMPLES = 16;
    const int DISCARD = 2; // Discard highest and lowest outliers
    int readings[SAMPLES];

    for (int i = 0; i < SAMPLES; i++)
    {
        readings[i] = analogRead(PIN_BAT_ADC);
        delayMicroseconds(100);
    }

    // Sort for outlier removal
    for (int i = 0; i < SAMPLES - 1; i++)
        for (int j = i + 1; j < SAMPLES; j++)
            if (readings[i] > readings[j])
            {
                int tmp = readings[i];
                readings[i] = readings[j];
                readings[j] = tmp;
            }

    // Average middle readings
    long sum = 0;
    for (int i = DISCARD; i < SAMPLES - DISCARD; i++)
        sum += readings[i];

    float avgRaw = (float)sum / (SAMPLES - 2 * DISCARD);
    // R1=10k / R2=10k 分压，所以实测电压是引脚读数的两倍
    float realBatteryVoltage = avgRaw * (3.3f / 4095.0f) * 2.0f;
    Serial.printf("[BAT] raw=%.1f vbat=%.2fV\n", avgRaw, realBatteryVoltage);
    return realBatteryVoltage;
}

// ── LED feedback ────────────────────────────────────────────

static void ledInit()
{
#if PIN_LED >= 0
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
#endif
#if PIN_RGB_LED >= 0
    neopixelWrite(PIN_RGB_LED, 0, 0, 0);
#endif
}

// LED 反馈语义只属于 WiFi 固件：BLE 固件里 LED 由 bleModeLoop 驱动
// （慢闪 = 广播中 / 常亮 = 已连接），两边同时写会打架。
#ifndef INKSIGHT_BLE
static void ledFeedback(const char *pattern)
{
#if PIN_LED < 0
    (void)pattern;
    return;
#else
    if (strcmp(pattern, "portal") == 0)
    {
        digitalWrite(PIN_LED, HIGH); // solid while portal is open
    }
    else if (strcmp(pattern, "off") == 0)
    {
        digitalWrite(PIN_LED, LOW);
    }
    else if (strcmp(pattern, "ack") == 0)
    {
        for (int i = 0; i < 2; i++)
        {
            digitalWrite(PIN_LED, HIGH);
            delay(80);
            digitalWrite(PIN_LED, LOW);
            delay(80);
        }
    }
#endif
}
#endif // !INKSIGHT_BLE

// ── Portal mode ─────────────────────────────────────────────

#ifndef INKSIGHT_BLE
// 配网热点和 WiFi 看门狗只在 WiFi 固件里存在：蓝牙固件全程不碰射频的 WiFi 侧。
enum class PortalEntryReason : uint8_t
{
    MANUAL,
    AUTO_WIFI_FAILURE,
};

static unsigned long portalStartedAt = 0;
static unsigned long portalTimeoutMs = 0;

static void enterPortalMode(PortalEntryReason reason)
{
    unsigned long t0 = millis();

    // AP 名必须在关 WiFi / 切模式之前取（旧版能用的配网就是这个顺序），
    // 并且只算一次：热点 SSID 和屏幕上显示的名字必须完全一致。
    String mac = WiFi.macAddress();
    String apName = "ColorInk-" + mac.substring(mac.length() - 5);
    apName.replace(":", "");

    webappStop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    ledFeedback("portal");
    portalPreScan(); // 先扫描（热点还没起来，不会踢掉手机），配网页加载直接读缓存
    startCaptivePortal(apName.c_str());
    Serial.printf("[PORTAL] AP '%s' visible at t=%lums\n", apName.c_str(), millis() - t0);

    // 屏幕只由网页控制：配网时也不刷屏，面板保留上一次的画面
    freeColorBuf(); // 配网期间用不到帧缓冲，释放 105KB 堆给 lwIP（DNS/HTTP）

    portalStartedAt = millis();
    portalTimeoutMs = (reason == PortalEntryReason::AUTO_WIFI_FAILURE)
                          ? PORTAL_AUTO_TIMEOUT_MS
                          : PORTAL_MANUAL_TIMEOUT_MS;
    Serial.printf("[PORTAL] %s portal timeout: %lus\n",
                  reason == PortalEntryReason::AUTO_WIFI_FAILURE ? "Auto" : "Manual",
                  portalTimeoutMs / 1000UL);
}

static void checkPortalTimeout()
{
    if (portalStartedAt == 0)
        return;
    if (WiFi.softAPgetStationNum() > 0)
    {
        // 有设备连着热点：暂停超时，避免配网中途被重启踢掉
        portalStartedAt = millis();
        return;
    }
    if (millis() - portalStartedAt < portalTimeoutMs)
        return;
    Serial.println("[PORTAL] timeout, restarting...");
    delay(200);
    ESP.restart();
}
#endif // !INKSIGHT_BLE

// ── WiFi watchdog (server mode) ─────────────────────────────

#ifndef INKSIGHT_BLE
static unsigned long wifiDownSince = 0;
static unsigned long lastReconnectAttempt = 0;
static int reconnectFails = 0;

static void handleWiFiWatchdog()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        wifiDownSince = 0;
        reconnectFails = 0;
        return;
    }
    if (wifiDownSince == 0)
        wifiDownSince = millis();
    if (millis() - lastReconnectAttempt < (unsigned long)LIVE_WIFI_RETRY_MS)
        return;
    lastReconnectAttempt = millis();
    Serial.println("[NET] WiFi lost, reconnecting...");
    // 运行中掉线多数是短暂抖动：用更短的预算快速试，试不通就早点开配网热点，
    // 别让"路由器关机了"这种情况把 AP 拖到几分钟后才出现。
    if (connectWiFiSTA(6000, 9000))
    {
        Serial.printf("[NET] reconnected  IP=%s\n", WiFi.localIP().toString().c_str());
        wifiDownSince = 0;
    }
    else if (++reconnectFails >= 3)
    {
        Serial.println("[NET] reconnect failed 3 times, entering portal");
        enterPortalMode(PortalEntryReason::AUTO_WIFI_FAILURE);
    }
}
#endif // !INKSIGHT_BLE

// ── Button ──────────────────────────────────────────────────

static void handleButton()
{
    static bool pressed = false;
    static unsigned long pressStart = 0;

    bool down = digitalRead(PIN_CFG_BTN) == LOW;
    if (down && !pressed)
    {
        pressed = true;
        pressStart = millis();
        return;
    }
    if (down && pressed)
    {
        if (millis() - pressStart >= (unsigned long)CFG_BTN_HOLD_MS)
        {
            pressed = false;
#ifdef INKSIGHT_BLE
            // 这颗板子只有一个按键（GPIO9，即 BOOT 键），长按留给"强制释放 BLE
            // 连接"这个兜底：手机端卡死时设备端否则没辙。LED 由 bleModeLoop 驱动，
            // 这里不碰它，免得和状态闪烁打架。
            bleForceDisconnect();
#else
            ledFeedback("off");
            enterPortalMode(PortalEntryReason::MANUAL);
#endif
        }
        return;
    }
    if (!down && pressed)
    {
        pressed = false;
        unsigned long dur = millis() - pressStart;
        if (dur >= (unsigned long)SHORT_PRESS_MIN_MS && dur < (unsigned long)CFG_BTN_HOLD_MS)
        {
#ifdef INKSIGHT_BLE
            // BLE 固件里按键不主动刷屏：一次刷新要阻塞 ~15s，期间 loop() 停转、
            // notify 发不出去，正在传图的链路会被拖成假死。刷屏只由 BLE 命令触发。
            Serial.println("[BTN] short press ignored (刷新只由 BLE 命令触发)");
#else
            if (galleryCount() > 0)
            {
                galleryCycle(1); // short press: next image (blocks ~15s)
            }
            else
            {
                ledFeedback("ack");
            }
#endif
        }
    }
}

// ── Heap watchdog ───────────────────────────────────────────
// 长期运行的内存体检，每分钟打一行。三个数一起看：
//   free     —— 当前余量。稳态应是一条水平线；持续单调下滑 = 有泄漏
//   min      —— 开机以来的最低水位。只要不再刷新新低，就没有累积占用
//   maxalloc —— 最大连续可分配块，碎片化指标。它缩水比 free 缩水更危险，
//               意味着后面 105KB 级别的分配随时可能失败
// 注意：开机常驻 105KB colorBuf，所以 maxalloc 只有几十 KB 是正常的。
static void logHeap()
{
    static unsigned long lastLog = 0;
    static uint32_t lastFree = 0;
    if (lastLog != 0 && millis() - lastLog < 60000UL)
        return;
    lastLog = millis();
    uint32_t freeNow = ESP.getFreeHeap();
    Serial.printf("[MEM] free=%u min=%u maxalloc=%u  delta=%+d  up=%lum\n",
                  (unsigned)freeNow, (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  lastFree ? (int)freeNow - (int)lastFree : 0,
                  millis() / 60000UL);
    lastFree = freeNow;
}

// ── Server mode ─────────────────────────────────────────────

void setup()
{
    unsigned long bootT0 = millis();
    Serial.begin(115200);
    delay(150);
    Serial.printf("\n[BOOT] ColorInk LAN gallery  %s %s\n", __DATE__, __TIME__);
    Serial.printf("[BOOT] free heap: %u\n", (unsigned)ESP.getFreeHeap());

    ledInit();
    pinMode(PIN_CFG_BTN, INPUT_PULLUP);
    gpioInit();

    loadConfig();
    galleryInit();
    Serial.printf("[BOOT] storage ready t=%lums\n", millis() - bootT0);

    // 预分配 105KB 帧缓冲：此时 WiFi/lwIP 还没起，堆最宽裕，保证一次性拿到
    // 连续内存。屏幕刷新后 colorBuf 常驻不再释放，运行期刷新不再依赖大块
    // malloc（上传过图片后堆碎片化，运行期大分配可能失败 → 刷新静默失效）。
    ensureColorBuf();

#ifdef INKSIGHT_BLE
    // 蓝牙固件：WiFi 一次都不启动（WiFi.mode / WiFi.begin 全程不调用），
    // 射频只服务 NimBLE，避开 C3 单天线的 WiFi/BLE 共存降速与发热。
    Serial.println("[BOOT] BLE-only build: WiFi stays off");
    bleModeStart();
    Serial.printf("[BOOT] ready t=%lums heap=%u\n",
                  millis() - bootT0, (unsigned)ESP.getFreeHeap());
    return;
#else
    bool btnHeld = digitalRead(PIN_CFG_BTN) == LOW;

    if (!btnHeld && getWiFiCount() > 0)
    {
        if (connectWiFiSTA())
        {
            Serial.printf("[BOOT] STA up t=%lums\n", millis() - bootT0);
            webappStart();
            Serial.printf("[BOOT] ready t=%lums\n", millis() - bootT0);
            ledFeedback("off");
            return;
        }
        Serial.println("[NET] WiFi connect failed, entering portal");
    }
    else if (btnHeld)
    {
        Serial.println("[BOOT] button held at boot, entering portal");
    }
    else
    {
        Serial.println("[NET] no saved WiFi, entering portal");
    }
    enterPortalMode(btnHeld ? PortalEntryReason::MANUAL
                            : PortalEntryReason::AUTO_WIFI_FAILURE);
#endif // INKSIGHT_BLE
}

void loop()
{
#ifdef INKSIGHT_BLE
    bleModeLoop(); // 把 GATT 回调排队的回包发出去
    handleButton();
    logHeap();
    delay(2); // C3 单核：让出 CPU 给 idle 和 NimBLE 主机任务
    return;
#else
    if (portalActive)
    {
        handlePortalClients();
        checkPortalTimeout();
        delay(2); // 配网时射频常开 + modem sleep 关闭，CPU 再空转就是纯加热
        return;
    }
    webappHandle();
    webappProcessPending(); // 执行网页排队的刷屏/清屏（~15s，期间泵 HTTP 服务）
    handleWiFiWatchdog();
    handleButton();
    logHeap();

    // ESP32-C3 是单核：loop 任务全速空转会一直占着 CPU，idle 任务没机会跑，
    // 芯片既不能降频也进不了空闲态，射频开着时整颗芯片持续发热。让出 2ms
    // 对 HTTP 几乎无影响（lwIP 收包由协议栈自己完成，这里只是搬运速度），
    // 上传 106KB 也仅多花百毫秒级。
    delay(2);
#endif // INKSIGHT_BLE
}
