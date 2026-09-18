#include "epd_driver.h"
#include "config.h"

#if defined(EPD_PANEL_JD79665)
// ── Huawei 3.98" 768x552 BWRY panel (JD79665 controller, hardware SPI) ──
// Driver ported from OpenEPaperLink CN fork (krstc/openepaperlinkforCN, tag_fw/src/epd_driver/jd79665.cpp).
// 2bpp packing (MSB first, 4 px/byte): 00=black, 01=white, 10=yellow, 11=red
//   — same convention the server already uses for the other 4-color panels.
// BUSY pin: HIGH = idle/ready. BS pin (if wired) must be LOW for 4-wire SPI,
//   optionally drive it via -DPIN_EPD_BS=<gpio>.

#include <stdarg.h>
#include <SPI.h>
#include "webapp.h" // 刷新期间泵 HTTP/配网服务，避免长时间收不到请求

// 硬件 SPI（FSPI，引脚经 GPIO 矩阵任意映射）：4MHz，写 105KB 帧数据约 0.2s
static SPIClass *epdSpi = nullptr;

static void spiWriteByte(uint8_t data)
{
    epdSpi->transfer(data);
}

static void epdSendCommand(uint8_t cmd)
{
    digitalWrite(PIN_EPD_DC, LOW); // DC low = command
    digitalWrite(PIN_EPD_CS, LOW);
    spiWriteByte(cmd);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void epdSendData(uint8_t data)
{
    digitalWrite(PIN_EPD_DC, HIGH); // DC high = data
    digitalWrite(PIN_EPD_CS, LOW);
    spiWriteByte(data);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void epdSendDataBulk(const uint8_t *buf, uint32_t len)
{
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    epdSpi->transferBytes(buf, nullptr, len);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void epdSendCmdData(uint8_t cmd, int len, ...)
{
    epdSendCommand(cmd);
    va_list ap;
    va_start(ap, len);
    while (len--)
        epdSendData((uint8_t)va_arg(ap, int));
    va_end(ap);
}

// 一次事务写完命令 + 参数：窗口设置每行都要发一次，逐字节 transfer 的开销会累积
static void epdSendCmdBulk(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    epdSendCommand(cmd);
    epdSendDataBulk(data, len);
}

// 正在刷新标志：防止并发刷新（例如开机重绘期间收到的 /api/display 请求）
static volatile bool epdRendering = false;

bool epdIsRendering()
{
    return epdRendering;
}

static void epdWaitBusy(unsigned long maxMs = 0)
{
    unsigned long t0 = millis();
    unsigned long timeoutMs = maxMs > 0 ? maxMs : 60000;
    while (digitalRead(PIN_EPD_BUSY) == LOW)
    {
        netServicePump(); // 刷新期间保持网页/配网可访问
        delay(10);
        if (millis() - t0 > timeoutMs)
        {
            Serial.println("EPD busy TIMEOUT!");
            return;
        }
    }
}

static bool jdPowerIsOn = false;

static void jdPowerOn()
{
    epdSendCommand(0x04);
    epdWaitBusy(10000);
    jdPowerIsOn = true;
}

static void jdPowerOff()
{
    epdSendCommand(0x02);
    epdSendData(0x00);
    epdWaitBusy(10000);
    jdPowerIsOn = false;
}

static void epdSetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t xe = x + w - 1;
    uint16_t ye = y + h - 1;
    const uint8_t win[9] = {
        (uint8_t)(x >> 8), (uint8_t)(x & 0xFF),
        (uint8_t)(xe >> 8), (uint8_t)(xe & 0xFF),
        (uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
        (uint8_t)(ye >> 8), (uint8_t)(ye & 0xFF),
        0x01};
    epdSendCmdBulk(0x83, win, sizeof(win));
}

static void epdReset()
{
#if defined(PIN_EPD_BS) && PIN_EPD_BS >= 0
    pinMode(PIN_EPD_BS, OUTPUT);
    digitalWrite(PIN_EPD_BS, LOW); // 4-wire SPI mode
#endif
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
    digitalWrite(PIN_EPD_RST, LOW);
    delay(2);
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(20);
    epdWaitBusy(10000);
    Serial.printf("[EPD] after reset BUSY=%d\n", digitalRead(PIN_EPD_BUSY));
    delay(30);
}

// ── GPIO initialization ─────────────────────────────────────

void gpioInit()
{
    pinMode(PIN_EPD_BUSY, INPUT);
    pinMode(PIN_EPD_RST, OUTPUT);
    pinMode(PIN_EPD_DC, OUTPUT);
    pinMode(PIN_EPD_CS, OUTPUT);
    pinMode(PIN_CFG_BTN, INPUT_PULLUP);
    digitalWrite(PIN_EPD_RST, HIGH);
    digitalWrite(PIN_EPD_CS, HIGH);
    epdSpi = new SPIClass(FSPI);
    epdSpi->begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, -1);
    epdSpi->beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    Serial.printf("[EPD] gpio init (HW SPI), BUSY pin=%d level=%d\n", PIN_EPD_BUSY, digitalRead(PIN_EPD_BUSY));
}

// ── EPD full init (JD79665 register setup + power on) ──

void epdInit()
{
    epdReset();

    epdSendCmdData(0xAA, 6, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18); // CMDH (unlock)
    epdSendCmdData(0x01, 1, 0x3F);                               // power setting
    epdSendCmdData(0x00, 2, 0x4B, 0x69);                         // panel setting
    epdSendCmdData(0x05, 4, 0x40, 0x1F, 0x1F, 0x2C);
    epdSendCmdData(0x08, 4, 0x6F, 0x1F, 0x1F, 0x22);
    epdSendCmdData(0x06, 4, 0x6F, 0x1F, 0x14, 0x14); // booster soft start
    epdSendCmdData(0x03, 4, 0x00, 0x54, 0x00, 0x44);
    epdSendCmdData(0x60, 2, 0x02, 0x00);                         // TCON setting
    epdSendCmdData(0x30, 1, 0x08);                               // PLL control
    epdSendCmdData(0x50, 1, 0x3F);                               // VCOM interval
    epdSendCmdData(0x61, 4, W >> 8, W & 0xFF, H >> 8, H & 0xFF); // resolution
    epdSendCmdData(0x65, 4, 0x10, 0x00, 0x20, 0x00);             // GSST setting
    epdSendCmdData(0xE3, 1, 0x2F);
    epdSendCmdData(0x84, 1, 0x01);

    jdPowerIsOn = false;
    jdPowerOn();
}

// ── EPD fast init (no separate fast waveform for this panel) ──

void epdInitFast() { epdInit(); }

// ── 2bpp write: one 0x83 window per row, then data stream ──

static void jdWrite2bpp(const uint8_t *buf)
{
    const int rowBytes = W / 4;
    for (int y = 0; y < H; y++)
    {
        // 写数据要 ~2s，期间也泵一次网络服务，网页不至于整段卡死
        if ((y & 63) == 0)
            netServicePump();
        epdSetWindow(0, y, W, 1);
        epdSendCommand(0x10);
        epdSendDataBulk(buf + y * rowBytes, rowBytes);
    }
}

static uint8_t jdPackMonoPixelGroup(const uint8_t *image, int rowBytes, int y, int x)
{
    uint8_t packed = 0;
    for (int bit = 0; bit < 4; bit++)
    {
        int px = x + bit;
        bool isBlack = (image[y * rowBytes + px / 8] & (0x80 >> (px % 8))) == 0;
        packed |= (isBlack ? 0x00 : 0x01) << (6 - bit * 2);
    }
    return packed;
}

// ── BW full refresh (converted to 2bpp) ──

void epdDisplay(const uint8_t *image)
{
    if (!ensureColorBuf())
    {
        Serial.println("[EPD] colorBuf alloc failed");
        return;
    }
    int rowBytes = W / 8;
    int out = 0;
    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x += 4)
        {
            colorBuf[out++] = jdPackMonoPixelGroup(image, rowBytes, y, x);
        }
    }
    epdDisplay2bpp(colorBuf);
}

