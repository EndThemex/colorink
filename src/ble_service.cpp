#ifdef INKSIGHT_BLE

#include "ble_service.h"
#include "config.h"
#include "gallery.h"
#include "epd_driver.h"

#include <NimBLEDevice.h>
#include <esp_mac.h>

// ── UUID ────────────────────────────────────────────────────
// 自定义 128 位 UUID。Web Bluetooth 侧靠 requestDevice 的 optionalServices
// 授权访问，不强求它出现在广播包里。
static const char *SVC_UUID  = "8e7a0001-9f3b-4b1e-8a55-1a2b3c4d5e6f";
static const char *CMD_UUID  = "8e7a0002-9f3b-4b1e-8a55-1a2b3c4d5e6f";
static const char *DATA_UUID = "8e7a0003-9f3b-4b1e-8a55-1a2b3c4d5e6f";
static const char *STAT_UUID = "8e7a0004-9f3b-4b1e-8a55-1a2b3c4d5e6f";

static const uint32_t ACK_STEP = 8192;  // 每收到这么多字节回一次流控确认

// ── 传输统计 ────────────────────────────────────────────────
// begin / reset 时清零：ack 从 0 重新计数，stats 里的校验和也对应当前这一次
// 传输，而不是开机以来的累计值。
static uint32_t rxTotal = 0;            // 本次上传收到的字节数
static uint32_t rxPackets = 0;          // DATA 写入次数（用来算平均包长）
static uint32_t nextAckAt = ACK_STEP;   // 下一条 ack 的触发水位
static uint32_t adlerA = 1, adlerB = 0; // Adler-32 累加器，初值 a=1 / b=0

static volatile bool uploading = false; // 是否处于 begin..end 之间

static uint16_t connMtu = 23;           // 协商后的 ATT MTU（默认 23，未协商前）
static uint16_t clientConnHandle = 0xFFFF;
static volatile bool clientConnected = false;

static NimBLECharacteristic *statChar = nullptr;

static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// ── 回包队列（GATT 回调任务 → 主循环，单向）─────────────────
// 单槽覆盖式：ack 的累计值是单调递增的，多次 ack 被合并成最新的一条不会影响
// 上位机判断（它只关心"累计值 >= 我发出去的量"）。
static portMUX_TYPE replyMux = portMUX_INITIALIZER_UNLOCKED;
static char replyBuf[160] = {0};
static volatile bool replyReady = false;

static void queueReply(const char *fmt, ...)
{
    char tmp[sizeof(replyBuf)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&replyMux);
    memcpy(replyBuf, tmp, sizeof(replyBuf));
    replyReady = true;
    portEXIT_CRITICAL(&replyMux);
}

// ── 图库列表的分片发送 ──────────────────────────────────────
// galleryListJson() 有 1KB 级别（20 张 × 约 60 字节），装不进一条通知，
// 只能按 MTU 切开发。命令行里只置一个请求标志，真正的目录扫描和分片发送
// 都留给主循环（blePumpReplies）做——LittleFS 目录扫描是阻塞操作，不该放在
// NimBLE 主机任务里。
static volatile bool listRequested = false;
static bool listPumping = false;        // 分片发送进行中（主循环私有）
static char listBuf[1600];              // 列表 JSON 快照
static size_t listLen = 0;
static size_t listSent = 0;
static uint8_t chunkOut[544];           // 一条通知的组包缓冲

// 排队的刷屏/清屏：绝不在 GATT 回调里执行（一次刷新阻塞 ~15s，会卡死链路）
static volatile int pendingDisplayId = -1;
static volatile bool pendingClear = false;
// 这次刷屏由按键触发：没有客户端在等 display:done，别发这条回包，
// 否则页面会把它当成自己那条 display 请求的应答，提前结束/错位。
static volatile bool pendingFromButton = false;

// ── 回包发送 ────────────────────────────────────────────────

static void sendToClient(const uint8_t *data, size_t len)
{
    if (statChar == nullptr || !clientConnected || len == 0)
        return;
    statChar->setValue(data, len);
    statChar->notify();
}

// Adler-32：模 65521，逐字节。网页端 web/ 里是同一份实现，
// 任何改动都必须两边同步，否则校验永远对不上。
static void adlerUpdate(const uint8_t *buf, size_t len)
{
    uint32_t a = adlerA, b = adlerB;
    for (size_t i = 0; i < len; i++)
    {
        a += buf[i];
        if (a >= 65521) a -= 65521;   // a < 65521 + 255，一次减法足够
        b += a;
        if (b >= 65521) b -= 65521;   // b < 2 * 65521，一次减法足够
    }
    adlerA = a;
    adlerB = b;
}

