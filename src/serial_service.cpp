#include "serial_service.h"

#include <stdarg.h>

#include "config.h"
#include "gallery.h"

namespace {

const char *PFX = "CI:";            // 命令与回包前缀
// 串口单批确认 4096 字节：网页端每发 4096 就等这条 ack。比 BLE 的 8192 更细，
// 配合 8KB 接收缓冲，USB CDC 满速灌数据时也不会把驱动环形缓冲冲掉。
const uint32_t ACK_STEP = 4096;
const size_t LINE_CAP = 240;        // 单条命令行上限，防止长串撑爆内存
const size_t DATA_BUF = 512;
const unsigned long DATA_IDLE_TIMEOUT_MS = 5000; // 数据阶段静默超时，自动中止半截上传

bool uploading = false;             // begin .. end 之间
bool dataMode = false;              // 正在接收裸字节
uint32_t expectBytes = 0;           // begin 声明的总字节数
uint32_t uploadBytes = 0;           // 本次已收字节数
uint32_t nextAckAt = ACK_STEP;      // 下一条 ack 的触发水位
unsigned long lastDataMs = 0;

uint32_t rxTotal = 0;               // 本次传输收到的字节数（stats 用）
uint32_t rxPackets = 0;             // 写入次数（用来算平均包长）
uint32_t adlerA = 1, adlerB = 0;    // Adler-32 累加器

String lineBuf;
uint8_t dataBuf[DATA_BUF];

int pendingDisplayId = -1;
bool pendingClear = false;
unsigned long lastRxMs = 0;         // 最近一次收到串口数据的时刻（0 = 从未收到）

void resetStats()
{
    rxTotal = 0;
    rxPackets = 0;
    nextAckAt = ACK_STEP;
    adlerA = 1;
    adlerB = 0;
}

// Adler-32：模 65521，逐字节。与 ble_service.cpp / 网页端是同一份实现，
// 任何改动都必须三处同步，否则校验永远对不上。
void adlerUpdate(const uint8_t *buf, size_t len)
{
    uint32_t a = adlerA, b = adlerB;
    for (size_t i = 0; i < len; i++)
    {
        a += buf[i];
        if (a >= 65521) a -= 65521;
        b += a;
        if (b >= 65521) b -= 65521;
    }
    adlerA = a;
    adlerB = b;
}

// 回包：单行 "CI:<payload>\n"
void reply(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    Serial.print(PFX);
    Serial.print(tmp);
    Serial.print('\n');
}

void replyRaw(const String &payload)
{
    Serial.print(PFX);
    Serial.print(payload);
    Serial.print('\n');
}

long parseLong(const String &s)
{
    if (s.length() == 0) return -1;
    return s.toInt();
}

void abortUpload(const char *why)
{
    galleryUploadAbort();
    uploading = false;
    dataMode = false;
    expectBytes = 0;
    uploadBytes = 0;
    Serial.printf("[SER] upload aborted (%s)\n", why);
    reply("err:%s", why);
}

// 裸字节阶段：一直读到 expectBytes 或串口空闲为止。不做命令解析，
// 否则图片数据里恰好出现 '\n' 就会被当成命令行。
void handleData()
{
    while (Serial.available() > 0 && uploadBytes < expectBytes)
    {
        size_t want = (size_t)(expectBytes - uploadBytes);
        size_t n = (size_t)Serial.available();
        if (n > want) n = want;
        if (n > DATA_BUF) n = DATA_BUF;
        size_t got = (size_t)Serial.read(dataBuf, n);
        if (got == 0) break;

        lastDataMs = millis();
        adlerUpdate(dataBuf, got);
        rxPackets++;
        rxTotal += (uint32_t)got;
        uploadBytes += (uint32_t)got;
        if (uploading)
            galleryUploadData(dataBuf, got);

        if (rxTotal >= nextAckAt)
        {
            while (rxTotal >= nextAckAt)
                nextAckAt += ACK_STEP;
            reply("ack:%u", (unsigned)rxTotal);
        }
    }

    if (uploadBytes >= expectBytes)
    {
        dataMode = false; // 收满了，后续回到命令行模式（等着收 end）
        return;
    }
    if (millis() - lastDataMs > DATA_IDLE_TIMEOUT_MS)
        abortUpload("上传超时，已中止");
}

void handleBegin(const String &rest)
{
    // 格式 begin:<名称>:<字节数>。名称理论上可能含 ':'，按最后一个 ':' 切分。
    int colon = rest.lastIndexOf(':');
    if (colon < 0)
    {
        reply("err:格式应为 begin:<名称>:<字节数>");
        return;
    }
    String nm = rest.substring(0, colon);
    long size = parseLong(rest.substring(colon + 1));

    if (size != (long)COLOR_BUF_LEN)
    {
        reply("err:尺寸不符，应为 %d 字节", COLOR_BUF_LEN);
        return;
    }
    if (uploading)
    {
        reply("err:已有上传在进行中");
        return;
    }

    int id = -1;
    String err;
    if (!galleryUploadStart(id, nm, &err))
    {
        reply("err:%s", err.c_str());
        return;
    }

    uploading = true;
    dataMode = true;
    expectBytes = (uint32_t)size;
    uploadBytes = 0;
    lastDataMs = millis();
    resetStats();
    reply("begin:ok:%d", id);
    Serial.printf("[SER] upload begin id=%d name='%s' size=%ld\n",
                  id, nm.c_str(), size);
}

void handleLine(String s)
{
    while (s.length() > 0 &&
           (s[0] == '\r' || s[0] == '\n' || s[0] == ' '))
        s.remove(0, 1);
    while (s.length() > 0 &&
           (s[s.length() - 1] == '\r' || s[s.length() - 1] == '\n' || s[s.length() - 1] == ' '))
        s.remove(s.length() - 1);
    if (s.startsWith(PFX)) s.remove(0, 3); // 容错：带不带前缀都认
    if (s.length() == 0) return;

    Serial.printf("[SER] cmd '%s'\n", s.c_str());

    if (s == "ping")
    {
        reply("pong");
    }
    else if (s == "info")
    {
        // 串口没有 MTU 概念，字段留 0，网页端按串口分支另行处理
        reply("info:0:%u:%lu", (unsigned)ESP.getFreeHeap(), millis());
    }
    else if (s == "stats")
    {
        reply("stats:%u:%u:%08x", (unsigned)rxTotal, (unsigned)rxPackets,
              (unsigned)((adlerB << 16) | adlerA));
    }
    else if (s == "reset")
    {
        resetStats();
        reply("reset:ok");
    }
    else if (s == "list")
    {
        // 串口一次能把完整 JSON 发出去，不需要 BLE 那套分片协议
        replyRaw("list:" + galleryListJson());
    }
    else if (s.startsWith("raw:"))
    {
        // 裸字节自检：只统计、不写 flash，对应网页端"自检"按钮。
        long size = parseLong(s.substring(4));
        if (size <= 0)
        {
            reply("err:raw 尺寸无效");
        }
        else
        {
            uploading = false;
            dataMode = true;
            expectBytes = (uint32_t)size;
            uploadBytes = 0;
            lastDataMs = millis();
            resetStats();
            reply("raw:ok");
        }
    }
    else if (s.startsWith("begin:"))
    {
        handleBegin(s.substring(6));
    }
    else if (s == "end")
    {
        String err;
        int id = galleryUploadEnd("", &err);
        uploading = false;
        dataMode = false;
        expectBytes = 0;
        uploadBytes = 0;
        if (id >= 0) reply("end:ok:%d", id);
        else reply("end:fail:%s", err.c_str());
    }
    else if (s == "abort")
    {
        galleryUploadAbort();
        uploading = false;
        dataMode = false;
        expectBytes = 0;
        uploadBytes = 0;
        reply("abort:ok");
    }
    else if (s.startsWith("display:"))
    {
        long id = parseLong(s.substring(8));
        if (id < 0 || !galleryExists((int)id))
        {
            reply("err:图片不存在");
        }
        else
        {
            reply("display:queued:%d", (int)id);
            pendingDisplayId = (int)id;
        }
    }
    else if (s.startsWith("delete:"))
    {
        long id = parseLong(s.substring(7));
        if (id >= 0 && galleryDeleteById((int)id))
            reply("delete:ok:%d", (int)id);
        else
            reply("delete:fail:%d", (int)id);
    }
    else if (s == "clear")
    {
        reply("clear:queued");
        pendingClear = true;
    }
    else
    {
        reply("err:unknown");
    }
}

// 排队的刷屏/清屏。一次刷新要阻塞 ~15s，串口回包在完成后才发出，
// 网页端按 display:done 等待即可。
void runPending()
{
    if (uploading || dataMode) return;

    if (pendingDisplayId >= 0)
    {
        int id = pendingDisplayId;
        pendingDisplayId = -1;
        bool ok = galleryDisplayById(id);
        Serial.printf("[SER] display id=%d %s\n", id, ok ? "done" : "failed");
        reply(ok ? "display:done:%d" : "display:fail:%d", id);
    }
    else if (pendingClear)
    {
        pendingClear = false;
        bool ok = galleryClearScreen();
        Serial.printf("[SER] clear %s\n", ok ? "done" : "failed");
        reply(ok ? "clear:done" : "clear:fail");
    }
}

} // namespace

void serialServiceBegin(unsigned long baud)
{
    // 必须在 Serial.begin 之前设：USB CDC / UART 的接收环形缓冲都在 begin
    // 时按这个尺寸创建。默认 256 字节太小，网页一次灌一批数据就会溢出。
    Serial.setRxBufferSize(8192);
    Serial.begin(baud);
}

void serialServiceLoop()
{
    if (Serial.available() > 0)
        lastRxMs = millis();

    if (dataMode)
    {
        handleData();
    }
    else
    {
        while (Serial.available() > 0 && !dataMode)
        {
            int r = Serial.read();
            if (r < 0) break;
            char c = (char)r;
            if (c == '\r') continue;
            if (c == '\n')
            {
                String line = lineBuf;
                lineBuf = "";
                if (line.length() > 0) handleLine(line);
            }
            else if (lineBuf.length() < LINE_CAP)
            {
                lineBuf += c;
            }
        }
    }

    runPending();
}

bool serialServiceBusy()
{
    return uploading || dataMode || pendingDisplayId >= 0 || pendingClear ||
           (lastRxMs != 0 && millis() - lastRxMs < 10000UL);
}
