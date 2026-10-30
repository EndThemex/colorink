#ifndef INKSIGHT_SERIAL_SERVICE_H
#define INKSIGHT_SERIAL_SERVICE_H

// ── 网页串口（Web Serial）通道 ───────────────────────────────
// 浏览器直接打开设备的 USB 串口（原生 USB CDC 或 USB-UART 桥），用和 BLE
// 通道同一套 ASCII 行命令上传图片、显示/删除/清屏。
//
// 命令都用 "CI:" 前缀 + 换行，例如 "CI:ping\n"；设备回包也从 "CI:" 开始
// 单独一行，所以浏览器按行过滤即可，串口里同时打印的 [BOOT]/[GAL] 日志不会
// 干扰协议。图片数据在 begin 之后以裸字节直接写串口，设备数满 COLOR_BUF_LEN
// 字节后自动回到命令行模式。
//
// 流控：每收到 ACK_STEP（4096）字节回一条 CI:ack:<累计字节>，浏览器按这个
// 节奏等确认再继续发，避免 USB 缓冲溢出导致图像整体错位。
//
// 初始化串口（替代 setup() 里的 Serial.begin）。必须在 Serial.begin 之前
// 调大接收缓冲，否则网页一次发一批数据就会把 256 字节的默认缓冲冲掉。
void serialServiceBegin(unsigned long baud);

// 主循环里调用：处理命令、接收裸字节并执行排队的刷屏/清屏。
void serialServiceLoop();

#endif // INKSIGHT_SERIAL_SERVICE_H
