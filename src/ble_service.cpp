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
static const char *SVC_UUID = "8e7a0001-9f3b-4b1e-8a55-1a2b3c4d5e6f";
static const char *CMD_UUID = "8e7a0002-9f3b-4b1e-8a55-1a2b3c4d5e6f";
static const char *DATA_UUID = "8e7a0003-9f3b-4b1e-8a55-1a2b3c4d5e6f";
static const char *STAT_UUID = "8e7a0004-9f3b-4b1e-8a55-1a2b3c4d5e6f";

static const uint32_t ACK_STEP = 8192; // 每落盘这么多字节回一次流控确认

// ── 传输统计 ────────────────────────────────────────────────
// begin / reset 时清零：ack 从 0 重新计数，stats 里的校验和也对应当前这一次
// 传输，而不是开机以来的累计值。
static uint32_t rxTotal = 0;            // 本次上传已入环形缓冲的字节数
static uint32_t rxPackets = 0;          // DATA 写入次数（用来算平均包长）
static uint32_t nextAckAt = ACK_STEP;   // 下一条 ack 的触发水位（主循环私有）
static uint32_t adlerA = 1, adlerB = 0; // Adler-32 累加器，初值 a=1 / b=0

static volatile bool uploading = false; // 是否处于 begin..end 之间

// ── 上传环形缓冲：GATT 回调只入队，LittleFS 落盘全在主循环 ──
// 老做法在回调里直写 LittleFS：flash 块擦除会让 NimBLE 主机任务停顿几百毫秒，
// 而控制器/host 侧缓冲只有几 KB，一批在飞的 Write Without Response 会被静默
// 挤爆——链路层重传只保证包到达对端控制器，之后就管不着了，表现为 ack 超时的
// "上传假死"。改成环形缓冲 + 主循环落盘后，主机任务永远只做 memcpy。
// ack 语义相应从"已收到"改成"已落盘"：上位机每 8KB 等一次 ack 才发下一批，
// 在飞量恒 ≤8KB，16KB 缓冲足够；上位机判据 ack >= 已发字节数不变。
static const size_t RING_CAP = 16 * 1024;
static uint8_t ringBuf[RING_CAP];
static volatile size_t ringHead = 0; // 生产者：主机任务（DataCB::onWrite）
static volatile size_t ringTail = 0; // 消费者：主循环（bleUploadPump）
static uint32_t flushedTotal = 0;    // 已落盘字节数，主循环私有

static uint16_t connMtu = 23; // 协商后的 ATT MTU（默认 23，未协商前）
static uint16_t clientConnHandle = 0xFFFF;
static volatile bool clientConnected = false;

static NimBLECharacteristic *statChar = nullptr;

static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// ── 回调日志（延迟到主循环打印）────────────────────────────
// GATT 回调跑在 NimBLE 主机任务里，这里绝不能直接 Serial.printf：USB-CDC 在
// "端口打开但没人读"时（电池供电、USB 没插，正是手机连它时的常态）写入会自旋
// 阻塞。一旦卡在主机任务里，MTU 交换、服务发现、通知全部停摆——上位机看到的
// 就是"设备出现在列表里、点连接之后没有任何反应"。所以回调只置标志，真正
// 打印交给主循环（bleModeLoop → flushBleEvents）。
static volatile bool evConnected = false;
static volatile bool evDisconnected = false;
static volatile int evMtu = -1;  // >=0 表示本次 MTU 协商结果待打印
static volatile int evSub = -1;  // >=0 表示通知订阅状态变化待打印

static void flushBleEvents()
{
    if (evConnected)
    {
        evConnected = false;
        Serial.printf("[BLE] connected  handle=%u  mtu=%u\n",
                      (unsigned)clientConnHandle, connMtu);
    }
    if (evMtu >= 0)
    {
        Serial.printf("[BLE] MTU negotiated %d (单包有效载荷 %d 字节)\n",
                      evMtu, evMtu - 3);
        evMtu = -1;
    }
    if (evSub >= 0)
    {
        // 上位机卡在服务发现/订阅阶段时，看这一行就能分清是谁的问题
        Serial.printf("[BLE] notify subscription %s\n", evSub ? "enabled" : "disabled");
        evSub = -1;
    }
    if (evDisconnected)
    {
        evDisconnected = false;
        Serial.println("[BLE] disconnected");
    }
}