static void resetTransferStats()
{
    rxTotal = 0;
    rxPackets = 0;
    nextAckAt = ACK_STEP;
    adlerA = 1;
    adlerB = 0;
}

// 十进制解析；失败返回 -1。
static long parseLong(const char *s)
{
    if (s == nullptr || *s == '\0')
        return -1;
    char *end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s)
        return -1;
    return v;
}

// ── GATT 回调 ───────────────────────────────────────────────

class ServerCB : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override
    {
        clientConnected = true;
        clientConnHandle = desc->conn_handle;
        // 这一层的 MTU 通常还是默认 23，真正的协商结果由 onMTUChange 补上
        connMtu = server->getPeerMTU(desc->conn_handle);
        Serial.printf("[BLE] connected  handle=%d  mtu=%u\n", desc->conn_handle, connMtu);
    }

    void onDisconnect(NimBLEServer *server) override
    {
        (void)server;
        clientConnected = false;
        clientConnHandle = 0xFFFF;
        clearReply();
        listRequested = false;
        listPumping = false;

        // 半截上传必须显式收尾：uploadFile 不关就一直占着句柄，残缺文件还会
        // 以正常命名留在 /img 里被列表当成一张能点却读不出来的图。
        if (uploading)
        {
            galleryUploadAbort();
            uploading = false;
        }

        Serial.println("[BLE] disconnected, advertising again");
        NimBLEDevice::startAdvertising();
    }

    void onMTUChange(uint16_t mtu, ble_gap_conn_desc *desc) override
    {
        (void)desc;
        connMtu = mtu;
        Serial.printf("[BLE] MTU negotiated %u (单包有效载荷 %u 字节)\n", mtu, mtu - 3);
    }

private:
    void clearReply()
    {
        portENTER_CRITICAL(&replyMux);
        replyReady = false;
        portEXIT_CRITICAL(&replyMux);
    }
};

class DataCB : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *ch) override
    {
        NimBLEAttValue v = ch->getValue();
        const uint8_t *p = v.data();
        size_t len = v.length();
        if (len == 0 || p == nullptr)
            return;

        adlerUpdate(p, len);

        rxPackets++;
        rxTotal += len;

        // 直写 LittleFS：galleryUploadData 内部就是 uploadFile.write()，
        // BLE 链路层有重传保证不会静默丢包，因此不需要环形缓冲解耦。
        // 回退点：若 iOS 实测不稳，改成环形缓冲 + 主循环 drain，
        // 同时把 ack 从"已收到"改成"已落盘"。
        if (uploading)
            galleryUploadData(p, len);

        // 单次写入不可能跨过多个 ACK_STEP，但用 while 兜住异常情况更省心
        if (rxTotal >= nextAckAt)
        {
            while (rxTotal >= nextAckAt)
                nextAckAt += ACK_STEP;
            queueReply("ack:%u", (unsigned)rxTotal);
        }
    }
};

class CmdCB : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *ch) override
    {
        std::string s = ch->getValue();
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\0'))
            s.pop_back();

        Serial.printf("[BLE] cmd '%s'\n", s.c_str());

        if (s == "ping")
        {
            queueReply("pong");
        }
        else if (s == "info")
        {
            queueReply("info:%u:%u:%lu", connMtu, (unsigned)ESP.getFreeHeap(), millis());
        }
        else if (s == "stats")
        {
            queueReply("stats:%u:%u:%08x", (unsigned)rxTotal, (unsigned)rxPackets,
                       (unsigned)((adlerB << 16) | adlerA));
        }
        else if (s == "reset")
        {
            resetTransferStats();
            queueReply("reset:ok");
        }
        else if (s == "list")
        {
            // 只置标志：目录扫描和分片发送都在主循环里做
            listRequested = true;
        }
        else if (s.rfind("begin:", 0) == 0)
        {
            handleBegin(s.substr(6));
        }
        else if (s == "end")
        {
            String err;
            int id = galleryUploadEnd("", &err);
            uploading = false;
            if (id >= 0)
                queueReply("end:ok:%d", id);
            else
                queueReply("end:fail:%s", err.c_str());
        }
        else if (s == "abort")
        {
            galleryUploadAbort();
            uploading = false;
            queueReply("abort:ok");
        }
        else if (s.rfind("display:", 0) == 0)
        {
            long id = parseLong(s.c_str() + 8);
            if (id < 0 || !galleryExists((int)id))
            {
                queueReply("err:图片不存在");
            }
            else
            {
                // 先入回包队列再置排队标志：保证"queued"一定排在"done"之前发出
                queueReply("display:queued:%d", (int)id);
                portENTER_CRITICAL(&pendingMux);
                pendingDisplayId = (int)id;
                portEXIT_CRITICAL(&pendingMux);
            }
        }
        else if (s.rfind("delete:", 0) == 0)
        {
            long id = parseLong(s.c_str() + 7);
            if (id >= 0 && galleryDeleteById((int)id))
                queueReply("delete:ok:%d", (int)id);
            else
                queueReply("delete:fail:%d", (int)id);
        }
        else if (s == "clear")
        {
            queueReply("clear:queued");
            portENTER_CRITICAL(&pendingMux);
            pendingClear = true;
            portEXIT_CRITICAL(&pendingMux);
        }
        else
        {
            queueReply("err:unknown");
        }
    }