// ── 2bpp color refresh ──

void epdDisplay2bpp(const uint8_t *image2bpp)
{
    if (epdRendering)
    {
        Serial.println("[EPD] refresh already in progress, request ignored");
        return;
    }
    epdRendering = true;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        unsigned long t0 = millis();
        Serial.printf("[EPD] attempt %d start BUSY=%d\n", attempt, digitalRead(PIN_EPD_BUSY));
        epdInit();
        Serial.printf("[EPD] init done %lums BUSY=%d\n", millis() - t0, digitalRead(PIN_EPD_BUSY));

        jdWrite2bpp(image2bpp);
        Serial.printf("[EPD] data done %lums\n", millis() - t0);

        epdSetWindow(0, 0, W, H);
        epdSendCommand(0x12);
        epdSendData(0x00);
        epdWaitBusy(60000);
        Serial.printf("[EPD] refresh done %lums\n", millis() - t0);

        jdPowerOff();
        Serial.printf("[EPD] all done %lums\n", millis() - t0);
        // colorBuf 保持常驻（开机时预分配，见 main.cpp setup）：运行期重新
        // malloc 105KB 连续内存在上传过图片后容易因堆碎片失败，导致刷新静默失败
        epdRendering = false;
        return;
    }
    epdRendering = false;
    Serial.println("[EPD] display failed after 3 attempts");
}

// ── Deep clear: no register-level deep clear, fall back to plain display ──

void epdDisplayDeepClear(const uint8_t *image)
{
    epdDisplay(image);
}

// ── Fast refresh: same as full for this panel ──

void epdDisplayFast(const uint8_t *image)
{
    epdDisplay(image);
}

// ── Partial refresh: not supported ──

bool epdSupportsPartialRefresh()
{
    return false;
}

void epdPartialDisplay(uint8_t *data, int xStart, int yStart, int xEnd, int yEnd)
{
    (void)data;
    (void)xStart;
    (void)yStart;
    (void)xEnd;
    (void)yEnd;
    epdDisplay(imgBuf);
}

void epdPartialDisplayWithOld(uint8_t *data, const uint8_t *oldData, int xStart, int yStart, int xEnd, int yEnd)
{
    (void)data;
    (void)oldData;
    (void)xStart;
    (void)yStart;
    (void)xEnd;
    (void)yEnd;
    epdDisplay(imgBuf);
}

// ── EPD sleep ───────────────────────────────────────────────

void epdSleep()
{
    if (jdPowerIsOn)
        jdPowerOff();
    epdSendCommand(0x07);
    epdSendData(0xA5);
    delay(100);
}

#else
#error "Unsupported EPD panel. Define EPD_PANEL_JD79665"
#endif