// ── 回包队列（主机任务 / 主循环 → 主循环泵出）───────────────
// 4 槽。原先是单槽覆盖式，只论证了 ack 能合并——但 display:done、end:ok、
// pong 这类独立回包挤在同一个泵间隔里会互相覆盖，页面只能干等超时。
// 消费端每 ~2ms 泵一次，正常排不满；满了挤掉最老的一条（新 ack 的累计值
// 天然覆盖老 ack，独立回包里挤掉最老的损失也最小）。
static portMUX_TYPE replyMux = portMUX_INITIALIZER_UNLOCKED;
static const uint8_t REPLY_SLOTS = 4;
static char replyQ[REPLY_SLOTS][160];
static volatile uint8_t replyHead = 0; // 下一个入队位
static volatile uint8_t replyTail = 0; // 下一个出队位

static void queueReply(const char *fmt, ...)
{
    char tmp[sizeof(replyQ[0])];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&replyMux);
    uint8_t next = (uint8_t)((replyHead + 1) % REPLY_SLOTS);
    if (next == replyTail)
        replyTail = (uint8_t)((replyTail + 1) % REPLY_SLOTS);
    memcpy(replyQ[replyHead], tmp, sizeof(replyQ[0]));
    replyHead = next;
    portEXIT_CRITICAL(&replyMux);
}

// ── 图库列表的分片发送 ──────────────────────────────────────
// galleryListJson() 有 2KB 级别（20 张 × 约 110 字节，名字上限 48 字节），
// 装不进一条通知，只能按 MTU 切开发。命令行里只置一个请求标志，真正的目录
// 扫描和分片发送都留给主循环（blePumpReplies）做——LittleFS 目录扫描是阻塞
// 操作，不该放在 NimBLE 主机任务里。
static volatile bool listRequested = false;
static bool listPumping = false; // 分片发送进行中（主循环私有）
static char listBuf[3072];       // 列表 JSON 快照
static size_t listLen = 0;
static size_t listSent = 0;
static uint8_t chunkOut[544]; // 一条通知的组包缓冲

// 排队的刷屏/清屏：绝不在 GATT 回调里执行（一次刷新阻塞 ~15s，会卡死链路）
static volatile int pendingDisplayId = -1;
static volatile bool pendingClear = false;
// 这次刷屏由按键触发：没有客户端在等 display:done，别发这条回包，
// 否则页面会把它当成自己那条 display 请求的应答，提前结束/错位。
static volatile bool pendingFromButton = false;
// GATT 回调收到的 display:<id>：回调只解析参数，存在性校验要扫 LittleFS
// 目录，必须留给主循环（见 ringPush 注释），校验和 display:queued 回包都在
// handleDisplayRequest() 里做。
static volatile int pendingDisplayReq = -1;

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
        if (a >= 65521)
            a -= 65521; // a < 65521 + 255，一次减法足够
        b += a;
        if (b >= 65521)
            b -= 65521; // b < 2 * 65521，一次减法足够
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

// ── 上传环形缓冲：入队（主机任务）/ 落盘（主循环）────────────

// 主机任务侧入队（单生产者）。失败 = 缓冲满；flush 流控下不该发生，真发生了
// 就丢弃且不计数，尾端 galleryUploadEnd 的 size 校验会报 end:fail，而不是
// 静默产出一张坏图。
static bool ringPush(const uint8_t *p, size_t len)
{
    size_t head = ringHead, tail = ringTail;
    size_t used = (head + RING_CAP - tail) % RING_CAP;
    if (len > RING_CAP - 1 - used)
        return false;
    size_t off = head;
    if (off + len > RING_CAP)
    {
        size_t first = RING_CAP - off;
        memcpy(ringBuf + off, p, first);
        memcpy(ringBuf, p + first, len - first);
    }
    else
    {
        memcpy(ringBuf + off, p, len);
    }
    ringHead = (size_t)((head + len) % RING_CAP);
    return true;
}

// 主循环侧：把环里数据落盘，按"已落盘"水位补发 ack。
static void ringDrain()
{
    if (ringTail == ringHead)
        return;
    if (!uploading)
    {
        ringTail = ringHead; // 传输已收尾/中止：残余直接丢
        return;
    }
    while (ringTail != ringHead)
    {
        size_t head = ringHead;
        size_t off = ringTail;
        size_t first = (head + RING_CAP - ringTail) % RING_CAP;
        if (off + first > RING_CAP)
            first = RING_CAP - off; // 这一轮只出到缓冲末尾，剩下的下一轮从头继续
        galleryUploadData(ringBuf + off, first);
        flushedTotal += first;
        ringTail = (size_t)((off + first) % RING_CAP);
    }
    if (flushedTotal >= nextAckAt)
    {
        while (flushedTotal >= nextAckAt)
            nextAckAt += ACK_STEP;
        // 一次落盘可能跨过多个水位，一条累计值覆盖全部（上位机判据 >=）
        queueReply("ack:%u", (unsigned)flushedTotal);
    }
}