private:
    void handleBegin(const std::string &rest)
    {
        // 格式 begin:<名称>:<字节数>。名称理论上可能含 ':'，所以按最后一个
        // ':' 切分，保证拿到的是末尾的数字。
        size_t colon = rest.rfind(':');
        if (colon == std::string::npos)
        {
            queueReply("err:格式应为 begin:<名称>:<字节数>");
            return;
        }
        std::string nm = rest.substr(0, colon);
        long size = parseLong(rest.substr(colon + 1).c_str());

        // 尺寸先校验：不符就立刻拒绝，不用等传完 106KB 才发现
        if (size != (long)COLOR_BUF_LEN)
        {
            queueReply("err:尺寸不符，应为 %d 字节", COLOR_BUF_LEN);
            return;
        }
        if (uploading)
        {
            queueReply("err:已有上传在进行中");
            return;
        }

        int id = -1;
        String err;
        if (!galleryUploadStart(id, String(nm.c_str()), &err))
        {
            queueReply("err:%s", err.c_str());
            return;
        }
        uploading = true;
        resetTransferStats(); // ack 从 0 重新计数，stats 校验和对应本次上传
        queueReply("begin:ok:%d", id);
    }
};

// ── 生命周期 ────────────────────────────────────────────────

bool bleModeStart()
{
    // 用 eFuse 里的 WiFi STA MAC 取名，不去碰 WiFi 射频（BLE 固件全程不开 WiFi）。
    // 名字规则和 WiFi 固件的 AP 名一致，两块固件对照时不会认错板子。
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[24];
    snprintf(name, sizeof(name), "ColorInk-%02X%02X", mac[4], mac[5]);

    NimBLEDevice::init(name);
    NimBLEDevice::setMTU(517); // ATT MTU 上限；Chrome 一般会接受

    // 发射功率保持 NimBLE 默认档位：之前为补链路余量试过 +9dBm，实测根因是
    // 40MHz 晶振负载电容不对（手指贴晶振才搜得到），与发射功率无关；+9dBm
    // 会让射频功放和 LDO 明显发热，所以还原，不再主动设功率。

    NimBLEServer *server = NimBLEDevice::createServer();
    if (server == nullptr)
    {
        Serial.println("[BLE] createServer failed");
        return false;
    }
    server->setCallbacks(new ServerCB());

    NimBLEService *svc = server->createService(SVC_UUID);
    if (svc == nullptr)
    {
        Serial.println("[BLE] createService failed");
        return false;
    }

    // CMD 同时给 Write 和 Write Without Response：上位机用哪种都能通，
    // 方便在 Bluefy 上排查它到底支持哪个。
    NimBLECharacteristic *cmdChar = svc->createCharacteristic(
        CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    cmdChar->setCallbacks(new CmdCB());

    // DATA 也带 Write：如果浏览器没有 writeValueWithoutResponse，
    // 上位机会退回带响应写入，链路本身仍然可用（只是慢很多）。
    NimBLECharacteristic *dataChar = svc->createCharacteristic(
        DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    dataChar->setCallbacks(new DataCB());

    statChar = svc->createCharacteristic(STAT_UUID, NIMBLE_PROPERTY::NOTIFY);

    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    // 广播包只有 31 字节，128 位 UUID 独占 18 字节。原来的写法是"UUID 进广播包、
    // 名字靠扫描响应补"；被动扫描的接收端（Windows 蓝牙列表、部分手机扫描工具）
    // 不发扫描请求，看到的就是一个没名字的设备，很容易被当成"搜不到"。这里反过来：
    // 广播包里放完整名字，UUID 挪到扫描响应。上位机按 namePrefix 过滤（见
    // web/ble_test.html），不依赖广播包里的服务 UUID。
    NimBLEAdvertisementData advData;
    advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    advData.setName(name);

    NimBLEAdvertisementData scanData;
    scanData.setCompleteServices(NimBLEUUID(SVC_UUID));

    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    adv->setScanResponse(true);

    // 20~25ms 的短间隔（单位 0.625ms）：扫描窗口的命中率更高，用时间换灵敏度。
    adv->setMinInterval(0x20);
    adv->setMaxInterval(0x28);

    if (!NimBLEDevice::startAdvertising())
        Serial.println("[BLE] startAdvertising failed");

    Serial.printf("[BLE] service up, name='%s'  MTU target=517  t=%lums\n",
                  name, millis());
    return true;
}

void bleForceDisconnect()
{
    if (!clientConnected || clientConnHandle == 0xFFFF)
    {
        Serial.println("[BLE] no client to disconnect");
        return;
    }
    NimBLEServer *server = NimBLEDevice::getServer();
    if (server == nullptr)
        return;
    // 断开后 onDisconnect 回调会重新开始广播，不需要在这里再启动一次
    server->disconnect(clientConnHandle);
    Serial.printf("[BLE] force disconnect handle=%u\n", clientConnHandle);
}

bool bleIsConnected()
{
    return clientConnected;
}

bool bleIsBusy()
{
    portENTER_CRITICAL(&pendingMux);
    bool pending = pendingDisplayId >= 0 || pendingClear;
    portEXIT_CRITICAL(&pendingMux);
    return uploading || pending || epdIsRendering();
}

void bleRequestNextImage()
{
    // 上传过程中别插一次 15s 的刷屏：会把正在跑的传输拖成假死
    if (uploading)
    {
        Serial.println("[BTN] short press ignored (upload in progress)");
        return;
    }

    // 上一次按键排队的图可能还没刷完，从它往后接着数，连按两次才会跳两张
    int base = -1;
    portENTER_CRITICAL(&pendingMux);
    base = pendingDisplayId;
    portEXIT_CRITICAL(&pendingMux);
    if (base < 0)
        base = galleryCurrentId();

    int id = galleryNextId(base, 1);
    if (id < 0)
    {
        Serial.println("[BTN] short press ignored (no images)");
        return;
    }

    portENTER_CRITICAL(&pendingMux);
    pendingDisplayId = id;
    pendingFromButton = true;
    pendingClear = false;
    portEXIT_CRITICAL(&pendingMux);
    Serial.printf("[BTN] short press -> display next id=%d\n", id);
}

void blePumpReplies()
{
    if (statChar == nullptr || !clientConnected)
        return;

    // 1) 图库列表分片优先：一次 list 只有几片，发完再回别的包，
    //    免得 chunk 之间插进其它通知把页面的拼装逻辑搅乱。
    if (listRequested && !listPumping)
    {
        listRequested = false;
        String json = galleryListJson();
        size_t n = json.length();
        if (n > sizeof(listBuf))
            n = sizeof(listBuf);
        memcpy(listBuf, json.c_str(), n);
        listLen = n;
        listSent = 0;
        listPumping = true;
    }

    if (listPumping)
    {
        // 一条通知最多 MTU-3 字节，再扣掉 "chunk:<已发>/<总长>:" 头部。
        // 上限再压到 512：ATT 属性值长度的规范上限就是 512，超过时 notify
        // 会被协议栈直接丢掉，而丢包在这里是静默的（页面只会一直等分片）。
        size_t payload = (connMtu > 23) ? (size_t)(connMtu - 3) : 20;
        if (payload > 512)
            payload = 512;

        char hdr[24];
        int hlen = snprintf(hdr, sizeof(hdr), "chunk:%u/%u:",
                            (unsigned)listSent, (unsigned)listLen);
        size_t room = (payload > (size_t)hlen + 1) ? payload - (size_t)hlen : 0;
        if (room > listLen - listSent)
            room = listLen - listSent;

        if (room == 0 || hlen <= 0)
        {
            // MTU 小到连头部都装不下：放弃本次列表，别把主循环卡成死循环
            Serial.printf("[BLE] list chunk too small for MTU %u, dropped\n", connMtu);
            listPumping = false;
            return;
        }

        memcpy(chunkOut, hdr, hlen);
        memcpy(chunkOut + hlen, listBuf + listSent, room);
        sendToClient(chunkOut, (size_t)hlen + room);
        listSent += room;
        if (listSent >= listLen)
            listPumping = false;
        return; // 这一轮的发送额度给了列表，其它回包下一轮再说
    }

    // 2) 普通回包
    char msg[sizeof(replyBuf)];
    bool have = false;
    portENTER_CRITICAL(&replyMux);
    if (replyReady)
    {
        memcpy(msg, replyBuf, sizeof(msg));
        replyReady = false;
        have = true;
    }
    portEXIT_CRITICAL(&replyMux);

    if (have)
        sendToClient((const uint8_t *)msg, strnlen(msg, sizeof(msg)));
}

// ── 广播看门狗 ──────────────────────────────────────────────
// 控制器异常时广播可能被静默停掉（没有回调也没有错误码），用户看到的就是
// "搜不到"，只能重启。每 30s 兜一次底：没连人、又不在广播，就重新开播。
static void advWatchdog()
{
    if (clientConnected)
        return;

    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    if (now - lastCheck < 30000UL)
        return;
    lastCheck = now;

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv == nullptr || adv->isAdvertising())
        return;

    Serial.println("[BLE] advertising lost, restarting");
    if (!adv->start())
        Serial.println("[BLE] restart advertising failed");
}

void bleModeLoop()
{
    advWatchdog();
    // 先把回包发出去（含 display:queued / clear:queued），再执行刷屏
    blePumpReplies();

    // 排队的刷屏/清屏。这里会阻塞 ~15 秒，期间 epdWaitBusy 通过 netServicePump
    // 反复调用 blePumpReplies，页面不会因为等不到回包而超时。
    if (!epdIsRendering())
    {
        int id = -1;
        bool fromButton = false;
        portENTER_CRITICAL(&pendingMux);
        id = pendingDisplayId;
        pendingDisplayId = -1;
        fromButton = pendingFromButton;
        pendingFromButton = false;
        portEXIT_CRITICAL(&pendingMux);

        bool clr = false;
        if (id < 0)
        {
            portENTER_CRITICAL(&pendingMux);
            clr = pendingClear;
            pendingClear = false;
            portEXIT_CRITICAL(&pendingMux);
        }

        if (id >= 0)
        {
            bool ok = galleryDisplayById(id);
            Serial.printf("[BLE] display id=%d %s%s\n", id, ok ? "done" : "failed",
                          fromButton ? " (button)" : "");
            // 按键触发的刷屏没有请求方在等回包，别发 display:done
            if (!fromButton)
                queueReply(ok ? "display:done:%d" : "display:fail:%d", id);
        }
        else if (clr)
        {
            bool ok = galleryClearScreen();
            Serial.printf("[BLE] clear %s\n", ok ? "done" : "failed");
            queueReply(ok ? "clear:done" : "clear:fail");
        }
    }

    // 这颗板子只有一个按键，屏幕上也没有状态栏，LED 是唯一的非串口反馈通道：
    // 慢闪 = 正在广播等人连，常亮 = 已连接。不看串口也能判断现在跑的是哪套固件。
#if PIN_LED >= 0
    static bool ledOn = false;
    static unsigned long lastBlink = 0;
    unsigned long now = millis();
    if (clientConnected)
    {
        if (!ledOn)
        {
            digitalWrite(PIN_LED, HIGH);
            ledOn = true;
        }
    }
    else if (now - lastBlink >= 500UL) // 1Hz
    {
        ledOn = !ledOn;
        digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
        lastBlink = now;
    }
#endif

    static unsigned long lastStat = 0;
    if (millis() - lastStat >= 30000UL)
    {
        lastStat = millis();
        Serial.printf("[BLE] link=%s mtu=%u rx=%uB/%upkts up=%s heap=%u\n",
                      clientConnected ? "up" : "down", connMtu,
                      (unsigned)rxTotal, (unsigned)rxPackets,
                      uploading ? "yes" : "no", (unsigned)ESP.getFreeHeap());
    }
}

#endif // INKSIGHT_BLE