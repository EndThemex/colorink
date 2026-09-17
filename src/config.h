#ifndef INKSIGHT_CONFIG_H
#define INKSIGHT_CONFIG_H

#include <Arduino.h>

#if defined(BOARD_PROFILE_ESP32_C3)
#define PIN_EPD_MOSI   6
#define PIN_EPD_SCK    4
#define PIN_EPD_CS     7
#define PIN_EPD_DC     1
#define PIN_EPD_RST    2
#define PIN_EPD_BUSY   10
#define PIN_BAT_ADC    0
#define PIN_CFG_BTN    9
#define PIN_LED        3
#elif defined(BOARD_PROFILE_ESP32_C3_HUAWEI)
// 华为 3.98" 768x552 JD79665 手机壳屏（原装小板飞线接线）
#define PIN_EPD_MOSI 6 // DIN
#define PIN_EPD_SCK 4  // CLK
#define PIN_EPD_CS 7   // CS
#define PIN_EPD_DC 2   // DC
#define PIN_EPD_RST 10 // RST
#define PIN_EPD_BUSY 1 // BUSY
#define PIN_BAT_ADC 0
#define PIN_CFG_BTN 9
#define PIN_LED 3
#elif defined(BOARD_PROFILE_ESP32_C3_WROOM02)
#define PIN_EPD_MOSI   6
#define PIN_EPD_SCK    4
#define PIN_EPD_CS     7
#define PIN_EPD_DC     1
#define PIN_EPD_RST    2
#define PIN_EPD_BUSY   10
#define PIN_BAT_ADC    0
#define PIN_CFG_BTN    9
#define PIN_LED        5
#elif defined(BOARD_PROFILE_ESP32_WROOM32E)
#define PIN_EPD_MOSI   14
#define PIN_EPD_SCK    13
#define PIN_EPD_CS     15
#define PIN_EPD_DC     27
#define PIN_EPD_RST    26
#define PIN_EPD_BUSY   25
#define PIN_BAT_ADC    35
#define PIN_CFG_BTN    0
#define PIN_LED        2
#define BOARD_HAS_AUDIO
#elif defined(BOARD_PROFILE_SMT_WROOM32E)
#define PIN_EPD_MOSI   14
#define PIN_EPD_SCK    13
#define PIN_EPD_CS     15
#define PIN_EPD_DC     27
#define PIN_EPD_RST    26
#define PIN_EPD_BUSY   25
#define PIN_BAT_ADC    35
#define PIN_CFG_BTN    0
#define PIN_LED        2
#define BOARD_HAS_AUDIO
#elif defined(BOARD_PROFILE_SMT_C3)
#define PIN_EPD_MOSI   6
#define PIN_EPD_SCK    4
#define PIN_EPD_CS     7
#define PIN_EPD_DC     1
#define PIN_EPD_RST    2
#define PIN_EPD_BUSY   10
#define PIN_BAT_ADC    0
#ifndef PIN_CFG_BTN
#define PIN_CFG_BTN    9
#endif
#define PIN_LED        5
#elif defined(BOARD_PROFILE_YD_ESP32_S3_N16R8)
#define PIN_EPD_MOSI   11
#define PIN_EPD_SCK    12
#define PIN_EPD_CS     10
#define PIN_EPD_DC     9
#define PIN_EPD_RST    8
#define PIN_EPD_BUSY   7
#define PIN_BAT_ADC    4
#define PIN_CFG_BTN    0
#define PIN_LED        -1
#define PIN_RGB_LED    48
#else
#error "Unsupported board profile"
#endif

#ifndef PIN_RGB_LED
#define PIN_RGB_LED -1
#endif

// ── Display constants ────────────────────────────────────────
// Default for 4.2" E-Paper (400x300, 1-bit).
// Override via build flags: -D EPD_WIDTH=800 -D EPD_HEIGHT=480
// Supported configurations:
//   4.2"  (400x300) - default
//   2.9"  (296x128)
//   5.83" (648x480)
//   7.5"  (800x480)
#ifndef EPD_WIDTH
#define EPD_WIDTH  400
#endif
#ifndef EPD_HEIGHT
#define EPD_HEIGHT 300
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
void freeColorBuf();
#endif

// Shared helper (defined in main.cpp)
float readBatteryVoltage();

// ── Refresh strategy ─────────────────────────────────────────
static const int FULL_REFRESH_INTERVAL = 10;  // Full refresh every N updates to clear ghosting

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
// WiFi -> captive portal fallback: when ALL saved networks fail to connect,
// do this many quick in-place retry sweeps (no reboot) before opening the AP.
// Keeps the portal fast to appear (user is likely waiting to reconfigure)
// while still riding out a brief blip such as a router rebooting.
static const int           WIFI_PORTAL_RETRY_SWEEPS   = 1;
static const unsigned long WIFI_PORTAL_RETRY_DELAY_MS = 3000;

#endif // INKSIGHT_CONFIG_H
