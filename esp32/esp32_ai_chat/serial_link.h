#pragma once
#include <Arduino.h>

enum LinkMode : uint8_t { LINK_BLE, LINK_TCP };

#define SL_TCP_PORT 8888

void slBegin(const char *devName);
void slStartAdv();   // 所有 GATT 服务注册完之后调用，开始广播
void slPoll();
bool slActive();
LinkMode slMode();
void slSetMode(LinkMode m);
void slToggleMode();
String slStatus();
bool slBleConnected();
String slBleAddr();
bool slBleAdvActive();
const char *slDeviceName();
bool slSend(const String &s);
String slLocalIp();
void slOtaSuspend();
void slOtaResume();
bool slBleSuspend();   // TLS 前临时释放 BLE 内存（网页蓝牙会断开）
void slBleResume();    // 重建 BLE 栈；之后需再调 otaBleBegin() + slStartAdv()
bool slBleDown();      // BLE 栈当前是否处于挂起状态