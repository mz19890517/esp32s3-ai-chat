#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

namespace bms {

struct BaseInfo
{
  float totalVoltage = 0;
  float current = 0;
  float remainPower = 0;
  float nominalPower = 0;
  int cycles = 0;
  int soc = -1;
  int cellCount = 0;
  int ntcCount = 0;
  float temperatures[4] = {0, 0, 0, 0};
  float balanceCurrent = 0;
  bool extended = false;
};

// 扫描列表条目：周围所有 BLE 广播（不止 BMS），用户可见"到底搜到了什么"
struct ScanItem
{
  NimBLEAddress addr;
  String name;      // 广播名，可能为空（显示为 MAC）
  int rssi = -100;
  bool hasFF00 = false;   // 广播里带 BMS 服务 UUID（大概率是电池）
};

bool begin();
void poll();
bool connected();
bool hasData();
BaseInfo info();

// fix14：扫描列表 + 手动点选连接。列表在 BLE 回调线程更新，取回的是快照副本
int scanCount();
ScanItem scanAt(int i);
void rescan();                          // 清空列表重新扫
bool connectAddr(const NimBLEAddress &a);   // 请求连接指定设备（异步，poll 里执行）
bool connecting();                      // 连接任务进行中（UI 显示"正在连接"并暂停扫描列表）

}
