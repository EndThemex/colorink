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

// ── NEW 型物理帧测试模式（-DEPD_JD79665_PHYS800）──────────────────────────
// 参考驱动（krstc/openepaperlinkforCN tag_fw/src/epd_driver/jd79665.cpp）对
// STYPE_SIZE_75_JD79665_BWRY_NEW 有独立路径：逻辑 768x552，物理 800x600，
// 逻辑图像写在 X 偏移 32 处，边缘补白。若屏实际是 new 型而按 768x552 声明，
// 物理 gate 552..599 永远不被扫描 → 固定位置竖条保留旧画面。此模式用于验证。
#ifdef EPD_JD79665_PHYS800
#define PHYS_XRES 800
#define PHYS_YRES 600
#define PHYS_X_OFFSET 32
#endif

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

// 等待控制器空闲。返回 false 表示超时（控制器卡死/波形异常），调用方据此重试。
static bool epdWaitBusy(unsigned long maxMs = 0)
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
            return false;
        }
    }
    return true;
}

static bool jdPowerIsOn = false;
// 控制器是否已进 0x07 深睡：深睡后 SPI 无响应，重复发送没有意义；
// epdReset（硬件复位）是唯一的唤醒路径，复位后清标志。
static bool jdInDeepSleep = false;

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
    // 硬件复位唤醒深睡中的控制器
    jdInDeepSleep = false;
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
#ifdef EPD_JD79665_PHYS800
    // new 型：向控制器声明物理帧 800x600（参考驱动同位置按 solumType 切换）
    epdSendCmdData(0x61, 4, PHYS_XRES >> 8, PHYS_XRES & 0xFF, PHYS_YRES >> 8, PHYS_YRES & 0xFF);
    Serial.println("[EPD] PHYS800 test mode: frame 800x600, X offset 32");
#else
    epdSendCmdData(0x61, 4, W >> 8, W & 0xFF, H >> 8, H & 0xFF); // resolution
#endif
    epdSendCmdData(0x65, 4, 0x10, 0x00, 0x20, 0x00);             // GSST setting
    epdSendCmdData(0xE3, 1, 0x2F);
    epdSendCmdData(0x84, 1, 0x01);

    jdPowerIsOn = false;
    jdPowerOn();
}

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

#ifdef EPD_JD79665_PHYS800
// ── new 型物理帧写入：单个全窗 + 连续数据流 ──
// 与参考驱动 epdWriteDisplayDataPhysicalFrame 一致：600 行 × 200 字节连续写。
// 每行前 8 字节（X 偏移 32px）填白，逻辑 192 字节接在其后；gate 552..599 的
// 补边行整行填白（0x55 = 0001 0101，即全白 2bpp 打包）。
static void jdWrite2bppPhys(const uint8_t *buf)
{
    const int physRowBytes = PHYS_XRES / 4; // 200
    const int xOffBytes = PHYS_X_OFFSET / 4; // 8
    const int logRowBytes = W / 4;           // 192
    uint8_t whiteRun[200];
    memset(whiteRun, 0x55, sizeof(whiteRun));

    epdSetWindow(0, 0, PHYS_XRES, PHYS_YRES);
    epdSendCommand(0x10);
    for (int gy = 0; gy < PHYS_YRES; gy++)
    {
        if ((gy & 63) == 0)
            netServicePump();
        if (gy < H)
        {
            epdSendDataBulk(whiteRun, xOffBytes);
            epdSendDataBulk(buf + gy * logRowBytes, logRowBytes);
        }
        else
        {
            epdSendDataBulk(whiteRun, physRowBytes);
        }
    }
}
#endif

// ── 2bpp color refresh ──
// 全工程唯一入口：gallery 只存/只发 2bpp BWRY 帧缓冲，单色转换路径已随
// imgBuf 一并移除（那 105KB 静态缓冲从未被写过）。

bool epdDisplay2bpp(const uint8_t *image2bpp)
{
    if (epdRendering)
    {
        Serial.println("[EPD] refresh already in progress, request ignored");
        return false;
    }
    epdRendering = true;

    bool ok = false;
    unsigned long t0 = millis();
    for (int attempt = 0; attempt < 3 && !ok; attempt++)
    {
        t0 = millis();
        Serial.printf("[EPD] attempt %d start BUSY=%d\n", attempt, digitalRead(PIN_EPD_BUSY));
        epdInit(); // 复位 + 寄存器序列 + 上电（也会把深睡中的控制器唤醒）
        Serial.printf("[EPD] init done %lums BUSY=%d\n", millis() - t0, digitalRead(PIN_EPD_BUSY));

#ifdef EPD_JD79665_PHYS800
        jdWrite2bppPhys(image2bpp);
#else
        jdWrite2bpp(image2bpp);
#endif
        Serial.printf("[EPD] data done %lums\n", millis() - t0);

#ifdef EPD_JD79665_PHYS800
        epdSetWindow(0, 0, PHYS_XRES, PHYS_YRES);
#else
        epdSetWindow(0, 0, W, H);
#endif
        epdSendCommand(0x12);
        epdSendData(0x00);
        // 刷新等待可能超时（控制器卡死）：计入失败，硬件复位后重试
        ok = epdWaitBusy(60000);
        Serial.printf("[EPD] refresh %s %lums\n", ok ? "done" : "TIMEOUT", millis() - t0);
    }

    // colorBuf 保持常驻（开机时预分配，见 main.cpp setup）：运行期重新
    // malloc 105KB 连续内存在上传过图片后容易因堆碎片失败，导致刷新静默失败
    epdSleep(); // 关面板电源轨 + 控制器进 0x07 深睡，降低待机电流
    Serial.printf("[EPD] all done %lums\n", millis());
    epdRendering = false;
    if (!ok)
        Serial.println("[EPD] display failed after 3 attempts");
    return ok;
}

// ── EPD sleep ───────────────────────────────────────────────

void epdSleep()
{
    if (jdInDeepSleep)
        return; // 已在深睡，SPI 无响应，重复发送没有意义
    if (jdPowerIsOn)
        jdPowerOff();
    epdSendCommand(0x07);
    epdSendData(0xA5);
    jdInDeepSleep = true;
    delay(100);
}

#else
#error "Unsupported EPD panel. Define EPD_PANEL_JD79665"
#endif
