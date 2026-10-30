#ifndef INKSIGHT_CONFIG_H
#define INKSIGHT_CONFIG_H

#include <Arduino.h>

// 只支持这一块板子：ESP32-C3 SuperMini 飞线华为 3.98" 手机壳屏。
// platformio.ini 里所有环境都定义 BOARD_PROFILE_ESP32_C3_HUAWEI。
#if defined(BOARD_PROFILE_ESP32_C3_HUAWEI)
// 华为 3.98" 768x552 JD79665 手机壳屏（原装小板飞线接线）
#define PIN_EPD_MOSI 3 // DIN
#define PIN_EPD_SCK 4  // CLK
#define PIN_EPD_CS 5   // CS
#define PIN_EPD_DC 6   // DC
#define PIN_EPD_RST 7  // RST
#define PIN_EPD_BUSY 10 // BUSY
#define PIN_BAT_ADC 0
#define PIN_CFG_BTN 2
#define PIN_LED 1
#else
#error "Unsupported board profile. Define BOARD_PROFILE_ESP32_C3_HUAWEI"
#endif

#ifndef PIN_RGB_LED
#define PIN_RGB_LED -1
#endif

// ── Display constants ────────────────────────────────────────
// 面板固定 768x552 四色（2bpp）。platformio.ini 里仍显式传 -DEPD_WIDTH/HEIGHT，
// 这里保留同名默认值，只为漏传时不至于编到错的尺寸。
#ifndef EPD_WIDTH
#define EPD_WIDTH 768
#endif
#ifndef EPD_HEIGHT
#define EPD_HEIGHT 552
#endif

static const int W = EPD_WIDTH;
static const int H = EPD_HEIGHT;
static const int ROW_BYTES   = W / 8;
static const int ROW_STRIDE  = (ROW_BYTES + 3) & ~3;  // BMP row stride (4-byte aligned)
static const int IMG_BUF_LEN = ROW_BYTES * H;
/** Preprocessor image buffer size for #if (IMG_BUF_LEN is not a cpp constant). */
#define INKSIGHT_IMG_BUF_BYTES_MACRO ((EPD_WIDTH / 8) * (EPD_HEIGHT))

#ifndef EPD_BPP
#define EPD_BPP 1
#endif
static const int COLOR_BUF_LEN = (W * H) / 4;  // 2bpp: 4 pixels per byte

// Shared framebuffers (defined in main.cpp)
extern uint8_t imgBuf[];
#if EPD_BPP >= 2
extern uint8_t *colorBuf;
extern bool useColorBuf;
bool ensureColorBuf();
#endif

// Shared helper (defined in main.cpp)
float readBatteryVoltage();

// ── Config defaults ─────────────────────────────────────────
static const int   WIFI_TIMEOUT    = 15000;   // ms (portal: single explicit connect)
static const int   MAX_WIFI_NETWORKS = 5;     // Max saved WiFi credentials (tried in order on boot)
// STA 连接（开机/重连）：单个网络的最长等待 + 全部网络的总预算。
// 没有预算时 5 个保存的网络逐个死等会拖到 1 分钟，配网热点迟迟看不见。
static const unsigned long WIFI_STA_ATTEMPT_MS = 8000;
static const unsigned long WIFI_STA_BUDGET_MS  = 15000;
static const int   CFG_BTN_HOLD_MS = 2000;    // Long press duration to trigger config mode
static const int   SHORT_PRESS_MIN_MS = 50;   // Minimum short press duration (debounce)
static const int   LIVE_WIFI_RETRY_MS = 5000; // Retry interval when WiFi is disconnected
static const unsigned long PORTAL_AUTO_TIMEOUT_MS = 3UL * 60UL * 1000UL;
static const unsigned long PORTAL_MANUAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;

#endif // INKSIGHT_CONFIG_H
