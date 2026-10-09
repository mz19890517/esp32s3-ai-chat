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

bool begin();
void poll();
bool connected();
bool hasData();
BaseInfo info();

}
