#pragma once
#include <Arduino.h>

// TF 卡升级：开机时自动检测卡根目录的固件并升级（也可在设置页手动触发）
// 硬件：ESP32-S3-Touch-LCD-3.5B，SD_MMC 1-bit，CLK=11 CMD=10 D0=9

bool otaSdMount();               // 挂载 TF 卡（带引脚回退），失败返回 false
void otaSdUnmount();
bool otaSdMounted();

bool otaSdScan();                // 挂载并扫描固件，结果见下面各 getter
String otaSdStatus();            // 一行状态文本，供界面显示
String otaSdFileName();
uint32_t otaSdFileSize();
String otaSdFileVersion();

bool otaSdBootCheck();           // 开机检测：找到固件就升级（成功则直接重启，不返回）
bool otaSdStartUpdate();         // 手动开始升级（由主循环推进）
void otaSdPoll();                // 主循环推进一次
void otaSdAbort();

String otaSdFormatSize(uint32_t bytes);
