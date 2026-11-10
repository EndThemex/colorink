      "use strict";

      // 与固件 ble_service.cpp 保持一致，改一处必须改两处
      const SVC = "8e7a0001-9f3b-4b1e-8a55-1a2b3c4d5e6f";
      const CMD = "8e7a0002-9f3b-4b1e-8a55-1a2b3c4d5e6f";
      const DATA = "8e7a0003-9f3b-4b1e-8a55-1a2b3c4d5e6f";
      const STAT = "8e7a0004-9f3b-4b1e-8a55-1a2b3c4d5e6f";

      const ACK_STEP = 8192; // BLE 流控确认间隔，必须与固件 ACK_STEP 一致
      const SERIAL_ACK_STEP = 4096; // 串口确认间隔，必须与 serial_service.cpp 一致
      const IMG_BYTES = 105984; // 552x768 四色 2bpp 的真实图片体积
      const MAX_CHUNK = 509; // MTU 512 时的单包有效载荷上限，兜底封顶
      const DISPLAY_MS = 40000; // 刷屏一次要 ~15s，等待 done 的超时要留足

      // ── 四色量化常量（与 WiFi 版 webapp_html.h 一致）─────────────
      const SW = 552,
        SH = 768;
      const PIXELS = SW * SH;
      const ROW_BYTES = SH >> 2; // 每物理行 768 像素 → 192 字节
      const FRAME_BYTES = PIXELS >> 2; // 105984
      const CODES = [0, 1, 2, 3]; // 00=黑 01=白 10=黄 11=红
      const QUANT_PAL = ["#000000", "#ffffff", "#f0d600", "#be201c"];
      const DISPLAY_PAL_DEFAULT = QUANT_PAL.slice();
      const DISPLAY_PAL_KEY = "colorink_display_pal_v3";
      let DISPLAY_PAL = DISPLAY_PAL_DEFAULT.slice();
      try {
        const saved = JSON.parse(
          localStorage.getItem(DISPLAY_PAL_KEY) || "null",
        );
        if (
          Array.isArray(saved) &&
          saved.length === 4 &&
          saved.every((h) => /^#[0-9a-f]{6}$/i.test(h))
        )
          DISPLAY_PAL = saved;
      } catch (_) {}

      const logEl = document.getElementById("log");
      const $ = (id) => document.getElementById(id);

      // ── 首次访问提示弹窗 ────────────────────────────────────────
      // 勾选「不再弹出」后写入 localStorage；未勾选则每次访问都提示。
      const TIP_KEY = "colorink_tip_hidden_v1";
      (function setupTip() {
        let hidden = false;
        try {
          hidden = localStorage.getItem(TIP_KEY) === "1";
        } catch (_) {}
        if (hidden) return;
        const mask = $("tipMask");
        mask.hidden = false;
        $("tipNoMore").addEventListener("change", (e) => {
          try {
            if (e.target.checked) localStorage.setItem(TIP_KEY, "1");
            else localStorage.removeItem(TIP_KEY);
          } catch (_) {}
        });
        $("tipClose").addEventListener("click", () => {
          mask.hidden = true;
        });
        document.addEventListener("keydown", (e) => {
          if (e.key === "Escape" && !mask.hidden) mask.hidden = true;
        });
        $("tipClose").focus();
      })();

      function log(text, cls) {
        const t = new Date().toTimeString().slice(0, 8);
        const span = document.createElement("span");
        if (cls) span.className = cls;
        span.textContent = "[" + t + "] " + text + "\n";
        logEl.appendChild(span);
        logEl.scrollTop = logEl.scrollHeight;
        // 调试页不在前台时，重要日志（非 dim）累积成徽标提醒
        if (cls !== "dim" && !$("pageDebug").classList.contains("active")) {
          unread++;
          updateBadge();
        }
      }

      // ── 选项卡 ──────────────────────────────────────────────────
      let unread = 0;
      function updateBadge() {
        const b = $("dbgBadge");
        b.hidden = unread === 0;
        b.textContent = unread > 99 ? "99+" : unread;
      }

      function switchTab(name) {
        for (const t of document.querySelectorAll(".tab"))
          t.classList.toggle("active", t.dataset.page === name);
        for (const p of document.querySelectorAll(".tabpage"))
          p.classList.toggle("active", p.id === name);
        if (name === "pageDebug") {
          unread = 0;
          updateBadge();
        }
        if (name === "pageCalendar") {
          calInit();
          calRender();
        }
      }
      for (const t of document.querySelectorAll(".tab"))
        t.addEventListener("click", () => switchTab(t.dataset.page));

      // 主页状态行：只放用户关心的结果，过程细节去调试页看日志
      const statusEl = $("status");
      function report(text, cls) {
        statusEl.textContent = text;
        statusEl.className = cls || "";
        log(text, cls);
      }

      // 全局浮层提示：任何 tab 下都看得见，上传/刷屏的过程反馈都走这里
      function toast(msg, ms, cls) {
        const t = $("toast");
        t.textContent = msg;
        t.className = cls || "";
        t.classList.add("show");
        clearTimeout(t._h);
        t._h = setTimeout(() => t.classList.remove("show"), ms || 2600);
      }

      function escapeHtml(s) {
        return String(s).replace(
          /[&<>"]/g,
          (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c],
        );
      }

      // ── 状态 ────────────────────────────────────────────────────
      let device = null,
        server = null;
      let chCmd = null,
        chData = null,
        chStat = null;
      let mtu = 23;
      let noResp = false;
      let busy = false;
      let link = null; // 'ble' | 'serial'
      let connected = false;
      let chunkSize = MAX_CHUNK; // 单次写入的数据块大小
      let serialPort = null,
        serialWriter = null,
        serialReader = null,
        serialBuf = "";
      const SERIAL_CHUNK = 4096; // 串口单包，远大于 BLE 的 MTU 限制
      const waiters = [];

      // ── 通知等待 ────────────────────────────────────────────────
      // pred(text, raw)：text 供人读/正则匹配，raw 是原始字节。列表分片必须在
      // raw 上拼装——按 text 拼会把跨片的 UTF-8 多字节字符截成乱码。
      function waitFor(pred, ms, label) {
        return new Promise((resolve, reject) => {
          // reject 一并挂到等待者上：断开时要能主动作废，不能让它们各自干等到超时
          // （刷屏那条要 40s，页面会一直停在"等待面板刷新"）。
          const w = { pred, resolve, reject, timer: null };
          w.timer = setTimeout(() => {
            const i = waiters.indexOf(w);
            if (i >= 0) waiters.splice(i, 1);
            reject(new Error("等待 " + label + " 超时（" + ms + "ms）"));
          }, ms);
          waiters.push(w);
        });
      }

      // 所有回包（BLE 通知 / 串口行）统一从这里分发。raw 只有 BLE 的分片列表用得到。
      function notify(text, raw) {
        if (text.startsWith("chunk:") && raw) {
          const i = text.indexOf(":", 6);
          log(
            "← " + text.slice(0, i) + ":<" + (raw.length - i - 1) + " 字节>",
            "dim",
          );
        } else if (text.startsWith("list:")) {
          log("← list:<" + (text.length - 5) + " 字节 JSON>", "dim");
        } else {
          log("← " + text, "dim");
        }
        for (let i = 0; i < waiters.length; i++) {
          if (waiters[i].pred(text, raw)) {
            const w = waiters.splice(i, 1)[0];
            clearTimeout(w.timer);
            w.resolve(text);
            return;
          }
        }
      }

      function onStat(ev) {
        const dv = ev.target.value;
        const raw = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
        notify(new TextDecoder().decode(raw), raw);
      }

      // 串口回包：设备把协议行写成 "CI:<payload>"，其它行是设备日志。
      function onSerialLine(line) {
        if (line.startsWith("CI:")) notify(line.slice(3), null);
        else log("设备 " + line, "dim");
      }

      // ── 写入 ────────────────────────────────────────────────────
      async function writeCmd(s) {
        log("→ CMD " + s, "dim");
        if (link === "serial") {
          if (!serialWriter) throw new Error("串口未打开");
          await serialWriter.write(new TextEncoder().encode("CI:" + s + "\n"));
          return;
        }
        const bytes = new TextEncoder().encode(s);
        if (typeof chCmd.writeValueWithResponse === "function")
          await chCmd.writeValueWithResponse(bytes);
        else await chCmd.writeValue(bytes); // 旧实现兜底
      }

      // 图片裸字节：串口直接写，BLE 按特征值写入方式分流
      async function sendData(slice) {
        if (link === "serial") {
          if (!serialWriter) throw new Error("串口未打开");
          await serialWriter.write(slice);
          return;
        }
        if (noResp) await chData.writeValueWithoutResponse(slice);
        else await chData.writeValueWithResponse(slice);
      }

      // ── 连接 ────────────────────────────────────────────────────
      // 上传按钮取决于「已连接 + 已有打包结果」，单独管理
      function updateUpBtn() {
        $("btnUpload").disabled = !(packed && connected && !busy);
      }

      function setConnected(on) {
        const off = !on || busy;
        for (const id of [
          "btnInfo",
          "btnList",
          "btnClear",
          "btnTestImg",
          "btnSelfTest",
        ])
          $(id).disabled = off;
        $("btnConnect").disabled = on || busy;
        $("btnSerial").disabled = on || busy;
        $("btnDisconnect").disabled = !on || busy;
        document
          .querySelectorAll("#list button")
          .forEach((b) => (b.disabled = off));
        updateUpBtn();
        updateCalBtn();
      }

      // 断开后的统一收尾：作废全部句柄引用，并立刻让还在等回包的 Promise 失败。
      // 不清的话，下一次 connect 会复用到已经失效的 server / ch*，第一步
      // getPrimaryService 就会抛 "GATT Server is disconnected"。
      function teardown(reason) {
        server = null;
        chCmd = null;
        chData = null;
        chStat = null;
        mtu = 23;
        noResp = false;
        connected = false;
        for (const w of waiters.splice(0, waiters.length)) {
          clearTimeout(w.timer);
          w.reject(new Error(reason || "连接已断开"));
        }
      }

      function onGattDisconnected(ev) {
        if (ev.target !== device) return; // 上一次 device 的迟到事件，别牵连当前连接
        // 主动 disconnect() 会先调 teardown（server 已置空），据此区分是不是意外掉线
        const unexpected = server !== null;
        teardown("连接已断开");
        link = null;
        setConnected(false);
        if (unexpected) report("设备已断开（链路已失效，请重新连接）", "bad");
        else report("已断开连接", "dim");
      }

      async function connect() {
        if (!navigator.bluetooth) {
          log(
            "✗ 拿不到 navigator.bluetooth：请用 https:// 或 http://localhost 打开本页",
            "bad",
          );
          return;
        }

        // 切到蓝牙前先放开串口，两条链路不能同时占着设备
        await closeSerial(true);
        teardown(null);
        link = null;

        // 同一次页面会话内重复连接：先释放上一次的句柄，绝不复用。
        if (device && device.gatt.connected) {
          log("释放上一次的连接…", "dim");
          device.gatt.disconnect();
        }

        log("请求设备…（弹出的列表里选 ColorInk-XXXX）");
        try {
          device = await navigator.bluetooth.requestDevice({
            filters: [{ namePrefix: "ColorInk" }], // 广播名以 ColorInk- 开头
            optionalServices: [SVC], // 自定义服务必须在这里授权，否则取服务会失败
          });
          device.addEventListener("gattserverdisconnected", onGattDisconnected);

          server = await device.gatt.connect();
          const svc = await server.getPrimaryService(SVC);
          chCmd = await svc.getCharacteristic(CMD);
          chData = await svc.getCharacteristic(DATA);
          chStat = await svc.getCharacteristic(STAT);

          await chStat.startNotifications();
          chStat.addEventListener("characteristicvaluechanged", onStat);

          noResp = typeof chData.writeValueWithoutResponse === "function";
          report("已连接：" + (device.name || "(未命名)"), "ok");
          log(
            noResp
              ? "✓ 支持 writeValueWithoutResponse"
              : "⚠ 本浏览器没有 writeValueWithoutResponse，回退为带响应写入（会慢很多）",
            noResp ? "ok" : "bad",
          );

          const pong = waitFor((t) => t === "pong", 3000, "pong");
          await writeCmd("ping");
          await pong;
          log("✓ 命令通道往返正常", "ok");

          link = "ble";
          connected = true;
          setConnected(true);
          await queryInfo();
          await refreshList();
        } catch (err) {
          // 半途失败也要清干净：否则状态停在"device/server 非空但全是死的"，
          // 下一次连接继续踩同一个坑。
          teardown("连接失败");
          link = null;
          setConnected(false);
          if (/disconnected/i.test(err.message || "")) {
            log(
              "提示：链路多半还被宿主端的 GATT 缓存占着。关掉其它开着本页的标签页、" +
                "在系统蓝牙设置里删除 ColorInk 设备，或长按板上 BOOT 键 2s 强制断开后再试。",
              "dim",
            );
          } else if (/not found/i.test(err.message || "")) {
            log(
              "提示：设备上没有 ColorInk 的 GATT 服务——选到的多半是别的蓝牙设备，" +
                "请重新连接并认准 ColorInk-XXXX。",
              "dim",
            );
          }
          throw err; // 交给 guarded 统一打红字
        }
      }

      function disconnect() {
        const linked = !!(device && device.gatt.connected);
        // 先收尾再断：这样 gattserverdisconnected 回调里能看出是主动断开，
        // 不会把它误报成意外的红字掉线。
        teardown("已主动断开");
        link = null;
        if (linked) device.gatt.disconnect();
        setConnected(false);
        if (!linked) log("当前没有已连接的设备", "dim");
      }

      // ── 串口连接（Web Serial）────────────────────────────────────
      // 设备端 serial_service.cpp 用 "CI:" 开头的行做命令与回包，图片数据在
      // begin 之后直接以裸字节写入，所以这里只按行解析回包、按块写数据。
      async function closeSerial(silent) {
        const port = serialPort;
        const reader = serialReader;
        const writer = serialWriter;
        serialPort = null;
        serialReader = null;
        serialWriter = null;
        serialBuf = "";
        if (reader) {
          try {
            await reader.cancel();
          } catch (_) {}
          // 读取循环会在 cancel 后释放 reader 锁，等它一拍再关端口
          await new Promise((r) => setTimeout(r, 60));
        }
        if (writer) {
          try {
            writer.releaseLock();
          } catch (_) {}
        }
        if (port) {
          try {
            await port.close();
          } catch (_) {}
        }
        if (!silent) report("已断开串口", "dim");
      }

      function handleSerialClosed() {
        serialPort = null;
        serialReader = null;
        if (serialWriter) {
          try {
            serialWriter.releaseLock();
          } catch (_) {}
        }
        serialWriter = null;
        link = null;
        teardown("串口已断开");
        setConnected(false);
        report("串口连接已断开（请重新连接）", "bad");
      }

      async function serialReadLoop(port) {
        const dec = new TextDecoder();
        let stop = false;
        try {
          while (!stop && port.readable) {
            const reader = port.readable.getReader();
            serialReader = reader;
            try {
              while (true) {
                const { value, done } = await reader.read();
                if (done) {
                  stop = true;
                  break;
                }
                serialBuf += dec.decode(value, { stream: true });
                let i;
                while ((i = serialBuf.indexOf("\n")) >= 0) {
                  const line = serialBuf.slice(0, i).replace(/\r$/, "");
                  serialBuf = serialBuf.slice(i + 1);
                  if (line) onSerialLine(line);
                }
              }
            } catch (err) {
              if (serialPort)
                log("串口读取中断：" + (err.message || err), "bad");
            } finally {
              serialReader = null;
              try {
                reader.releaseLock();
              } catch (_) {}
            }
          }
        } finally {
          if (serialPort === port) handleSerialClosed();
        }
      }

      async function connectSerial() {
        if (!("serial" in navigator)) {
          log(
            "✗ 拿不到 navigator.serial：请用电脑版 Chrome / Edge，并通过 https:// 或 localhost 打开本页",
            "bad",
          );
          return;
        }

        // 蓝牙链路先让位
        if (link === "ble") {
          teardown("切换到串口");
          try {
            if (device && device.gatt.connected) device.gatt.disconnect();
          } catch (_) {}
        }
        await closeSerial(true);
        teardown(null);
        link = null;

        let port = null;
        try {
          port = await navigator.serial.requestPort();
        } catch (err) {
          if (err && err.name === "NotFoundError") {
            log("已取消选择串口", "dim");
            return;
          }
          throw err;
        }

        log("打开串口…（115200，设备端单包 " + SERIAL_CHUNK + " 字节）");
        try {
          await port.open({ baudRate: 115200 });
          serialPort = port;
          serialWriter = port.writable.getWriter();
          serialBuf = "";
          link = "serial";
          chunkSize = SERIAL_CHUNK;
          serialReadLoop(port);

          const pong = waitFor((t) => t === "pong", 3000, "pong");
          await writeCmd("ping");
          await pong;
          log("✓ 串口命令通道往返正常", "ok");

          connected = true;
          setConnected(true);
          const info = port.getInfo ? port.getInfo() : {};
          report(
            "已连接：USB 串口" +
              (info.usbVendorId
                ? " (" +
                  info.usbVendorId.toString(16) +
                  ":" +
                  (info.usbProductId || 0).toString(16) +
                  ")"
                : ""),
            "ok",
          );
          await queryInfo();
          await refreshList();
        } catch (err) {
          await closeSerial(true);
          teardown("串口连接失败");
          link = null;
          setConnected(false);
          if (/failed to open|access denied/i.test(err.message || "")) {
            log(
              "提示：串口被占用。关掉 Arduino 串口监视器、platformio device monitor 或其它占用本串口的程序再试。",
              "dim",
            );
          } else if (/timeout|超时/i.test(err.message || "")) {
            log(
              "提示：没收到 pong。确认烧的是本仓库固件、波特率 115200，并换一条能传数据的 USB 线。",
              "dim",
            );
          }
          throw err;
        }
      }

      async function queryInfo() {
        const p = waitFor((t) => t.startsWith("info:"), 3000, "info");
        await writeCmd("info");
        const f = (await p).split(":");
        if (link === "serial") {
          chunkSize = SERIAL_CHUNK;
          log(
            "串口链路：单包 " +
              chunkSize +
              " 字节，设备空闲堆 = " +
              f[2] +
              " 字节，已开机 " +
              (parseInt(f[3], 10) / 1000).toFixed(1) +
              "s",
            "ok",
          );
          return;
        }
        mtu = parseInt(f[1], 10) || 23;
        chunkSize = Math.min(MAX_CHUNK, mtu - 3);
        log(
          "协商 MTU = " + mtu + "，单包有效载荷 = " + chunkSize + " 字节",
          "ok",
        );
        log(
          "设备空闲堆 = " +
            f[2] +
            " 字节，已开机 " +
            (parseInt(f[3], 10) / 1000).toFixed(1) +
            "s",
        );
      }

      // ── 图库列表 ────────────────────────────────────────────────
      // 设备按 chunk:<本片起始偏移>/<总长>:<数据> 分片推送。偏移是自描述的，
      // 所以乱序/重发都能正确落位；拼到 got == total 即完整。
      // 串口下一行就是完整 JSON；BLE 才需要按分片拼装。
      async function fetchList() {
        if (link === "serial") {
          const p = waitFor(
            (t) => t.startsWith("list:") || t.startsWith("err:"),
            5000,
            "list",
          );
          await writeCmd("list");
          const txt = await p;
          if (!txt.startsWith("list:")) throw new Error(txt.slice(4));
          return JSON.parse(txt.slice(5));
        }
        return new Promise((resolve, reject) => {
          let buf = null,
            total = 0,
            got = 0;
          const t = setTimeout(() => {
            const i = waiters.findIndex((w) => w.pred === pred);
            if (i >= 0) waiters.splice(i, 1);
            reject(new Error("等待列表分片超时（5s）"));
          }, 5000);

          function pred(text, raw) {
            // 头部固定 ASCII：只在原始字节里找前两个 ':'，避免误判名称里的字符
            let c2 = -1,
              seen = 0;
            for (let i = 0; i < raw.length && i < 32; i++) {
              if (raw[i] === 0x3a && ++seen === 2) {
                c2 = i;
                break;
              }
            }
            if (c2 < 0) return false;
            const m = /^chunk:(\d+)\/(\d+)$/.exec(
              new TextDecoder().decode(raw.subarray(0, c2)),
            );
            if (!m) return false;

            const off = parseInt(m[1], 10);
            if (!buf) {
              total = parseInt(m[2], 10);
              buf = new Uint8Array(total);
            }
            const payload = raw.subarray(c2 + 1);
            buf.set(payload, off);
            got += payload.length;
            if (got < total) return false;

            clearTimeout(t);
            resolve(JSON.parse(new TextDecoder().decode(buf)));
            return true;
          }

          // 先挂上等待者再发命令：回包可能在 writeCmd 的 await 期间就到达
          waiters.push({ pred, resolve: () => {}, reject, timer: t });
          writeCmd("list").catch((err) => {
            clearTimeout(t);
            reject(err);
          });
        });
      }

      async function refreshList() {
        const data = await fetchList();
        const box = $("list");
        box.innerHTML = "";
        const imgs = data.images || [];
        log(
          "图库共 " +
            imgs.length +
            " 张，当前 #" +
            data.current +
            "，剩余空间 " +
            (data.free / 1024).toFixed(0) +
            " KB",
          "ok",
        );
        if (imgs.length === 0) {
          box.innerHTML = '<div class="empty">图库为空，先上传一张图片。</div>';
          return;
        }
        for (const im of imgs) {
          const card = document.createElement("div");
          card.className = "card";
          card.innerHTML =
            '<div class="cinfo">' +
            '<span class="cid">#' +
            im.id +
            "</span>" +
            '<span class="nm">' +
            escapeHtml(im.name) +
            "</span>" +
            (im.current ? '<span class="cur">当前</span>' : "") +
            "</div>" +
            '<span class="cmeta">' +
            Math.round(im.size / 1024) +
            " KB</span>";
          const show = document.createElement("button");
          show.textContent = "显示";
          show.onclick = () => guarded(() => displayImage(im.id));
          const del = document.createElement("button");
          del.textContent = "删除";
          del.className = "danger";
          del.onclick = () => guarded(() => deleteImage(im.id));
          card.append(show, del);
          box.appendChild(card);
        }
      }

      async function displayImage(id) {
        const queued = waitFor(
          (t) => t === "display:queued:" + id || t.startsWith("err:"),
          3000,
          "display:queued",
        );
        await writeCmd("display:" + id);
        const q = await queued;
        if (q.startsWith("err:")) {
          report("✗ 显示被拒：" + q.slice(4), "bad");
          toast("✗ 显示被拒：" + q.slice(4), 5000, "bad");
          return;
        }
        report("已排队，等待面板刷新（约 15 秒，期间请勿离开本页）…");
        toast("屏幕刷新中（约 15 秒，请勿离开本页）…", 16000);
        const done = await waitFor(
          (t) => t === "display:done:" + id || t === "display:fail:" + id,
          DISPLAY_MS,
          "display:done:" + id,
        );
        const ok = done === "display:done:" + id;
        report(ok ? "✓ 已显示 #" + id : "✗ 显示失败 #" + id, ok ? "ok" : "bad");
        toast(
          ok ? "✓ 已显示 #" + id : "✗ 显示失败 #" + id,
          3500,
          ok ? "ok" : "bad",
        );
        await refreshList();
      }

      async function deleteImage(id) {
        const p = waitFor(
          (t) => t.startsWith("delete:ok:") || t.startsWith("delete:fail:"),
          5000,
          "delete",
        );
        await writeCmd("delete:" + id);
        const txt = await p;
        const ok = txt === "delete:ok:" + id;
        report(
          ok ? "✓ 已删除 #" + id : "✗ 删除失败：" + txt,
          ok ? "ok" : "bad",
        );
        if (ok) toast("✓ 已删除 #" + id, 2500, "ok");
        else toast("✗ 删除失败：" + txt, 5000, "bad");
        await refreshList();
      }

      async function clearScreen() {
        const queued = waitFor(
          (t) => t === "clear:queued",
          3000,
          "clear:queued",
        );
        await writeCmd("clear");
        await queued;
        report("已排队，等待清屏（约 15 秒）…");
        toast("清屏中（约 15 秒，请勿离开本页）…", 16000);
        const done = await waitFor(
          (t) => t === "clear:done" || t === "clear:fail",
          DISPLAY_MS,
          "clear:done",
        );
        const ok = done === "clear:done";
        report(ok ? "✓ 已清屏" : "✗ 清屏失败", ok ? "ok" : "bad");
        toast(ok ? "✓ 已清屏" : "✗ 清屏失败", 3500, ok ? "ok" : "bad");
        await refreshList();
      }

      // ── 传输 ────────────────────────────────────────────────────
      // 与固件 adlerUpdate() 逐位一致的实现：模 65521，逐字节累加。
      function adler32(buf) {
        let a = 1,
          b = 0;
        for (let i = 0; i < buf.length; i++) {
          a += buf[i];
          if (a >= 65521) a -= 65521;
          b += a;
          if (b >= 65521) b -= 65521;
        }
        return (((b << 16) | a) >>> 0).toString(16).padStart(8, "0");
      }

      // 把 buf 按批推送，每批等设备 ack 再发下一批。
      // writeValueWithoutResponse / 串口写入都不排队，猛灌会静默丢包（表现为图像整体错位）。
      // onProgress(sent)：每推完一批回调一次，用于状态行显示进度。
      async function sendBytes(buf, onProgress) {
        const total = buf.length;
        const chunk = chunkSize;
        const step = link === "serial" ? SERIAL_ACK_STEP : ACK_STEP;
        const t0 = performance.now();
        let sent = 0,
          stalls = 0;
        while (sent < total) {
          const batchEnd = Math.min(sent + step, total);
          while (sent < batchEnd) {
            const end = Math.min(sent + chunk, batchEnd);
            await sendData(buf.subarray(sent, end));
            sent = end;
          }
          if (onProgress) onProgress(sent);
          if (sent < total) {
            await waitFor(
              (t) => {
                const m = /^ack:(\d+)$/.exec(t);
                return m && parseInt(m[1], 10) >= sent;
              },
              5000,
              "ack>=" + sent,
            );
            stalls++;
          }
        }
        return { secs: (performance.now() - t0) / 1000, stalls };
      }

      // ── 图片处理管线（基座移植自 WiFi 版 webapp_html.h）──────────
      // 新增 moink 风格的暖色色域映射与自动色阶，其余仍走 Oklab 分类 + 边缘门控扩散
      function clamp8(v) {
        return v < 0 ? 0 : v > 255 ? 255 : v;
      }
      function hexToRgb(h) {
        return [
          parseInt(h.slice(1, 3), 16),
          parseInt(h.slice(3, 5), 16),
          parseInt(h.slice(5, 7), 16),
        ];
      }
      const QUANT_RGB = QUANT_PAL.map(hexToRgb);
      // sRGB → 线性光查找表
      const LIN = new Float32Array(256);
      for (let i = 0; i < 256; i++) {
        const c = i / 255;
        LIN[i] = c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
      }
      function rgbToOklab(r, g, b) {
        const lr = LIN[clamp8(r) | 0],
          lg = LIN[clamp8(g) | 0],
          lb = LIN[clamp8(b) | 0];
        const l = Math.cbrt(
          0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb,
        );
        const m = Math.cbrt(
          0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb,
        );
        const s = Math.cbrt(
          0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb,
        );
        return [
          0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
          1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
          0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
        ];
      }
      const QUANT_OK = QUANT_RGB.map((c) => rgbToOklab(c[0], c[1], c[2]));

      // 中性灰分类：2=纯灰（只许黑白） 1=近灰（暗部/亮部只许黑白） 0=彩色
      function neutralClass(L, a, b) {
        const chroma = Math.hypot(a, b);
        if (chroma < 0.02) return 2;
        if (chroma < 0.04) return 1;
        return 0;
      }
      // 各目标色的固有偏好微调（比 Oklab 距离小两个量级，只做边界推手）
      function colorBiasCost(k, L, a, b, r, g, bl) {
        let bias = 0;
        const chroma = Math.hypot(a, b);
        if (k === 0) {
          if (L < 0.22) bias -= 0.0045;
          else if (L < 0.34) bias -= 0.0015;
        }
        if (k === 1) {
          if (L > 0.92) bias -= 0.004;
          else if (L > 0.82 && chroma < 0.07) bias -= 0.0015;
        }
        if (k === 2) {
          if (r > 125 && g > 105 && bl < 120 && Math.abs(r - g) < 85)
            bias -= 0.0018;
        }
        if (k === 3) {
          if (r > g * 1.22 && r > bl * 1.35) bias -= 0.0018;
        }
        return bias;
      }
      function allowedByNeutral(k, neutral, L) {
        if (neutral >= 2) return k === 0 || k === 1;
        if (neutral === 1) {
          if (L < 0.4) return k === 0 || k === 1;
          if (L > 0.72) return k === 0 || k === 1;
        }
        return true;
      }
      // 边缘越强，误差扩散权重越低（边缘处宁可硬切也不把误差洒过边界）
      function edgeGate(edge) {
        if (edge < 0.025) return 1.0;
        if (edge < 0.06) return 0.92;
        if (edge < 0.11) return 0.68;
        if (edge < 0.18) return 0.32;
        if (edge < 0.28) return 0.1;
        return 0.025;
      }

      // 第一遍：初始四色标注（同时缓存每个像素的 Oklab，供后续复用）
      function buildInitialLabels(px, w, h) {
        const labels = new Uint8Array(w * h);
        const lab = new Float32Array(w * h * 3);
        for (let y = 0; y < h; y++) {
          for (let x = 0; x < w; x++) {
            const p = (y * w + x) * 4;
            const r = px[p],
              g = px[p + 1],
              b = px[p + 2];
            const q = rgbToOklab(r, g, b);
            const L = q[0],
              a = q[1],
              bb = q[2];
            const li = (y * w + x) * 3;
            lab[li] = L;
            lab[li + 1] = a;
            lab[li + 2] = bb;
            const neutral = neutralClass(L, a, bb);
            let best = 0,
              bestCost = Infinity;
            for (let k = 0; k < 4; k++) {
              if (!allowedByNeutral(k, neutral, L)) continue;
              const t = QUANT_OK[k];
              const dl = L - t[0],
                da = a - t[1],
                db = bb - t[2];
              let cost = dl * dl + da * da + db * db;
              cost += colorBiasCost(k, L, a, bb, r, g, b);
              if (neutral === 1 && (k === 2 || k === 3)) cost += 0.0035;
              if (cost < bestCost) {
                bestCost = cost;
                best = k;
              }
            }
            labels[y * w + x] = best;
          }
        }
        return { labels, lab };
      }

      // 孤点修正：与四邻大多不同的像素，若邻域多数色合法则改判
      function refineIsolatedLabels(labels, lab, w, h) {
        const out = new Uint8Array(labels);
        for (let y = 1; y < h - 1; y++) {
          for (let x = 1; x < w - 1; x++) {
            const idx = y * w + x;
            const k = labels[idx];
            let same = 0;
            const n = [
              labels[idx - 1],
              labels[idx + 1],
              labels[idx - w],
              labels[idx + w],
            ];
            for (let i = 0; i < 4; i++) if (n[i] === k) same++;
            if (same >= 2) continue;
            const votes = [0, 0, 0, 0];
            for (let i = 0; i < 4; i++) votes[n[i]]++;
            let best = k,
              bestVotes = votes[k];
            for (let c = 0; c < 4; c++) {
              if (votes[c] > bestVotes) {
                best = c;
                bestVotes = votes[c];
              }
            }
            if (best !== k && bestVotes >= 3) {
              const li = idx * 3;
              const L = lab[li],
                a = lab[li + 1],
                b = lab[li + 2];
              const neutral = neutralClass(L, a, b);
              if (allowedByNeutral(best, neutral, L)) out[idx] = best;
            }
          }
        }
        return out;
      }

      // 边缘强度图（右向 / 下向），Oklab 距离 → 0..1
      function buildEdgeMaps(lab, labels, w, h) {
        const edgeR = new Uint8Array(w * h);
        const edgeD = new Uint8Array(w * h);
        function calcEdge(i, j) {
          const a = i * 3,
            b = j * 3;
          const dl = lab[a] - lab[b],
            da = lab[a + 1] - lab[b + 1],
            db = lab[a + 2] - lab[b + 2];
          const d = Math.sqrt(dl * dl + da * da + db * db);
          return Math.min(1, d * 3.2);
        }
        for (let y = 0; y < h; y++) {
          const row = y * w;
          for (let x = 0; x < w; x++) {
            const i = row + x;
            if (x + 1 < w) edgeR[i] = Math.round(calcEdge(i, i + 1) * 255);
            if (y + 1 < h) edgeD[i] = Math.round(calcEdge(i, i + w) * 255);
          }
        }
        return { edgeR, edgeD };
      }

      // 误差扩散目标权重：核权重 × 边缘门控，整体按 1.75 倍上限重新归一
      function buildDiffusionWeights(kernel, x, y, rtl, edgeR, edgeD) {
        const items = [];
        let sum = 0;
        for (let m = 0; m < kernel.length; m++) {
          const ox = kernel[m][0],
            oy = kernel[m][1],
            wt = kernel[m][2];
          const nx = rtl ? x - ox : x + ox;
          const ny = y + oy;
          if (nx < 0 || nx >= SW || ny < 0 || ny >= SH) continue;
          let edge;
          if (oy === 0) {
            const left = Math.min(x, nx);
            edge = edgeR[y * SW + left] / 255;
          } else {
            const topY = Math.min(y, ny);
            edge = edgeD[topY * SW + x] / 255;
          }
          const gate = edgeGate(edge);
          const weight = wt * gate;
          if (weight > 0) {
            items.push({ nx, ny, ox, oy, weight });
            sum += weight;
          }
        }
        if (sum > 1e-8) {
          let baseSum = 0;
          for (let m = 0; m < kernel.length; m++) {
            const ox = kernel[m][0],
              oy = kernel[m][1];
            const nx = rtl ? x - ox : x + ox;
            const ny = y + oy;
            if (nx >= 0 && nx < SW && ny >= 0 && ny < SH)
              baseSum += kernel[m][2];
          }
          const targetSum = Math.min(baseSum, sum * 1.75);
          const scale = targetSum / sum;
          for (const it of items) it.weight *= scale;
        }
        return items;
      }

      const BAYER8 = [
        [0, 32, 8, 40, 2, 34, 10, 42],
        [48, 16, 56, 24, 50, 18, 58, 26],
        [12, 44, 4, 36, 14, 46, 6, 38],
        [60, 28, 52, 20, 62, 30, 54, 22],
        [3, 35, 11, 43, 1, 33, 9, 41],
        [51, 19, 59, 27, 49, 17, 57, 25],
        [15, 47, 7, 39, 13, 45, 5, 37],
        [63, 31, 55, 23, 61, 29, 53, 21],
      ];
      const FSK = [
        [1, 0, 7 / 16],
        [-1, 1, 3 / 16],
        [0, 1, 5 / 16],
        [1, 1, 1 / 16],
      ];
      const ATK = [
        [1, 0, 1 / 8],
        [2, 0, 1 / 8],
        [-1, 1, 1 / 8],
        [0, 1, 1 / 8],
        [1, 1, 1 / 8],
        [2, 1, 1 / 8],
      ];

      // 主量化：work(SW×SH RGBA) → 预览 ImageData + 2bpp packedBuf
      function quantizeTo2bpp(work) {
        const mode = $("dmode").value;
        const doSharp = $("sharpen").checked;
        const doVivid = $("vivid").checked;
        const wc = work.getContext("2d");
        const image = wc.getImageData(0, 0, SW, SH);
        const px = image.data;
        // 管线顺序与 moink 一致：曝光 → 色域映射 → 锐化 → 抖动
        if ($("autolev").checked) autoLevels(px, SW, SH);
        colorMapPixels(px, $("cmap").value);
        if (doVivid) enhance(px);
        if (doSharp) sharpenLuma(px, SW, SH);

        const first = buildInitialLabels(px, SW, SH);
        const labels = refineIsolatedLabels(first.labels, first.lab, SW, SH);
        const { edgeR, edgeD } = buildEdgeMaps(first.lab, labels, SW, SH);

        const out = new ImageData(SW, SH);
        const oc = out.data;
        const packedBuf = new Uint8Array(FRAME_BYTES);

        const diffMode = mode === "fs" || mode === "atkinson";
        const kernel = mode === "atkinson" ? ATK : FSK;
        let currentErr = diffMode ? new Float32Array(SW * 3) : null;
        let nextErr = diffMode ? new Float32Array(SW * 3) : null;

        function candidateIndex(r, g, b, oR, oG, oB, oL, oA, oBok) {
          const q = rgbToOklab(r, g, b);
          const L = q[0],
            a = q[1],
            bb = q[2];
          const neutral = neutralClass(oL, oA, oBok);
          let best = 0,
            bestCost = Infinity;
          for (let k = 0; k < 4; k++) {
            if (!allowedByNeutral(k, neutral, oL)) continue;
            const t = QUANT_OK[k];
            const dl = L - t[0],
              da = a - t[1],
              db = bb - t[2];
            let cost = dl * dl + da * da + db * db;
            cost += colorBiasCost(k, oL, oA, oBok, oR, oG, oB);
            if (neutral === 1 && (k === 2 || k === 3)) cost += 0.0035;
            if (oL < 0.14 && k !== 0) cost += 0.005;
            if (oL > 0.965 && k !== 1) cost += 0.004;
            if (cost < bestCost) {
              bestCost = cost;
              best = k;
            }
          }
          return best;
        }

        for (let y = 0; y < SH; y++) {
          const rtl = diffMode && y & 1; // 蛇形扫描：奇数行反向，消除方向纹理
          for (let n = 0; n < SW; n++) {
            const x = rtl ? SW - 1 - n : n;
            const idx = y * SW + x;
            const p4 = idx * 4;
            const originalR = px[p4],
              originalG = px[p4 + 1],
              originalB = px[p4 + 2];
            const li = idx * 3;
            const originalL = first.lab[li],
              originalA = first.lab[li + 1],
              originalBOK = first.lab[li + 2];
            let r, g, b;
            if (diffMode) {
              const ei = x * 3;
              r = clamp8(originalR + currentErr[ei]);
              g = clamp8(originalG + currentErr[ei + 1]);
              b = clamp8(originalB + currentErr[ei + 2]);
            } else {
              r = originalR;
              g = originalG;
              b = originalB;
              if (mode === "bayer") {
                const t = (BAYER8[y & 7][x & 7] + 0.5) / 64 - 0.5;
                const d = t * 36;
                r = clamp8(r + d);
                g = clamp8(g + d);
                b = clamp8(b + d);
              }
            }
            const k = candidateIndex(
              r,
              g,
              b,
              originalR,
              originalG,
              originalB,
              originalL,
              originalA,
              originalBOK,
            );
            const outRgb = QUANT_RGB[k];
            // 预览用可校准色板；packedBuf 只写颜色索引
            const displayRgb = hexToRgb(DISPLAY_PAL[k]);
            oc[p4] = displayRgb[0];
            oc[p4 + 1] = displayRgb[1];
            oc[p4 + 2] = displayRgb[2];
            oc[p4 + 3] = 255;
            // 打包：帧缓冲一"行" = 屏幕竖直方向的 768 像素（转置映射，已在屏上验证）
            const dx = SH - 1 - y;
            const byteI = x * ROW_BYTES + (dx >> 2);
            const shift = 6 - ((dx & 3) << 1);
            packedBuf[byteI] |= CODES[k] << shift;
            if (diffMode) {
              const er = r - outRgb[0],
                eg = g - outRgb[1],
                eb = b - outRgb[2];
              const diffusion = buildDiffusionWeights(
                kernel,
                x,
                y,
                rtl,
                edgeR,
                edgeD,
              );
              for (const item of diffusion) {
                const ei = item.nx * 3;
                if (item.oy === 0) {
                  currentErr[ei] += er * item.weight;
                  currentErr[ei + 1] += eg * item.weight;
                  currentErr[ei + 2] += eb * item.weight;
                } else {
                  nextErr[ei] += er * item.weight;
                  nextErr[ei + 1] += eg * item.weight;
                  nextErr[ei + 2] += eb * item.weight;
                }
              }
            }
          }
          if (diffMode && y + 1 < SH) {
            const tmp = currentErr;
            currentErr = nextErr;
            nextErr = tmp;
            nextErr.fill(0);
          }
        }
        quantCache.getContext("2d").putImageData(out, 0, 0);
        quantCacheOK.v = true;
        return packedBuf;
      }

      // 鲜艳：饱和 ×1.10 + 对比 ×1.04
      function enhance(px) {
        const sat = 1.1,
          con = 1.04;
        for (let i = 0; i < px.length; i += 4) {
          const lum = px[i] * 0.299 + px[i + 1] * 0.587 + px[i + 2] * 0.114;
          px[i] = (lum + (px[i] - lum) * sat - 128) * con + 128;
          px[i + 1] = (lum + (px[i + 1] - lum) * sat - 128) * con + 128;
          px[i + 2] = (lum + (px[i + 2] - lum) * sat - 128) * con + 128;
        }
      }

      // USM 锐化（亮度域），跳过边缘 1px
      function sharpenLuma(px, w, h) {
        const src = new Uint8ClampedArray(px);
        const row = w * 4;
        const amount = 0.42;
        for (let y = 1; y < h - 1; y++) {
          for (let x = 1; x < w - 1; x++) {
            const p = (y * w + x) * 4;
            const y0 = src[p] * 0.299 + src[p + 1] * 0.587 + src[p + 2] * 0.114;
            const yl =
              src[p - 4] * 0.299 + src[p - 3] * 0.587 + src[p - 2] * 0.114;
            const yr =
              src[p + 4] * 0.299 + src[p + 5] * 0.587 + src[p + 6] * 0.114;
            const yu =
              src[p - row] * 0.299 +
              src[p - row + 1] * 0.587 +
              src[p - row + 2] * 0.114;
            const yd =
              src[p + row] * 0.299 +
              src[p + row + 1] * 0.587 +
              src[p + row + 2] * 0.114;
            const avg = (yl + yr + yu + yd) / 4;
            const delta = (y0 - avg) * amount;
            px[p] = clamp8(src[p] + delta);
            px[p + 1] = clamp8(src[p + 1] + delta);
            px[p + 2] = clamp8(src[p + 2] + delta);
          }
        }
      }

      // ── 暖色色域映射（参考 moink 的有限调色板成像）──────────────────
      // 黑/白/红/黄四种墨水能"平均"出来的只有 R 最大、B 最小的暖色带（色相 0°~60°）；
      // 蓝/青/绿无论怎么抖动都只会落到黑或白。所以先按暖色亲和度把颜色推进暖带，
      // 再用幂律提饱和（线性拉到满饱和会把整张图变成平涂红块），最后才做四色量化。
      const WARM_SAT_K = 0.75;

      function clamp01(v) {
        return v < 0 ? 0 : v > 1 ? 1 : v;
      }

      // 归一化 RGB(0..1) → HSL，h/s/l 均为 0..1
      function rgb2hsl01(r, g, b) {
        const mx = Math.max(r, g, b),
          mn = Math.min(r, g, b);
        const l = (mx + mn) * 0.5,
          d = mx - mn;
        if (d < 1e-12) return [0, 0, l];
        const den = l > 0.5 ? 2 - mx - mn : mx + mn;
        const s = Math.min(1, d / Math.max(den, 1e-12));
        const rc = (mx - r) / d,
          gc = (mx - g) / d,
          bc = (mx - b) / d;
        let hh;
        if (mx === r) hh = bc - gc;
        else if (mx === g) hh = 2 + rc - bc;
        else hh = 4 + gc - rc;
        return [(((hh / 6) % 1) + 1) % 1, s, l];
      }
      function hue2rgb01(p, q, t) {
        t = ((t % 1) + 1) % 1;
        if (t < 1 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
        return p;
      }
      function hsl2rgb01(h, s, l) {
        const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        const p = 2 * l - q;
        return [
          hue2rgb01(p, q, h + 1 / 3),
          hue2rgb01(p, q, h),
          hue2rgb01(p, q, h - 1 / 3),
        ];
      }

      // 暖色亲和度：红→黄带内 =1，向紫红/黄绿两侧平滑降到 0。
      // 用 smoothstep 而不是硬阈值，否则平滑渐变上会留一条等值线接缝。
      function warmAffinity(hd) {
        if (hd > 300) {
          const t = clamp01((hd - 300) / 50);
          return t * t * (3 - 2 * t);
        }
        if (hd < 110) {
          const t = clamp01((hd - 60) / 50);
          return 1 - t * t * (3 - 2 * t);
        }
        return 0;
      }

      // 日落渐变 LUT（luma → RGB），全局暖化时叠一层，让压缩后的色相保留明暗层次
      const WARM_RAMP = (function () {
        const stops = [
          [0, [0, 0, 0]],
          [0.1, [26, 6, 4]],
          [0.26, [92, 18, 12]],
          [0.42, [168, 40, 16]],
          [0.56, [214, 86, 18]],
          [0.7, [236, 140, 26]],
          [0.84, [248, 200, 60]],
          [0.93, [255, 235, 150]],
          [1, [255, 255, 255]],
        ];
        const lut = new Float32Array(256 * 3);
        for (let i = 0; i < 256; i++) {
          const t = i / 255;
          let k = 0;
          while (k < stops.length - 2 && t >= stops[k + 1][0]) k++;
          const x0 = stops[k][0],
            c0 = stops[k][1],
            x1 = stops[k + 1][0],
            c1 = stops[k + 1][1];
          const f = (t - x0) / (x1 - x0);
          for (let c = 0; c < 3; c++)
            lut[i * 3 + c] = c0[c] + (c1[c] - c0[c]) * f;
        }
        return lut;
      })();

      // natural  = 只提暖色：暖色区域提饱和并拉向暖带，冷色只降饱和、不硬拉（保留照片原貌）
      // adaptive = 把整圈色相压进 0°~60°，再叠日落渐变（红黄最多，但整体明显偏暖）
      // gray     = 亮度直出，适合文档/线稿
      // naive    = 不映射，作为对照
      function colorMapPixels(px, mode) {
        if (mode === "naive") return;
        for (let i = 0; i < px.length; i += 4) {
          const r = px[i],
            g = px[i + 1],
            b = px[i + 2];
          if (mode === "gray") {
            const L = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            px[i] = px[i + 1] = px[i + 2] = L;
            continue;
          }
          const hsl = rgb2hsl01(r / 255, g / 255, b / 255);
          const hd = hsl[0] * 360;
          let h2, s2;
          if (mode === "adaptive") {
            h2 = (((30 + (hsl[0] - 0.5) * 58) % 360) + 360) % 360;
            s2 = hsl[1];
          } else {
            // 选择性模型：亲和度高 → 幂律提饱和 + 色相拉向 30°；
            // 亲和度低 → 只降饱和并轻微向暖带漂移，保留冷色的黑白观感。
            const wa = warmAffinity(hd);
            const boost = clamp01(0.6 * wa);
            s2 =
              Math.pow(hsl[1], 1 - WARM_SAT_K * boost) *
              (1 - clamp01(0.25 * (1 - wa)));
            let drift = -0.25 * (1 - wa) * (hd - 60);
            drift = Math.max(-60, Math.min(0, drift));
            const d2 = ((30 - hd + 540) % 360) - 180;
            const pull = Math.max(-18, Math.min(18, 0.12 * d2));
            h2 = (((hd + drift + wa * pull) % 360) + 360) % 360;
          }
          const fin = hsl2rgb01(h2 / 360, s2, hsl[2]);
          if (mode === "adaptive") {
            const li =
              (Math.round(0.2126 * r + 0.7152 * g + 0.0722 * b) | 0) * 3;
            const cast = 0.4;
            px[i] = fin[0] * 255 * (1 - cast) + WARM_RAMP[li] * cast;
            px[i + 1] = fin[1] * 255 * (1 - cast) + WARM_RAMP[li + 1] * cast;
            px[i + 2] = fin[2] * 255 * (1 - cast) + WARM_RAMP[li + 2] * cast;
          } else {
            px[i] = fin[0] * 255;
            px[i + 1] = fin[1] * 255;
            px[i + 2] = fin[2] * 255;
          }
        }
      }

      // 自动色阶：按亮度直方图裁掉两端 0.5% 的极值，再把有效区间线性拉满 0..255
      function autoLevels(px, w, h) {
        const n = w * h;
        const hist = new Uint32Array(256);
        for (let i = 0, p = 0; i < n; i++, p += 4) {
          const L = Math.round(
            0.2126 * px[p] + 0.7152 * px[p + 1] + 0.0722 * px[p + 2],
          );
          hist[L < 0 ? 0 : L > 255 ? 255 : L]++;
        }
        const clip = Math.max(1, Math.round(n * 0.005));
        let c = 0,
          lo = 0,
          hi = 255;
        for (let i = 0; i < 256; i++) {
          c += hist[i];
          if (c >= clip) {
            lo = i;
            break;
          }
        }
        c = 0;
        for (let i = 0; i < 256; i++) {
          c += hist[i];
          if (c >= n - clip) {
            hi = i;
            break;
          }
        }
        if (hi - lo < 8) return;
        const scale = 255 / (hi - lo);
        for (let i = 0; i < px.length; i += 4) {
          px[i] = clamp8((px[i] - lo) * scale);
          px[i + 1] = clamp8((px[i + 1] - lo) * scale);
          px[i + 2] = clamp8((px[i + 2] - lo) * scale);
        }
      }

      // ── 选图 / 预览 / 位置拖动 ───────────────────────────────────
      const drop = $("drop");
      const fileIn = $("file");
      const preview = $("preview");
      const perr = $("perr");
      let srcImage = null;
      let packed = null;
      const quantCache = document.createElement("canvas");
      quantCache.width = SW;
      quantCache.height = SH;
      const quantCacheOK = { v: false };
      const previewStage = document.createElement("canvas");
      previewStage.width = SW;
      previewStage.height = SH;
      const srcCache = document.createElement("canvas");
      srcCache.width = SW;
      srcCache.height = SH;
      const srcCacheKey = { rot: -1, fit: "", src: null };
      let imgOffsetX = 0,
        imgOffsetY = 0;

      function clampOffset() {
        imgOffsetX = Math.max(-SW, Math.min(SW, imgOffsetX | 0));
        imgOffsetY = Math.max(-SH, Math.min(SH, imgOffsetY | 0));
      }
      function syncPosUI() {
        $("posx").textContent = imgOffsetX;
        $("posy").textContent = imgOffsetY;
        $("posrow").style.display = "flex";
      }

      // 旋转 + 填充 → srcCache（带分步减半抗锯齿），结果按 (rot,fit,src) 缓存
      function prepareSrcCache(img) {
        const rot = +$("rot").value;
        const fit = $("fit").value;
        if (
          srcCacheKey.src === img &&
          srcCacheKey.rot === rot &&
          srcCacheKey.fit === fit
        )
          return;

        const landscape = rot === 0 || rot === 180;
        const tw = landscape ? SW : SH;
        const th = landscape ? SH : SW;
        let src = img,
          cw = img.width,
          ch = img.height;
        const finalScale =
          fit === "cover"
            ? Math.max(tw / cw, th / ch)
            : Math.min(tw / cw, th / ch);
        const targetW = cw * finalScale,
          targetH = ch * finalScale;
        // 一步缩太狠会糊：先按 ≤2× 分步减半到接近目标
        while (cw >> 1 >= targetW && ch >> 1 >= targetH) {
          const nc = document.createElement("canvas");
          nc.width = Math.max(1, cw >> 1);
          nc.height = Math.max(1, ch >> 1);
          const nx = nc.getContext("2d");
          nx.imageSmoothingEnabled = true;
          nx.imageSmoothingQuality = "high";
          nx.drawImage(src, 0, 0, nc.width, nc.height);
          src = nc;
          cw = nc.width;
          ch = nc.height;
        }
        const tmp = document.createElement("canvas");
        tmp.width = tw;
        tmp.height = th;
        const tc = tmp.getContext("2d");
        tc.imageSmoothingEnabled = true;
        tc.imageSmoothingQuality = "high";
        tc.fillStyle = "#ffffff";
        tc.fillRect(0, 0, tw, th);
        const s =
          fit === "cover"
            ? Math.max(tw / cw, th / ch)
            : Math.min(tw / cw, th / ch);
        const dw = cw * s,
          dh = ch * s;
        tc.drawImage(src, (tw - dw) / 2, (th - dh) / 2, dw, dh);
        // 旋转落位（0°/90°/180°/270°）
        const sc = srcCache.getContext("2d");
        sc.imageSmoothingEnabled = true;
        sc.imageSmoothingQuality = "high";
        sc.fillStyle = "#ffffff";
        sc.fillRect(0, 0, SW, SH);
        sc.save();
        if (rot === 0) {
          sc.drawImage(tmp, 0, 0);
        } else if (rot === 90) {
          sc.translate(SW, 0);
          sc.rotate(Math.PI / 2);
          sc.drawImage(tmp, 0, 0);
        } else if (rot === 180) {
          sc.translate(SW, SH);
          sc.rotate(Math.PI);
          sc.drawImage(tmp, 0, 0);
        } else {
          sc.translate(0, SH);
          sc.rotate(-Math.PI / 2);
          sc.drawImage(tmp, 0, 0);
        }
        sc.restore();
        srcCacheKey.src = img;
        srcCacheKey.rot = rot;
        srcCacheKey.fit = fit;
      }

      // srcCache + 用户拖动偏移 → 工作画布
      function drawToWork() {
        const work = document.createElement("canvas");
        work.width = SW;
        work.height = SH;
        const wc = work.getContext("2d");
        wc.imageSmoothingEnabled = true;
        wc.imageSmoothingQuality = "high";
        wc.fillStyle = "#ffffff";
        wc.fillRect(0, 0, SW, SH);
        const ox = imgOffsetX | 0,
          oy = imgOffsetY | 0;
        const srcX = ox < 0 ? -ox : 0;
        const srcY = oy < 0 ? -oy : 0;
        const dstX = Math.max(0, ox);
        const dstY = Math.max(0, oy);
        const srcW = Math.max(0, Math.min(SW - srcX, SW - dstX));
        const srcH = Math.max(0, Math.min(SH - srcY, SH - dstY));
        if (srcW > 0 && srcH > 0) {
          wc.drawImage(
            srcCache,
            srcX,
            srcY,
            srcW,
            srcH,
            dstX,
            dstY,
            srcW,
            srcH,
          );
        }
        return work;
      }

      function process() {
        perr.textContent = "";
        if (!srcImage) return;
        try {
          prepareSrcCache(srcImage);
          const work = drawToWork();
          packed = quantizeTo2bpp(work);
          preview.style.display = "block";
          $("pvEmpty").hidden = true;
          renderPreview();
          syncPosUI();
        } catch (e) {
          packed = null;
          perr.textContent = "处理失败：" + e.message;
          console.error(e);
        }
        updateUpBtn();
      }

      function renderPreview() {
        if (!quantCacheOK.v) return;
        const rot = +$("rot").value;
        const landscape = rot === 90 || rot === 270;
        const pw = landscape ? SH : SW;
        const ph = landscape ? SW : SH;
        if (preview.width !== pw || preview.height !== ph) {
          preview.width = pw;
          preview.height = ph;
        }
        // 先在帧坐标系合成，再按显示方向旋转回来
        const sc = previewStage.getContext("2d");
        sc.fillStyle = "#ffffff";
        sc.fillRect(0, 0, SW, SH);
        const ox = imgOffsetX | 0,
          oy = imgOffsetY | 0;
        const srcX = ox < 0 ? -ox : 0;
        const srcY = oy < 0 ? -oy : 0;
        const dstX = Math.max(0, ox);
        const dstY = Math.max(0, oy);
        const srcW = Math.max(0, Math.min(SW - srcX, SW - dstX));
        const srcH = Math.max(0, Math.min(SH - srcY, SH - dstY));
        if (srcW > 0 && srcH > 0) {
          sc.drawImage(
            quantCache,
            srcX,
            srcY,
            srcW,
            srcH,
            dstX,
            dstY,
            srcW,
            srcH,
          );
        }
        const pc = preview.getContext("2d");
        pc.fillStyle = "#ffffff";
        pc.fillRect(0, 0, pw, ph);
        pc.save();
        if (rot === 90) {
          // 90° 写入的帧逆时针转回来，横屏查看
          pc.translate(0, ph);
          pc.rotate(-Math.PI / 2);
        } else if (rot === 270) {
          pc.translate(pw, 0);
          pc.rotate(Math.PI / 2);
        }
        pc.drawImage(previewStage, 0, 0);
        pc.restore();
        $("pvMeta").textContent =
          pw + " × " + ph + " · 四色 2bpp · " + (landscape ? "横屏" : "竖屏");
      }

      function loadFile(file) {
        const allowed = ["image/jpeg", "image/png", "image/webp"];
        if (!file || !allowed.includes(file.type)) {
          perr.textContent = "请选择 JPG / PNG / WebP 图片";
          return;
        }
        const url = URL.createObjectURL(file);
        const img = new Image();
        img.onload = () => {
          srcImage = img;
          imgOffsetX = 0;
          imgOffsetY = 0;
          const base = (file.name || "").replace(/\.[^.]+$/, "");
          if (!$("iname").value) $("iname").value = base.slice(0, 24);
          process();
          URL.revokeObjectURL(url);
        };
        img.onerror = () => {
          perr.textContent = "图片解码失败";
          URL.revokeObjectURL(url);
        };
        img.src = url;
      }

      drop.addEventListener("click", () => fileIn.click());
      fileIn.addEventListener("change", () => loadFile(fileIn.files[0]));
      ["dragover", "dragenter"].forEach((ev) => {
        drop.addEventListener(ev, (e) => {
          e.preventDefault();
          drop.classList.add("hover");
        });
      });
      ["dragleave", "dragend"].forEach((ev) => {
        drop.addEventListener(ev, () => drop.classList.remove("hover"));
      });
      drop.addEventListener("drop", (e) => {
        e.preventDefault();
        drop.classList.remove("hover");
        loadFile(e.dataTransfer.files[0]);
      });
      $("rot").addEventListener("change", process);
      $("fit").addEventListener("change", process);
      $("dmode").addEventListener("change", process);
      $("sharpen").addEventListener("change", process);
      $("vivid").addEventListener("change", process);
      $("cmap").addEventListener("change", process);
      $("autolev").addEventListener("change", process);

      // 预览上拖动调位置（Pointer Events，兼容触摸）
      (function setupDrag() {
        let dragging = false,
          startX = 0,
          startY = 0,
          baseX = 0,
          baseY = 0,
          rafId = 0;
        function applyMove(clientX, clientY) {
          const r = preview.getBoundingClientRect();
          const rot = +$("rot").value;
          const dx = clientX - startX,
            dy = clientY - startY;
          if (rot === 90 || rot === 270) {
            // 横屏预览：预览 X/Y 分别对应帧的 Y/X，需换算拖动方向
            const dxF = dx * (SH / r.width);
            const dyF = dy * (SW / r.height);
            if (rot === 90) {
              imgOffsetX = baseX - dyF;
              imgOffsetY = baseY + dxF;
            } else {
              imgOffsetX = baseX + dyF;
              imgOffsetY = baseY - dxF;
            }
          } else {
            const sx = SW / r.width,
              sy = SH / r.height;
            imgOffsetX = baseX + dx * sx;
            imgOffsetY = baseY + dy * sy;
          }
          clampOffset();
          syncPosUI();
          if (!rafId) {
            rafId = requestAnimationFrame(() => {
              rafId = 0;
              renderPreview();
            });
          }
        }
        function onDown(e) {
          if (preview.style.display === "none") return;
          if (!srcImage) return;
          if (e.button !== undefined && e.button !== 0) return;
          dragging = true;
          preview.classList.add("dragging");
          startX = e.clientX;
          startY = e.clientY;
          baseX = imgOffsetX;
          baseY = imgOffsetY;
          try {
            preview.setPointerCapture(e.pointerId);
          } catch (_) {}
          e.preventDefault();
        }
        function onMove(e) {
          if (!dragging) return;
          applyMove(e.clientX, e.clientY);
          e.preventDefault();
        }
        function onUp(e) {
          if (!dragging) return;
          dragging = false;
          preview.classList.remove("dragging");
          if (rafId) {
            cancelAnimationFrame(rafId);
            rafId = 0;
          }
          try {
            if (e && e.pointerId !== undefined)
              preview.releasePointerCapture(e.pointerId);
          } catch (_) {}
          process(); // 拖完重新量化
        }
        preview.addEventListener("pointerdown", onDown);
        preview.addEventListener("pointermove", onMove);
        preview.addEventListener("pointerup", onUp);
        preview.addEventListener("pointercancel", onUp);
        preview.addEventListener("pointerleave", onUp);
      })();

      $("posreset").addEventListener("click", () => {
        imgOffsetX = 0;
        imgOffsetY = 0;
        syncPosUI();
        process();
      });

      // ── 显示色板校准（只影响浏览器预览）──────────────────────────
      function syncDisplayPal() {
        DISPLAY_PAL.forEach((h, i) => {
          $("sw" + i).style.background = h;
          $("pc" + i).value = h;
        });
      }
      syncDisplayPal();
      for (let i = 0; i < 4; i++) {
        $("pc" + i).addEventListener("change", (e) => {
          DISPLAY_PAL[i] = e.target.value;
          try {
            localStorage.setItem(DISPLAY_PAL_KEY, JSON.stringify(DISPLAY_PAL));
          } catch (_) {}
          syncDisplayPal();
          if (calLabels) calPaintPreview();
          process();
        });
      }
      $("calreset").addEventListener("click", () => {
        DISPLAY_PAL = DISPLAY_PAL_DEFAULT.slice();
        try {
          localStorage.removeItem(DISPLAY_PAL_KEY);
        } catch (_) {}
        syncDisplayPal();
        if (calLabels) calPaintPreview();
        process();
      });
      $("calshot").addEventListener("click", () => {
        const c = document.createElement("canvas");
        c.width = SW;
        c.height = SH;
        const x = c.getContext("2d");
        QUANT_PAL.forEach((h, i) => {
          x.fillStyle = h;
          x.fillRect(0, Math.round((i * SH) / 4), SW, Math.round(SH / 4));
        });
        const a = document.createElement("a");
        a.href = c.toDataURL("image/png");
        a.download = "colorink_palette_calibration.png";
        a.click();
      });

      // ── 日历（四色）─────────────────────────────────────────────
      // 日历不复用照片管线：自动色阶/色域映射/误差扩散会把文字边缘糊掉。
      // 这里只做「纯四色绘制 → 中性像素黑白归并 → 最近色量化 → 共用转置打包」。
      const CAL_KEY = "colorink_calendar_cfg_v1";
      const CAL_DEF = {
        theme: "classic",
        layout: "portrait",
        follow: true,
        date: "",
        weekStart: 1,
        accent: "yellow",
        scale: "normal",
        lunar: "main",
        festDot: true,
        festCd: true,
        weekNum: true,
        remain: true,
        hero: true,
        grid: true,
        weekend: true,
        todayHi: true,
        note: "",
        name: "",
      };
      let calCfg = Object.assign({}, CAL_DEF);
      try {
        Object.assign(
          calCfg,
          JSON.parse(localStorage.getItem(CAL_KEY) || "{}"),
        );
      } catch (_) {}

      const CAL_INK = QUANT_PAL[0],
        CAL_WHITE = QUANT_PAL[1];
      const CAL_YELLOW = QUANT_PAL[2],
        CAL_RED = QUANT_PAL[3];
      let calLabels = null,
        calPacked = null,
        calTimer = 0,
        calReady = false;

      function calPad2(n) {
        return n < 10 ? "0" + n : "" + n;
      }
      function calYmd(d) {
        return (
          d.getFullYear() +
          "-" +
          calPad2(d.getMonth() + 1) +
          "-" +
          calPad2(d.getDate())
        );
      }
      function calTodayDate() {
        const n = new Date();
        return new Date(n.getFullYear(), n.getMonth(), n.getDate(), 12, 0, 0);
      }
      function calParseDate(s) {
        const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(s || "");
        if (!m) return calTodayDate();
        return new Date(+m[1], +m[2] - 1, +m[3], 12, 0, 0);
      }
      function calDayDiff(a, b) {
        const ua = Date.UTC(a.getFullYear(), a.getMonth(), a.getDate());
        const ub = Date.UTC(b.getFullYear(), b.getMonth(), b.getDate());
        return Math.round((ub - ua) / 86400000);
      }
      function calWeekdayName(d) {
        return "周" + "日一二三四五六"[d.getDay()];
      }
      function calIsoWeek(d) {
        const t = new Date(
          Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()),
        );
        const dayNum = (t.getUTCDay() + 6) % 7;
        t.setUTCDate(t.getUTCDate() - dayNum + 3);
        const firstThursday = new Date(Date.UTC(t.getUTCFullYear(), 0, 4));
        return 1 + Math.round((t - firstThursday) / 604800000);
      }

      // 浏览器内置中国历法：兼容 Chrome/Edge/Safari，拿不到就自动跳过农历
      const CAL_LUNAR_FMT = (function () {
        try {
          if (typeof Intl !== "undefined" && Intl.DateTimeFormat)
            return new Intl.DateTimeFormat("zh-CN-u-ca-chinese", {
              year: "numeric",
              month: "long",
              day: "numeric",
            });
        } catch (_) {}
        return null;
      })();
      const CAL_LUNAR_OK = !!(
        CAL_LUNAR_FMT && typeof CAL_LUNAR_FMT.formatToParts === "function"
      );
      const CAL_LUNAR_MONTH = {
        正月: 1,
        一月: 1,
        二月: 2,
        三月: 3,
        四月: 4,
        五月: 5,
        六月: 6,
        七月: 7,
        八月: 8,
        九月: 9,
        十月: 10,
        十一月: 11,
        冬月: 11,
        十二月: 12,
        腊月: 12,
      };
      const CAL_LUNAR_DAYS = [
        "初一",
        "初二",
        "初三",
        "初四",
        "初五",
        "初六",
        "初七",
        "初八",
        "初九",
        "初十",
        "十一",
        "十二",
        "十三",
        "十四",
        "十五",
        "十六",
        "十七",
        "十八",
        "十九",
        "二十",
        "廿一",
        "廿二",
        "廿三",
        "廿四",
        "廿五",
        "廿六",
        "廿七",
        "廿八",
        "廿九",
        "三十",
      ];
      const CAL_LUNAR_CACHE = new Map();

      function calLunarOf(d) {
        if (!CAL_LUNAR_OK) return null;
        const key = calYmd(d);
        if (CAL_LUNAR_CACHE.has(key)) return CAL_LUNAR_CACHE.get(key);
        let v = null;
        try {
          let monthName = "",
            day = 0,
            ganzhi = "",
            lunarYear = 0;
          for (const p of CAL_LUNAR_FMT.formatToParts(d)) {
            if (p.type === "month") monthName = p.value;
            else if (p.type === "day") day = parseInt(p.value, 10) || 0;
            else if (p.type === "yearName") ganzhi = p.value;
            else if (p.type === "relatedYear")
              lunarYear = parseInt(p.value, 10) || 0;
          }
          const month = CAL_LUNAR_MONTH[monthName.replace(/^闰/, "")] || 0;
          if (month && day) {
            v = {
              monthName,
              month,
              leap: monthName.startsWith("闰"),
              day,
              dayName: CAL_LUNAR_DAYS[day - 1] || "",
              ganzhi,
              lunarYear,
            };
          }
        } catch (_) {}
        if (CAL_LUNAR_CACHE.size > 4000) CAL_LUNAR_CACHE.clear();
        CAL_LUNAR_CACHE.set(key, v);
        return v;
      }

      const CAL_SOLAR_FEST = [
        [1, 1, "元旦"],
        [2, 14, "情人节"],
        [3, 8, "妇女节"],
        [5, 1, "劳动节"],
        [6, 1, "儿童节"],
        [9, 10, "教师节"],
        [10, 1, "国庆节"],
        [12, 24, "平安夜"],
        [12, 25, "圣诞节"],
      ];
      const CAL_LUNAR_FEST = [
        [1, 1, "春节"],
        [1, 15, "元宵"],
        [5, 5, "端午"],
        [7, 7, "七夕"],
        [8, 15, "中秋"],
        [9, 9, "重阳"],
      ];

      function calFestivalOf(d) {
        const m = d.getMonth() + 1,
          day = d.getDate();
        for (const f of CAL_SOLAR_FEST)
          if (f[0] === m && f[1] === day) return f[2];
        const l = calLunarOf(d);
        if (!l || l.leap) return "";
        for (const f of CAL_LUNAR_FEST)
          if (f[0] === l.month && f[1] === l.day) return f[2];
        if (l.month === 12 && (l.day === 29 || l.day === 30)) {
          const n = new Date(
            d.getFullYear(),
            d.getMonth(),
            d.getDate() + 1,
            12,
          );
          const ln = calLunarOf(n);
          if (ln && ln.month === 1 && ln.day === 1) return "除夕";
        }
        return "";
      }

      function calNextFestival(d) {
        for (let i = 0; i <= 400; i++) {
          const n = new Date(
            d.getFullYear(),
            d.getMonth(),
            d.getDate() + i,
            12,
          );
          const f = calFestivalOf(n);
          if (f) return { name: f, days: i };
        }
        return null;
      }

      function calSize(base) {
        const k = { compact: 0.94, normal: 1, large: 1.08 }[calCfg.scale] || 1;
        return Math.max(17, Math.round(base * k));
      }
      function calFont(ctx, size, weight) {
        ctx.font =
          (weight || 400) +
          " " +
          size +
          'px "PingFang SC","Microsoft YaHei","Noto Sans SC",sans-serif';
      }
      function calText(ctx, text, x, y, size, color, weight, align) {
        calFont(ctx, size, weight);
        ctx.fillStyle = color;
        ctx.textAlign = align || "left";
        ctx.textBaseline = "alphabetic";
        ctx.fillText(text, x, y);
        ctx.textAlign = "left";
      }
      function calRoundRect(ctx, x, y, w, h, r) {
        ctx.beginPath();
        if (ctx.roundRect) ctx.roundRect(x, y, w, h, r);
        else ctx.rect(x, y, w, h);
        ctx.fill();
      }

      // 主题调色板：经典为白底黑字红点缀；月白为白底加黄饰带（横贯顶部的季节色带）；
      // 墨黑为黑底白字黄点缀（黑底上红色发暗，改用黄提亮）。banner 为 null 表示无饰带。
      function calPal() {
        if (calCfg.theme === "ink")
          return {
            bg: CAL_INK,
            fg: CAL_WHITE,
            accent: CAL_YELLOW,
            weekend: CAL_YELLOW,
            fest: CAL_YELLOW,
            banner: null,
          };
        if (calCfg.theme === "moon")
          return {
            bg: CAL_WHITE,
            fg: CAL_INK,
            accent: CAL_RED,
            weekend: CAL_RED,
            fest: CAL_RED,
            banner: CAL_YELLOW,
          };
        return {
          bg: CAL_WHITE,
          fg: CAL_INK,
          accent: CAL_RED,
          weekend: CAL_RED,
          fest: CAL_RED,
          banner: null,
        };
      }

      // 月历网格：竖版与横版共用，仅几何参数和字号不同
      function calDrawGrid(ctx, d, g) {
        const pal = calPal();
        const y = d.getFullYear(),
          m = d.getMonth();
        const ws = calCfg.weekStart;
        const first = new Date(y, m, 1, 12);
        const startCol = (first.getDay() - ws + 7) % 7;
        const ndays = new Date(y, m + 1, 0).getDate();
        const today = calTodayDate();
        const rows = Math.ceil((startCol + ndays) / 7);
        const rowH = Math.min(g.maxRowH, (g.bottom - g.rowY) / rows);
        const offsetY = (g.bottom - g.rowY - rows * rowH) / 2;

        for (let i = 0; i < 7; i++) {
          const wd = (ws + i) % 7;
          const col =
            calCfg.weekend && (wd === 0 || wd === 6) ? pal.weekend : pal.fg;
          calText(
            ctx,
            "日一二三四五六"[wd],
            g.x0 + g.colW * (i + 0.5),
            g.headY,
            g.headFont,
            col,
            700,
            "center",
          );
        }

        for (let n = 1; n <= ndays; n++) {
          const cell = startCol + n - 1;
          const cx = g.x0 + g.colW * ((cell % 7) + 0.5);
          const cy =
            g.rowY +
            offsetY +
            Math.floor(cell / 7) * rowH +
            (rowH + g.font * 0.7) / 2;
          const date = new Date(y, m, n, 12);
          const wd = date.getDay();
          const isToday =
            date.getFullYear() === today.getFullYear() &&
            date.getMonth() === today.getMonth() &&
            date.getDate() === today.getDate();
          const marked = !!(calCfg.todayHi && isToday);
          if (marked) {
            ctx.fillStyle = calCfg.accent === "red" ? CAL_RED : CAL_YELLOW;
            calRoundRect(
              ctx,
              cx - g.blockW / 2,
              cy - g.font * 0.35 - g.blockH / 2,
              g.blockW,
              g.blockH,
              8,
            );
          }
          let numColor = pal.fg;
          if (marked) numColor = calCfg.accent === "red" ? CAL_WHITE : pal.fg;
          else if (calCfg.weekend && (wd === 0 || wd === 6))
            numColor = pal.weekend;
          calText(
            ctx,
            String(n),
            cx,
            cy,
            g.font,
            numColor,
            marked ? 700 : 600,
            "center",
          );

          if (calCfg.festDot && calFestivalOf(date)) {
            ctx.fillStyle = marked
              ? calCfg.accent === "red"
                ? CAL_WHITE
                : CAL_RED
              : pal.fest;
            ctx.beginPath();
            ctx.arc(cx, cy + g.font * 0.28, g.dotR, 0, Math.PI * 2);
            ctx.fill();
          }
        }
      }

      function calDrawPortrait(ctx, d) {
        const pal = calPal();
        const y = d.getFullYear(),
          m = d.getMonth() + 1,
          day = d.getDate();
        const lunar = calCfg.lunar !== "off" ? calLunarOf(d) : null;
        const next = calCfg.festCd ? calNextFestival(d) : null;
        const remain = calDayDiff(d, new Date(y, 11, 31, 12));

        ctx.fillStyle = pal.bg;
        ctx.fillRect(0, 0, SW, SH);
        // 月白主题：顶部黄饰带，标题嵌在带内；周数在黄底上改用黑字
        const hasNote = !!calCfg.note.trim();
        if (pal.banner) {
          ctx.fillStyle = pal.banner;
          ctx.fillRect(0, 0, SW, 96);
        }
        calText(
          ctx,
          y + "年" + m + "月",
          30,
          68,
          calSize(36),
          pal.fg,
          700,
          "left",
        );
        if (calCfg.weekNum)
          calText(
            ctx,
            "第 " + calIsoWeek(d) + " 周",
            522,
            64,
            calSize(20),
            pal.banner ? pal.fg : pal.accent,
            700,
            "right",
          );

        if (calCfg.hero)
          calText(
            ctx,
            String(day),
            26,
            246,
            calSize(150),
            pal.fg,
            700,
            "left",
          );
        const hx = calCfg.hero ? 230 : 30;
        let ly = calCfg.hero ? 140 : 96;
        calText(
          ctx,
          calWeekdayName(d),
          hx,
          ly,
          calSize(32),
          pal.fg,
          700,
          "left",
        );
        ly += 36;
        calText(
          ctx,
          y + "年" + m + "月" + day + "日",
          hx,
          ly,
          calSize(19),
          pal.fg,
          500,
          "left",
        );
        ly += 32;
        if (lunar) {
          calText(
            ctx,
            "农历 " +
              lunar.monthName +
              lunar.dayName +
              (calCfg.lunar === "full" ? " · " + lunar.ganzhi + "年" : ""),
            hx,
            ly,
            calSize(19),
            pal.fg,
            500,
            "left",
          );
          ly += 32;
        }
        if (calCfg.remain) {
          calText(
            ctx,
            "今年剩余 " + remain + " 天",
            hx,
            ly,
            calSize(21),
            pal.accent,
            700,
            "left",
          );
          ly += 32;
        }
        if (next) {
          calText(
            ctx,
            next.days === 0
              ? "今日 " + next.name
              : "距 " + next.name + " 还有 " + next.days + " 天",
            hx,
            ly,
            calSize(18),
            pal.accent,
            600,
            "left",
          );
        }

        ctx.fillStyle = pal.fg;
        ctx.fillRect(30, 292, 492, 4);

        if (calCfg.grid) {
          calDrawGrid(ctx, d, {
            x0: 30,
            colW: (SW - 60) / 7,
            headY: 334,
            rowY: 382,
            bottom: pal.banner
              ? hasNote
                ? 704
                : 712
              : hasNote
                ? 720
                : 736,
            maxRowH: 72,
            headFont: calSize(21),
            font: calSize(27),
            blockW: calSize(58),
            blockH: calSize(52),
            dotR: 3.5,
          });
        }
        if (hasNote) {
          // 月白主题：底部对称黄饰带，备注黑字嵌在带内
          if (pal.banner) {
            ctx.fillStyle = pal.banner;
            ctx.fillRect(0, 724, SW, 44);
          }
          calText(
            ctx,
            calCfg.note.trim(),
            SW / 2,
            pal.banner ? 750 : 746,
            calSize(17),
            pal.fg,
            500,
            "center",
          );
        }
      }

      function calDrawLandscape(ctx, d) {
        const pal = calPal();
        const y = d.getFullYear(),
          m = d.getMonth() + 1,
          day = d.getDate();
        const lunar = calCfg.lunar !== "off" ? calLunarOf(d) : null;
        const next = calCfg.festCd ? calNextFestival(d) : null;
        const remain = calDayDiff(d, new Date(y, 11, 31, 12));

        ctx.fillStyle = pal.bg;
        ctx.fillRect(0, 0, SH, SW);
        // 月白主题：顶部黄饰带，标题嵌在带内；周数在黄底上改用黑字
        const hasNote = !!calCfg.note.trim();
        if (pal.banner) {
          ctx.fillStyle = pal.banner;
          ctx.fillRect(0, 0, SH, 76);
        }
        calText(
          ctx,
          y + "年" + m + "月",
          30,
          54,
          calSize(34),
          pal.fg,
          700,
          "left",
        );
        if (calCfg.weekNum)
          calText(
            ctx,
            "第 " + calIsoWeek(d) + " 周",
            738,
            50,
            calSize(19),
            pal.banner ? pal.fg : pal.accent,
            700,
            "right",
          );

        if (calCfg.hero)
          calText(
            ctx,
            String(day),
            28,
            176,
            calSize(120),
            pal.fg,
            700,
            "left",
          );
        const hx = calCfg.hero ? 206 : 30;
        let ly = calCfg.hero ? 104 : 80;
        calText(
          ctx,
          calWeekdayName(d),
          hx,
          ly,
          calSize(30),
          pal.fg,
          700,
          "left",
        );
        ly += 30;
        calText(
          ctx,
          y + "年" + m + "月" + day + "日",
          hx,
          ly,
          calSize(19),
          pal.fg,
          500,
          "left",
        );
        ly += 28;
        if (lunar) {
          calText(
            ctx,
            "农历 " +
              lunar.monthName +
              lunar.dayName +
              (calCfg.lunar === "full" ? " · " + lunar.ganzhi + "年" : ""),
            hx,
            ly,
            calSize(19),
            pal.fg,
            500,
            "left",
          );
          ly += 28;
        }
        if (calCfg.remain) {
          calText(
            ctx,
            "今年剩余 " + remain + " 天",
            hx,
            ly,
            calSize(20),
            pal.accent,
            700,
            "left",
          );
          ly += 28;
        }
        if (next) {
          calText(
            ctx,
            next.days === 0
              ? "今日 " + next.name
              : "距 " + next.name + " 还有 " + next.days + " 天",
            hx,
            ly,
            calSize(18),
            pal.accent,
            600,
            "left",
          );
        }

        ctx.fillStyle = pal.fg;
        ctx.fillRect(30, 238, 708, 4);

        if (calCfg.grid) {
          calDrawGrid(ctx, d, {
            x0: 30,
            colW: (SH - 60) / 7,
            headY: 270,
            rowY: 308,
            bottom: pal.banner
              ? hasNote
                ? 496
                : 502
              : hasNote
                ? 522
                : 538,
            maxRowH: 56,
            headFont: calSize(19),
            font: calSize(23),
            blockW: calSize(64),
            blockH: calSize(34),
            dotR: 3,
          });
        }
        if (hasNote) {
          // 月白主题：底部对称黄饰带，备注黑字嵌在带内
          if (pal.banner) {
            ctx.fillStyle = pal.banner;
            ctx.fillRect(0, 512, SH, 40);
          }
          calText(
            ctx,
            calCfg.note.trim(),
            SH / 2,
            pal.banner ? 538 : 542,
            calSize(17),
            pal.fg,
            500,
            "center",
          );
        }
      }

      function calComposeFrame(d) {
        const frame = document.createElement("canvas");
        frame.width = SW;
        frame.height = SH;
        const fc = frame.getContext("2d");
        fc.fillStyle = CAL_WHITE;
        fc.fillRect(0, 0, SW, SH);
        if (calCfg.layout === "landscape") {
          const raw = document.createElement("canvas");
          raw.width = SH;
          raw.height = SW;
          calDrawLandscape(raw.getContext("2d"), d);
          fc.imageSmoothingEnabled = false;
          fc.save();
          fc.translate(SW, 0);
          fc.rotate(Math.PI / 2);
          fc.drawImage(raw, 0, 0);
          fc.restore();
        } else {
          calDrawPortrait(fc, d);
        }
        return frame;
      }

      // 文字专用量化：中性像素只判黑/白，彩色像素取最近四色，不做抖动
      function calQuantize(frame) {
        const px = frame.getContext("2d").getImageData(0, 0, SW, SH).data;
        const labels = new Uint8Array(PIXELS);
        for (let i = 0, p = 0; i < PIXELS; i++, p += 4) {
          const q = rgbToOklab(px[p], px[p + 1], px[p + 2]);
          const L = q[0],
            a = q[1],
            b = q[2];
          let k;
          if (Math.sqrt(a * a + b * b) < 0.03) {
            k = L > 0.55 ? 1 : 0;
          } else {
            let best = 0,
              bestD = Infinity;
            for (let c = 0; c < 4; c++) {
              const t = QUANT_OK[c];
              const dl = L - t[0],
                da = a - t[1],
                db = b - t[2];
              const dist = dl * dl + da * da + db * db;
              if (dist < bestD) {
                bestD = dist;
                best = c;
              }
            }
            k = best;
          }
          labels[i] = k;
        }
        return labels;
      }

      function calPackLabels(labels) {
        const buf = new Uint8Array(FRAME_BYTES);
        for (let y = 0; y < SH; y++) {
          for (let x = 0; x < SW; x++) {
            const dx = SH - 1 - y;
            const byteI = x * ROW_BYTES + (dx >> 2);
            buf[byteI] |= CODES[labels[y * SW + x]] << (6 - ((dx & 3) << 1));
          }
        }
        return buf;
      }

      function calSyncSwatches() {
        DISPLAY_PAL.forEach((h, i) => {
          const el = $("calSw" + i);
          if (el) el.style.background = h;
        });
      }

      function calPaintPreview() {
        if (!calLabels) return;
        const landscape = calCfg.layout === "landscape";
        const cv = $("calPreview");
        const pw = landscape ? SH : SW;
        const ph = landscape ? SW : SH;
        if (cv.width !== pw || cv.height !== ph) {
          cv.width = pw;
          cv.height = ph;
        }
        const img = new ImageData(SW, SH);
        const d = img.data;
        const pal = DISPLAY_PAL.map(hexToRgb);
        for (let i = 0, p = 0; i < PIXELS; i++, p += 4) {
          const c = pal[calLabels[i]];
          d[p] = c[0];
          d[p + 1] = c[1];
          d[p + 2] = c[2];
          d[p + 3] = 255;
        }
        const ctx = cv.getContext("2d");
        if (landscape) {
          // 横版帧按顺时针 90° 写入设备，预览逆时针转回来，按横屏 768×552 显示
          const raw = document.createElement("canvas");
          raw.width = SW;
          raw.height = SH;
          raw.getContext("2d").putImageData(img, 0, 0);
          ctx.save();
          ctx.translate(0, ph);
          ctx.rotate(-Math.PI / 2);
          ctx.drawImage(raw, 0, 0);
          ctx.restore();
        } else {
          ctx.putImageData(img, 0, 0);
        }
        const meta = $("calMeta");
        if (meta)
          meta.textContent =
            (landscape ? "768 × 552 横版" : "552 × 768 竖版") +
            " · 四色量化 2bpp";
        calSyncSwatches();
      }

      function calAutoName(d) {
        const suffix =
          calCfg.theme === "ink"
            ? " 墨"
            : calCfg.theme === "moon"
              ? " 月"
              : "";
        return (
          "日历 " + calYmd(d) + (calCfg.layout === "landscape" ? " 横" : "") + suffix
        );
      }

      function calUpdateHint(d) {
        const bits = [];
        bits.push(
          calCfg.layout === "landscape"
            ? "横版 768×552 → 顺时针旋转 90° 写入帧，需横持设备查看"
            : "竖版 552×768",
        );
        if (calCfg.lunar !== "off") {
          const l = calLunarOf(d);
          if (!CAL_LUNAR_OK) bits.push("浏览器不支持农历，已跳过");
          else if (l)
            bits.push(
              "农历 " +
                l.monthName +
                l.dayName +
                (calCfg.lunar === "full" ? " · " + l.ganzhi + "年" : ""),
            );
        }
        bits.push("静态图片，日期变化需重新生成上传");
        $("calHint").textContent = bits.join(" · ");
      }

      function calCollect() {
        return {
          theme: $("calTheme").value,
          layout: $("calLayout").value,
          follow: $("calFollow").checked,
          date: $("calDate").value,
          weekStart: parseInt($("calWeekStart").value, 10),
          accent: $("calAccent").value,
          scale: $("calScale").value,
          lunar: $("calLunar").value,
          festDot: $("calFestDot").checked,
          festCd: $("calFestCd").checked,
          weekNum: $("calWeekNum").checked,
          remain: $("calRemain").checked,
          hero: $("calHero").checked,
          grid: $("calGrid").checked,
          weekend: $("calWeekend").checked,
          todayHi: $("calTodayHi").checked,
          note: $("calNote").value.slice(0, 24),
          name: $("calName").value.slice(0, 24),
        };
      }

      function calEffectiveDate() {
        if (calCfg.follow) return calTodayDate();
        return calParseDate(calCfg.date || $("calDate").value);
      }

      function calSave() {
        try {
          localStorage.setItem(CAL_KEY, JSON.stringify(calCfg));
        } catch (_) {}
      }

      function calRender() {
        if (!calReady) return;
        const err = $("calErr");
        err.textContent = "";
        try {
          calCfg = calCollect();
          calSave();
          const lsc = $("calConvert");
          if (lsc)
            lsc.classList.toggle("is-landscape", calCfg.layout === "landscape");
          const d = calEffectiveDate();
          const frame = calComposeFrame(d);
          calLabels = calQuantize(frame);
          calPacked = calPackLabels(calLabels);
          calPaintPreview();
          calUpdateHint(d);
          $("calName").placeholder = calAutoName(d);
        } catch (e) {
          calPacked = null;
          calLabels = null;
          err.textContent = "日历生成失败：" + e.message;
          console.error(e);
        }
        updateCalBtn();
      }

      function calScheduleRender() {
        clearTimeout(calTimer);
        calTimer = setTimeout(calRender, 80);
      }

      function calDownload() {
        if (!calLabels) return;
        const c = document.createElement("canvas");
        c.width = SW;
        c.height = SH;
        const ctx = c.getContext("2d");
        const img = new ImageData(SW, SH);
        const data = img.data;
        for (let i = 0, p = 0; i < PIXELS; i++, p += 4) {
          const rgb = QUANT_RGB[calLabels[i]];
          data[p] = rgb[0];
          data[p + 1] = rgb[1];
          data[p + 2] = rgb[2];
          data[p + 3] = 255;
        }
        ctx.putImageData(img, 0, 0);
        const d = calEffectiveDate();
        const a = document.createElement("a");
        a.href = c.toDataURL("image/png");
        a.download =
          "colorink_calendar_" +
          calYmd(d) +
          (calCfg.layout === "landscape" ? "_landscape" : "_portrait") +
          ".png";
        a.click();
      }

      function calApplyCfg() {
        $("calTheme").value = calCfg.theme || "classic";
        $("calLayout").value = calCfg.layout;
        $("calWeekStart").value = String(calCfg.weekStart);
        $("calAccent").value = calCfg.accent;
        $("calScale").value = calCfg.scale;
        $("calLunar").value = calCfg.lunar;
        $("calFestDot").checked = !!calCfg.festDot;
        $("calFestCd").checked = !!calCfg.festCd;
        $("calWeekNum").checked = !!calCfg.weekNum;
        $("calRemain").checked = !!calCfg.remain;
        $("calHero").checked = !!calCfg.hero;
        $("calGrid").checked = !!calCfg.grid;
        $("calWeekend").checked = !!calCfg.weekend;
        $("calTodayHi").checked = !!calCfg.todayHi;
        $("calFollow").checked = !!calCfg.follow;
        $("calNote").value = calCfg.note || "";
        $("calName").value = calCfg.name || "";
        $("calDate").value = calCfg.follow
          ? calYmd(calTodayDate())
          : calCfg.date || calYmd(calTodayDate());
        $("calDate").disabled = !!calCfg.follow;
      }

      function calInit() {
        if (calReady) return;
        calReady = true;
        calApplyCfg();
        const ids = [
          "calTheme",
          "calLayout",
          "calDate",
          "calWeekStart",
          "calAccent",
          "calScale",
          "calLunar",
          "calFestDot",
          "calFestCd",
          "calWeekNum",
          "calRemain",
          "calHero",
          "calGrid",
          "calWeekend",
          "calTodayHi",
          "calNote",
          "calName",
        ];
        for (const id of ids) {
          const el = $(id);
          const ev =
            el.tagName === "SELECT" || el.type === "checkbox"
              ? "change"
              : "input";
          el.addEventListener(ev, calScheduleRender);
        }
        $("calFollow").addEventListener("change", () => {
          $("calDate").disabled = $("calFollow").checked;
          if ($("calFollow").checked)
            $("calDate").value = calYmd(calTodayDate());
          calScheduleRender();
        });
        $("calToday").addEventListener("click", () => {
          $("calFollow").checked = true;
          $("calDate").disabled = true;
          $("calDate").value = calYmd(calTodayDate());
          calScheduleRender();
        });
        $("calPng").addEventListener("click", calDownload);
        $("calUpload").addEventListener("click", () => {
          if (!calPacked) return;
          const fallback = calAutoName(calEffectiveDate());
          guarded(() =>
            uploadImage(
              safeName($("calName").value, fallback),
              calPacked,
              $("calUpload"),
            ),
          );
        });
        calSyncSwatches();
      }

      function updateCalBtn() {
        const b = $("calUpload");
        if (b) b.disabled = !(calPacked && connected && !busy);
      }

      // ── 上传（BLE 通道）──────────────────────────────────────────
      // 名称只留可打印字符 / 中文，禁 ':'（固件按最后一个 ':' 切分数字，但别考验它）
      function safeName(s, fallback) {
        const n = String(s || "")
          .replace(/[:\x00-\x1f]/g, "")
          .trim()
          .slice(0, 24);
        return n || fallback;
      }

      // 105984 字节的合成图：按字节四等分 → 四条横向色带，
      // 每字节 = 4 个同色像素。色码 0=黑 1=白 2=黄 3=红。
      function makeTestImage() {
        const buf = new Uint8Array(IMG_BYTES);
        const band = IMG_BYTES / 4;
        for (let i = 0; i < IMG_BYTES; i++)
          buf[i] = Math.floor(i / band) * 0x55;
        return buf;
      }

      // 通用上传：begin → 数据 → end → stats 核对 → 刷新列表 → 显示。
      // 各阶段用 toast + 按钮文案给反馈（状态行同时更新，细节进日志）。
      async function uploadImage(name, buf, btn) {
        const labelBtn = btn || $("btnUpload");
        const origLabel = labelBtn.textContent;
        try {
          toast("准备上传…");
          const localSum = adler32(buf);

          const started = waitFor(
            (t) => t.startsWith("begin:ok:") || t.startsWith("err:"),
            4000,
            "begin",
          );
          await writeCmd("begin:" + name + ":" + buf.length);
          const b = await started;
          if (!b.startsWith("begin:ok:")) {
            const msg = b.slice(4);
            report("✗ 上传被拒：" + msg, "bad");
            toast("✗ 上传被拒：" + msg, 5000, "bad");
            return;
          }
          const id = parseInt(b.slice(9), 10);
          log("设备已开上传通道 #" + id + "，本地 Adler-32 = " + localSum);
          toast(
            "开始传输 #" +
              id +
              "（" +
              Math.round(buf.length / 1024) +
              " KB），请保持本页在前台…",
            8000,
          );

          report("传输中 0 / " + buf.length + " 字节…");
          let lastPct = -1;
          const { secs, stalls } = await sendBytes(buf, (sent) => {
            const pct = Math.round((sent / buf.length) * 100);
            statusEl.textContent =
              "传输中 " + sent + " / " + buf.length + " 字节…";
            // toast 每个批次（8KB）才刷一次，百分比变了才动，避免频繁重排
            if (pct !== lastPct) {
              lastPct = pct;
              toast(
                "传输中 " +
                  pct +
                  "%（" +
                  sent +
                  " / " +
                  buf.length +
                  " 字节）…",
                8000,
              );
              labelBtn.textContent = "上传 " + pct + "%";
            }
          });

          labelBtn.textContent = "校验中…";
          toast("传输完成，等待设备落盘校验…");

          const ended = waitFor(
            (t) => t.startsWith("end:ok:") || t.startsWith("end:fail:"),
            5000,
            "end",
          );
          await writeCmd("end");
          const e = await ended;
          if (!e.startsWith("end:ok:")) {
            const msg = e.slice(9);
            report("✗ 落盘失败：" + msg, "bad");
            toast("✗ 落盘失败：" + msg, 5000, "bad");
            await refreshList();
            return;
          }

          // begin 时设备已清零统计，所以 stats 反映的正是本次上传
          const sp = waitFor((t) => t.startsWith("stats:"), 3000, "stats");
          await writeCmd("stats");
          const f = (await sp).split(":");
          const devBytes = parseInt(f[1], 10),
            devSum = f[3];

          const bytesOk = devBytes === buf.length;
          const sumOk = devSum === localSum;
          log(
            "设备收到 " +
              devBytes +
              " / " +
              buf.length +
              " 字节 → " +
              (bytesOk
                ? "✓ 无丢包"
                : "✗ 丢包 " + (buf.length - devBytes) + " 字节"),
            bytesOk ? "ok" : "bad",
          );
          log(
            "耗时 " +
              secs.toFixed(2) +
              "s，吞吐 " +
              (buf.length / secs / 1024).toFixed(1) +
              " KB/s，流控等待 " +
              stalls +
              " 次",
          );
          if (!bytesOk || !sumOk) {
            report(
              "校验失败 设备=" +
                devSum +
                " 本地=" +
                localSum +
                "，数据错乱，请重试",
              "bad",
            );
            toast("✗ 校验失败：数据不一致，请重新上传", 5000, "bad");
            await refreshList();
            return;
          }
          report(
            "✓ 上传成功（" +
              secs.toFixed(1) +
              "s，" +
              (buf.length / secs / 1024).toFixed(1) +
              " KB/s），屏幕刷新中…",
            "ok",
          );
          toast("✓ 上传成功，数据校验一致", 2500, "ok");

          await refreshList();
          labelBtn.textContent = "刷新中…";
          await displayImage(id);
        } catch (e) {
          // 传输异常时主动中止，避免固件 uploading 标志一直挂起
          try {
            await writeCmd("abort");
          } catch (_) {}
          throw e;
        } finally {
          labelBtn.textContent = origLabel;
        }
      }

      // 裸字节链路自检：不写 flash，只重置统计后灌 105984 字节再看 stats。
      // 拿来区分"链路有问题"还是"图库/落盘有问题"。
      async function selfTest() {
        const buf = new Uint8Array(IMG_BYTES);
        for (let i = 0; i < IMG_BYTES; i++) buf[i] = i & 0xff;
        const localSum = adler32(buf);

        const rst = waitFor((t) => t === "reset:ok", 3000, "reset:ok");
        await writeCmd("reset");
        await rst;

        // BLE 的数据特征值随时可收裸字节；串口要先让设备进入"只收数据"模式
        if (link === "serial") {
          const raw = waitFor(
            (t) => t === "raw:ok" || t.startsWith("err:"),
            3000,
            "raw",
          );
          await writeCmd("raw:" + IMG_BYTES);
          const r = await raw;
          if (r.startsWith("err:")) throw new Error("自检被拒：" + r.slice(4));
        }

        const { secs, stalls } = await sendBytes(buf);

        const sp = waitFor((t) => t.startsWith("stats:"), 3000, "stats");
        await writeCmd("stats");
        const f = (await sp).split(":");
        const devBytes = parseInt(f[1], 10),
          devSum = f[3];

        log(
          "自检：设备收到 " +
            devBytes +
            " / " +
            IMG_BYTES +
            "，校验和 " +
            (devSum === localSum
              ? "一致 ✓"
              : "不一致 ✗（设备=" + devSum + " 本地=" + localSum + "）"),
          devBytes === IMG_BYTES && devSum === localSum ? "ok" : "bad",
        );
        log(
          "耗时 " +
            secs.toFixed(2) +
            "s，吞吐 " +
            (IMG_BYTES / secs / 1024).toFixed(1) +
            " KB/s，流控等待 " +
            stalls +
            " 次",
        );
      }

      // ── 按钮 ────────────────────────────────────────────────────
      async function guarded(fn) {
        if (busy) return;
        busy = true;
        setConnected(connected);
        try {
          await fn();
        } catch (e) {
          report("✗ " + e.message, "bad");
          toast("✗ " + e.message, 5000, "bad");
        } finally {
          busy = false;
          setConnected(connected);
        }
      }

      $("btnConnect").addEventListener("click", () => guarded(connect));
      $("btnSerial").addEventListener("click", () => guarded(connectSerial));
      $("btnDisconnect").addEventListener("click", () => {
        if (link === "serial") {
          guarded(async () => {
            teardown("已主动断开");
            link = null;
            await closeSerial(false);
            setConnected(false);
          });
        } else disconnect();
      });
      $("btnInfo").addEventListener("click", () => guarded(queryInfo));
      $("btnList").addEventListener("click", () => guarded(refreshList));
      $("btnClear").addEventListener("click", () => guarded(clearScreen));
      $("btnUpload").addEventListener("click", () => {
        if (!packed) return;
        guarded(() => uploadImage(safeName($("iname").value, "image"), packed));
      });
      $("btnTestImg").addEventListener("click", () =>
        guarded(() =>
          uploadImage(
            safeName($("iname").value, "test"),
            makeTestImage(),
            $("btnTestImg"),
          ),
        ),
      );
      $("btnSelfTest").addEventListener("click", () => guarded(selfTest));
      calInit();

      const PAGE_VER = "2026-11-02a"; // 每次改本页都改这里：用来确认浏览器加载的不是缓存旧版
      log(
        "页面就绪 v" +
          PAGE_VER +
          "。蓝牙连接（手机/电脑）或串口连接（电脑 Chrome/Edge）→ 上传页选图转四色并显示，日历页生成并下发日历，图库页管理已传图片，调试页看日志。",
      );
      log(
        "本地试跑：在本目录执行 python -m http.server 8000，然后开 http://localhost:8000/ble_test.html",
      );
      log(
        "（http://localhost 也算安全上下文，Web Bluetooth / Web Serial 都能直接用）",
        "dim",
      );
      unread = 0;
      updateBadge(); // 启动提示不算未读，徽标只统计连接后的重要事件