// ── 主循环执行的文件类命令 ──────────────────────────────────
// begin/end/abort/reset/delete 都要动 LittleFS 或传输状态，不能留在主机任务
// 里执行（目录扫描、关文件刷写、unlink 都会停顿 host 几百毫秒）。统一存进
// 单槽交给主循环：上位机逐条等回包才发下一条，天然单飞。
static char pendingCmd[96] = {0};
static volatile bool pendingCmdReady = false;
// 断线时的半截上传收尾同样移交主循环（onDisconnect 也在主机任务里）
static volatile bool abortRequested = false;

static void setPendingCmd(const char *s)
{
    snprintf(pendingCmd, sizeof(pendingCmd), "%s", s);
    pendingCmdReady = true;
}

// 格式 begin:<名称>:<字节数>。名称理论上可能含 ':'，所以按最后一个
// ':' 切分，保证拿到的是末尾的数字。
static void handleBegin(const std::string &rest)
{
    if (!clientConnected)
        return; // 断线竞态兜底：命令已入队但连接先没了，别开一个没人接收的上传

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
    ringTail = ringHead;  // 清掉上一次的残余（中止路径没走到时的兜底）
    flushedTotal = 0;     // ack 水位对应本次传输
    resetTransferStats(); // ack 从 0 重新计数，stats 校验和对应本次上传
    queueReply("begin:ok:%d", id);
}

static void handleEnd()
{
    String err;
    int id = galleryUploadEnd("", &err);
    uploading = false;
    if (id >= 0)
        queueReply("end:ok:%d", id);
    else
        queueReply("end:fail:%s", err.c_str());
}

// 主循环处理 display:<id>：存在性校验 + display:queued 回包。
// 回调里只解析出 id，扫目录这种阻塞操作统一在这里做。
static void handleDisplayRequest()
{
    int id;
    portENTER_CRITICAL(&pendingMux);
    id = pendingDisplayReq;
    pendingDisplayReq = -1;
    portEXIT_CRITICAL(&pendingMux);
    if (id < 0)
        return;

    // 上传中拒绝刷屏（与按键路径 bleRequestNextImage 的 uploading 保护一致）。
    // 串口通道的上传同样在写 flash，所以用 galleryUploadBusy() 一并挡掉。
    if (uploading || galleryUploadBusy())
    {
        queueReply("err:上传中，暂不能刷屏");
        return;
    }
    if (!galleryExists(id))
    {
        queueReply("err:图片不存在");
        return;
    }
    // 先入回包队列再置排队标志：保证 "queued" 一定排在 "done" 之前发出
    queueReply("display:queued:%d", id);
    portENTER_CRITICAL(&pendingMux);
    pendingDisplayId = id;
    portEXIT_CRITICAL(&pendingMux);
}

