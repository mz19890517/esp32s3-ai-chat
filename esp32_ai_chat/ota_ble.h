#pragma once
#include <Arduino.h>

// 蓝牙推送升级：设备做 BLE 外设，网页（tools/ota_web/index.html）通过 Web Bluetooth
// 把 firmware.bin 推给设备。协议见 ota_ble.cpp 顶部注释。

#define OTA_BLE_SVC  "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#define OTA_BLE_CTRL "6e400005-b5a3-f393-e0a9-e50e24dcca9e"
#define OTA_BLE_DATA "6e400006-b5a3-f393-e0a9-e50e24dcca9e"
#define OTA_BLE_STAT "6e400007-b5a3-f393-e0a9-e50e24dcca9e"

void otaBleBegin();              // 必须在 slBegin() 之后调用
void otaBlePoll();               // 主循环调用
void otaBleCancel();             // 中止当前蓝牙接收
String otaBleStatus();           // 一行状态文本
bool otaBleRunning();
