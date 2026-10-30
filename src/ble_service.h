#ifndef INKSIGHT_BLE_SERVICE_H
#define INKSIGHT_BLE_SERVICE_H

#ifdef INKSIGHT_BLE

#include <Arduino.h>

// ── BLE 正式协议 ─────────────────────────────────────────────
// 编译期二选一：-DINKSIGHT_BLE 的固件不启动 WiFi，只跑这个 GATT 服务。
// 上位机是托管在 HTTPS 上的静态页（web/），Web Bluetooth 直连。
//
// 服务 UUID 8e7a0001-...，三个特征值：
//   CMD    (8e7a0002, Write)                ASCII 行命令
//   DATA   (8e7a0003, Write Without Resp.)  图片裸字节流，无包头
//   STATUS (8e7a0004, Notify)               回包
//
// 命令 → 回包：
//   ping                    → pong
//   info                    → info:<协商 MTU>:<空闲堆>:<开机毫秒>
//   stats                   → stats:<收到字节>:<包数>:<adler32 8 位十六进制>
//   reset                   → reset:ok                  清零统计与校验和
//   list                    → chunk:<已发>/<总长>:<json 片段>
//                             图库 JSON 有 1KB 级别，装不进一条通知，按 MTU 切片
//                             连发，页面按 <已发>/<总长> 拼装，拼满即完整
//   begin:<name>:<size>     → begin:ok:<id> | err:<原因>
//                             开一个上传；<size> 必须等于 COLOR_BUF_LEN，
//                             尺寸不符立刻拒绝，不用等传完 106KB 才发现
//   end                     → end:ok:<id> | end:fail:<原因>
//   abort                   → abort:ok                  丢弃半截上传并删文件
//   display:<id>            → display:queued:<id>，刷新完成后 → display:done:<id>
//   delete:<id>             → delete:ok:<id> | delete:fail:<id>
//   clear                   → clear:queued，完成后 → clear:done
//   其它                    → err:unknown
//
// 流控：DATA 每累计收到 8192 字节回一条 ack:<累计字节数>。上位机按这个节奏等
// 确认再继续发——writeValueWithoutResponse 是不排队的，猛灌会静默丢包，而丢包
// 表现为图像整体错位，不是"传失败"这种能一眼看出来的现象。
//
// 校验：Adler-32（模 65521，逐字节），网页端有同一份实现，两边必须逐位一致。

// 初始化 NimBLE、注册服务并开始广播。设备名沿用 WiFi 固件的 AP 名规则
// （ColorInk-XXXX，取 WiFi STA MAC 末两字节），方便和 WiFi 固件对照。
// 返回 false 表示 BLE 起不来。
bool bleModeStart();

// 只把排队中的回包发出去。不执行刷屏、不碰 LED，因此可以在 epdWaitBusy 的
// 15 秒刷新等待里安全调用——否则刷新期间页面收不到任何回包，只能干等到超时。
void blePumpReplies();

// 在主循环里调用：发回包 + 执行排队的刷屏/清屏 + 驱动 LED 状态灯。
//
// 刷屏刻意排到这里执行，绝不在 GATT 回调里做：一次刷新要阻塞 ~15 秒，卡住
// NimBLE 主机任务会直接把链路拖死。
//
// LED：慢闪（1Hz）= 正在广播等人连；常亮 = 已连接。这块板子只有一个按键、
// 屏幕上也没有状态栏，LED 是唯一的非串口反馈通道。
void bleModeLoop();

// 强制踢掉当前客户端并重新广播。绑在这颗板子唯一的按键（长按）上：
// 手机端卡死（Bluefy 崩了、连接半死）时，设备端否则没有任何办法释放连接。
void bleForceDisconnect();

// 单击按键：把"显示下一张已存图片"排进队列，真正的刷屏在 bleModeLoop 里执行。
// 不在按钮处理里直接刷屏——一次刷新阻塞 ~15s，会把 loop 的回包发送卡住。
void bleRequestNextImage();

// 当前是否有客户端连接。
bool bleIsConnected();

// 设备是否正忙：上传进行中 / 有排队的刷屏或清屏 / 屏幕正在刷新。
// 自动深睡用它判断"还在被人使用"。
bool bleIsBusy();

#endif // INKSIGHT_BLE
#endif // INKSIGHT_BLE_SERVICE_H