// 主循环泵：中止收尾 → 环形缓冲落盘 → 文件类命令 → display 请求。
// 顺序不能反：end 之前必须把环里剩余数据全部落盘（size 校验才准），
// abort 要先丢掉环里的残余再删文件；display 要在 end 之后判定，
// 否则刚传完就刷屏会被 uploading 误挡。
// 可以在 15s 刷新等待（netServicePump）里调用：与刷屏同属主循环任务，
// 嵌套只是两次串行的文件系统操作，LittleFS 单任务访问安全。
void bleUploadPump()
{
    if (abortRequested)
    {
        abortRequested = false;
        ringTail = ringHead; // 消费者私有，直接丢残余
        galleryUploadAbort();
        uploading = false;
        return;
    }
    ringDrain();

    // 刷新中别执行 delete：文件正被刷屏流程读着，等下一轮
    if (pendingCmdReady &&
        !(epdIsRendering() && strncmp(pendingCmd, "delete:", 7) == 0))
    {
        pendingCmdReady = false;
        std::string s(pendingCmd);
        Serial.printf("[BLE] exec cmd '%s'\n", s.c_str());

        if (s == "reset")
        {
            resetTransferStats();
            queueReply("reset:ok");
        }
        else if (s.rfind("begin:", 0) == 0)
        {
            handleBegin(s.substr(6));
        }
        else if (s == "end")
        {
            handleEnd();
        }
        else if (s == "abort")
        {
            ringTail = ringHead;
            galleryUploadAbort();
            uploading = false;
            queueReply("abort:ok");
        }
        else if (s.rfind("delete:", 0) == 0)
        {
            long id = parseLong(s.c_str() + 7);
            if (id >= 0 && galleryDeleteById((int)id))
                queueReply("delete:ok:%d", (int)id);
            else
                queueReply("delete:fail:%d", (int)id);
        }
    }

    handleDisplayRequest();
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

        // 显式协商连接参数：监督超时是掉线容错的关键旋钮——中央侧默认常见
        // 4~6s，屏刷新的大电流噪声或 2.4GHz 干扰突发一超过它就断链。请求
        // 30~60ms 间隔 + 10s 超时；中央拒绝就维持原参数，无副作用。
        server->updateConnParams(desc->conn_handle,
                                 24 /* 30ms */, 48 /* 60ms */, 0, 1000 /* 10s */);
        // DLE（251 字节长包）：老 Windows 中央可能不主动发起，设备侧补一枪；
        // 空口占用时间大约减半，list 分片和上传都受益，请求失败无害。
        server->setDataLen(desc->conn_handle, 251);

        // 只置标志，不在主机任务里打印（见 flushBleEvents 的注释）
        evConnected = true;
    }

    void onDisconnect(NimBLEServer *server) override
    {
        (void)server;
        clientConnected = false;
        clientConnHandle = 0xFFFF;
        clearReply();
        listRequested = false;
        listPumping = false;
        // 还没执行的文件类命令作废：断线前入队的 begin 会开出一个没有接收方
        // 的上传，uploading 卡在 true，自动深睡永远进不去。
        pendingCmdReady = false;
        // display 请求同理：请求方已经走了，没必要再刷一次屏
        pendingDisplayReq = -1;

        // 半截上传必须显式收尾：uploadFile 不关就一直占着句柄，残缺文件还会
        // 以正常命名留在 /img 里被列表当成一张能点却读不出来的图。
        // 收尾要动 LittleFS，不能在主机任务里做——置标志交给主循环执行。
        if (uploading)
            abortRequested = true;

        // 重启广播由框架接管（advertiseOnDisconnect 默认 true，本回调返回后
        // 自动开播），这里不再手动 startAdvertising()。
        evDisconnected = true; // 同上：打印交给主循环
    }

    void onMTUChange(uint16_t mtu, ble_gap_conn_desc *desc) override
    {
        (void)desc;
        connMtu = mtu;
        evMtu = (int)mtu; // 同上：打印交给主循环
    }

private:
    void clearReply()
    {
        portENTER_CRITICAL(&replyMux);
        replyTail = replyHead;
        portEXIT_CRITICAL(&replyMux);
    }
};

// 状态特征的订阅回调：只用来在串口里留一行"手机到底订上了没有"。
// 排查"连上但页面没反应"时，这行是分界线——有 enabled 说明主机任务活着、
// 通知通道通了，问题在浏览器侧；没有则说明根本没走到订阅。
class StatCB : public NimBLECharacteristicCallbacks
{
    void onSubscribe(NimBLECharacteristic *ch, ble_gap_conn_desc *desc,
                     uint16_t subValue) override
    {
        (void)ch;
        (void)desc;
        evSub = (subValue != 0) ? 1 : 0; // 打印交给主循环，见 flushBleEvents
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
        if (!uploading)
            return;

        // 只入环形缓冲，绝不在这里碰 LittleFS（见 ringPush 注释）。
        // adler/计数只统计真正入队（最终会落盘）的字节：被丢弃的字节不计，
        // 尾端 size 校验才会把它暴露成 end:fail，而不是静默产出坏图。
        if (!ringPush(p, len))
            return;
        adlerUpdate(p, len);
        rxPackets++;
        rxTotal += len;
        // ack 不在这里发：由主循环落盘后按 flushedTotal 水位补发（"已落盘"语义）
    }
};

