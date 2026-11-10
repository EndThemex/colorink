# ColorInk — 四色墨水屏相框固件

基于 ESP32-C3 的局域网/蓝牙电子相框。面板为华为 3.98" 手机壳屏（768×552，JD79665 控制器），仅支持**黑 / 白 / 黄 / 红**四种颜色（2bpp 帧缓冲）。浏览器端完成图片的四色量化，设备只接收打包好的 2bpp 帧缓冲并刷屏。

## 硬件

- 主控：ESP32-C3 SuperMini（唯一支持的开发板 `BOARD_PROFILE_ESP32_C3_HUAWEI`）
- 屏幕：3.98" 768×552 BWRY 墨水屏，JD79665 驱动，软件模拟 SPI
- 引脚定义见 [src/config.h](src/config.h)：DIN=3 / CLK=4 / CS=5 / DC=6 / RST=7 / BUSY=10，电池 ADC=0，按键=2，LED=1

## 固件形态（编译期二选一）

ESP32-C3 单射频，WiFi 与 BLE 不能共存，因此拆成两套互斥固件（见 [platformio.ini](platformio.ini)）：

| 环境 | 通道 | 说明 |
|------|------|------|
| `epd_398_jd79665_c3_promini` / `_std` | WiFi | STA 模式 + 内置网页，mDNS `http://colorink.local`；无 WiFi 时长按按键进入配网热点 `ColorInk-XXXX` |
| `epd_398_jd79665_c3_promini_ble` / `_std_ble` | BLE | NimBLE GATT，完全不编译 WiFi；上位机为托管在 HTTPS 的静态页（Web Bluetooth 直连）；空闲 5 分钟自动深睡，按键唤醒 |

## 功能

- **图库**：图片以裸 2bpp 帧缓冲存入 LittleFS（`/img/<id>_<name>.raw`），支持列表 / 显示 / 删除 / 清屏，当前显示 id 持久化在 NVS
- **上传通道**：WiFi 固件走 HTTP 分片上传；BLE 固件走 GATT（CMD/DATA/STATUS 三特征值 + ack 流控 + Adler-32 校验）
- **网页串口（Web Serial）**：浏览器直连 USB 串口，`CI:` 前缀 ASCII 行命令，与 BLE 同一套协议
- **按键**：短按切下一张图，长按 2s 进入配网热点
- **电池**：ADC 采样（去极值取均值）

## 四色量化规则（浏览器端执行）

中性灰像素（色度差 < 40）仅按亮度分配黑/白；只有真正带彩色的像素才参与红/黄判定。字体抗锯齿边缘属于中性灰，不做该区分会被错误判定为红/黄。预览工具 [test.html](test.html) 使用与真机完全相同的量化规则。

## 目录结构

```
src/                固件源码
  main.cpp          启动流程、主循环、帧缓冲
  config.h          引脚与常量配置
  epd_driver.*      JD79665 屏驱动（2bpp 刷屏，约 15s，失败重试 3 次）
  gallery.*         LittleFS 图库
  storage.*         NVS（WiFi 凭证多网络保存）
  webapp.*          WiFi 版：STA 连接 + mDNS + HTTP API
  portal.*          WiFi 版：配网热点门户
  ble_service.*     BLE 版：GATT 服务与协议
  serial_service.*  Web Serial 通道
web/                BLE 上位机静态页（ble_test.html，需 HTTPS 托管）
data/               固件内嵌网页资源
test.html           四色量化预览/测试工具（浏览器本地打开）
merge_firmware.py   构建后合并 bootloader+分区表+应用 为 firmware_merged.bin
```

## 构建与烧录

```bash
# 安装 PlatformIO 后

# 编译（默认环境 epd_398_jd79665_c3_promini_ble）
pio run

# 烧录
pio run -t upload

# 上传文件系统（WiFi 版内嵌网页）
pio run -t uploadfs

# 生成合并固件（.pio/build/<env>/firmware_merged.bin，构建后自动执行）
pio run -t mergebin   # 由 extra_scripts=post:merge_firmware.py 触发
```

环境切换：`pio run -e epd_398_jd79665_c3_std`，或修改 `platformio.ini` 的 `default_envs`。

## 使用方法

### WiFi 固件
1. 首次使用长按按键 2s 进入配网热点 `ColorInk-XXXX`，连接后按门户提示配置 WiFi（最多保存 5 个网络，开机按序尝试）
2. 联网后访问 `http://colorink.local`，在网页上传图片（浏览器自动完成四色量化并打包 2bpp）、查看/删除/显示/清屏
3. 刷屏期间页面持续可用（约 15s，等待期间仍响应请求）

### BLE 固件
1. 用 HTTPS 托管 `web/ble_test.html`（设备自带页面是 http 非安全上下文，`navigator.bluetooth` 不可用）
2. Web Bluetooth 连接设备 `ColorInk-XXXX`，上传/管理图片
3. BLE 未连接、无按键、无串口流量持续 5 分钟后自动深睡，按键唤醒（即重启）

### Web Serial（两种固件均可用）
浏览器通过 Web Serial 直连设备 USB 串口，使用 `CI:` 行命令（`CI:ping`、`CI:list` 等）上传与管理图片。
