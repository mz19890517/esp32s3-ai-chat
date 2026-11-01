#pragma once
#include <Arduino.h>

// 日志双写：所有日志走这里 = 真实串口 + 32KB 内存环形缓冲。
// 网页通过 /api/logs 读取缓冲（ota_http.cpp），不用接 USB 也能排障。
// 开关：cfg_table 的 "log.http"（默认开），关闭时 /api/logs 返回 404，本地缓冲照常记录。

void logTeeBegin();                       // setup 里尽早调用
void logTeePrintf(const char *fmt, ...);  // printf 风格（替代 Serial.printf）
void logTeePrint(const String &s);        // 整段写入（内部转 write）
void logTeeClear();
String logTeeDump(size_t maxBytes = 24000); // 取最近的日志（老→新）

// —— 飞行记录仪：把日志尾部快照写入 NVS，崩溃重启后可读回 ——
// 用法：请求发起/进入高危阶段前 Save；请求正常结束后 Clear。
// 重启后 Load 到非空快照 = 上次请求中途崩溃，Load 内部读完即清，不会重复打印。
void logTeePersistSave();
String logTeePersistLoad(bool *valid = nullptr);
void logTeePersistClear();
// 快照轮转：开机时把「上次崩溃」挪到「再上次崩溃」槽位，连崩两次也不丢现场
String logTeePersistLoadKey(const char *key, bool *valid = nullptr);
void logTeePersistSaveKey(const char *key, const String &s);
