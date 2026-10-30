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
