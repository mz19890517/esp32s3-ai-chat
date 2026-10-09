#pragma once
#include <Arduino.h>

struct TouchPoint
{
  int x, y;
  bool pressed;
};

void touchInit();
bool touchRead(TouchPoint &tp);