class CmdCB : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *ch) override
    {
        std::string s = ch->getValue();
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\0'))
            s.pop_back();

        // 这里刻意不打串口日志：USB-CDC 的 write 在"端口打开但对端不读"时
        // 可能长时间自旋，卡在主机任务里就是掉线。命令日志改在主循环
        // 执行时打（bleUploadPump）。ping/info/stats/list 是纯内存操作，
        // 留在回调里立即响应；动 LittleFS / 传输状态的全部转主循环。

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
        else if (s == "reset" || s == "end" || s == "abort" ||
                 s.rfind("begin:", 0) == 0 || s.rfind("delete:", 0) == 0)
        {
            setPendingCmd(s.c_str());
        }
        else if (s == "list")
        {
            // 只置标志：目录扫描和分片发送都在主循环里做
            listRequested = true;
        }
        else if (s.rfind("display:", 0) == 0)
        {
            // 只解析参数。存在性校验（galleryExists）要扫 LittleFS 目录，
            // 不能在主机任务里做——flash 读停顿几十~几百 ms，会把在飞包挤爆。
            // 上传中判定、存在性校验和 display:queued 回包全部交给主循环。
            long id = parseLong(s.c_str() + 8);
            if (id < 0)
            {
                queueReply("err:图片不存在");
            }
            else
            {
                portENTER_CRITICAL(&pendingMux);
                pendingDisplayReq = (int)id;
                portEXIT_CRITICAL(&pendingMux);
            }
        }
        else if (s == "clear")
        {
            if (uploading)
            {
                queueReply("err:上传进行中");
            }
            else
            {
                queueReply("clear:queued");
                portENTER_CRITICAL(&pendingMux);
                pendingClear = true;
                portEXIT_CRITICAL(&pendingMux);
            }
        }
        else
        {
            queueReply("err:unknown");
        }
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
    statChar->setCallbacks(new StatCB());

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
        {
            // 宁可报错也不发半截 JSON：页面按声明的总长拼满就直接 parse，
            // 残包只会得到一个看不懂的语法错误
            Serial.printf("[BLE] list too large: %u > %u bytes\n",
                          (unsigned)n, (unsigned)sizeof(listBuf));
            queueReply("err:列表过长");
        }
        else
        {
            memcpy(listBuf, json.c_str(), n);
            listLen = n;
            listSent = 0;
            listPumping = true;
        }
    }

    if (listPumping)
    {
        // 分片之间让出约一个连接事件（20ms）：连发会把 MSYS 缓冲池（20 块）
        // 顶到极限，与上传期 RX 流量叠加时会静默丢通知，页面只能干等分片。
        // 第一片不等（listSent==0 时立即发），列表本身只有几片，总延迟 <100ms。
        static unsigned long lastChunkMs = 0;
        if (listSent > 0 && millis() - lastChunkMs < 20UL)
            return;
        lastChunkMs = millis();

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

    // 2) 普通回包（一次只出一槽，维持既有发送节奏）
    char msg[sizeof(replyQ[0])];
    bool have = false;
    portENTER_CRITICAL(&replyMux);
    if (replyTail != replyHead)
    {
        memcpy(msg, replyQ[replyTail], sizeof(msg));
        replyTail = (uint8_t)((replyTail + 1) % REPLY_SLOTS);
        have = true;
    }
    portEXIT_CRITICAL(&replyMux);

    if (have)
        sendToClient((const uint8_t *)msg, strnlen(msg, sizeof(msg)));
}

// ── 广播看门狗 ──────────────────────────────────────────────
// 控制器异常时广播可能被静默停掉（没有回调也没有错误码），用户看到的就是
// "搜不到"，只能重启。每 10s 兜一次底：没连人、又不在广播，就重新开播。
static void advWatchdog()
{
    if (clientConnected)
        return;

    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    if (now - lastCheck < 10000UL)
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
    flushBleEvents(); // 回调期攒下的连接/MTU/订阅事件在这里打印
    advWatchdog();
    // 先落盘 + 执行文件类命令（end 的回包必须排在实际落盘之后），
    // 再把回包发出去（含 display:queued / clear:queued），最后执行刷屏
    bleUploadPump();
    blePumpReplies();

    // 排队的刷屏/清屏。这里会阻塞 ~15 秒，期间 epdWaitBusy 通过 netServicePump
    // 反复调用 bleUploadPump/blePumpReplies，页面不会因为等不到回包而超时。
    // uploading 时挂起不执行（双保险：display:/按键路径本就拒绝在上传中排队
    // 刷屏）。上传结束后下一轮 loop 自动接着执行。
    // 上传进行中不执行：刷屏读 flash 与上传写 flash 会并发访问 LittleFS，
    // 串口通道的上传由 galleryUploadBusy() 覆盖。
    if (!epdIsRendering() && !uploading && !galleryUploadBusy())
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