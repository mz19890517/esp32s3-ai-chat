#include "touch_driver.h"
#include "board_config.h"
#include <Wire.h>

bool touchRead(TouchPoint &tp)
{
  tp.pressed = false;
  uint8_t cmd[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00};
  uint8_t data[14] = {0};

  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(cmd, 11);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)14) != 14) return false;
  Wire.readBytes(data, 14);

  if (data[1] == 0 || data[2] == 0 || data[3] < 2 || data[5] < 2) return false;
  if (data[0] == 0xFF || data[1] > 2) return false;

  tp.x = ((data[2] & 0x0F) << 8) | data[3];
  tp.y = ((data[4] & 0x0F) << 8) | data[5];
  if (tp.x >= SCREEN_W) tp.x = SCREEN_W - 1;
  if (tp.y >= SCREEN_H) tp.y = SCREEN_H - 1;
  tp.pressed = true;
  return true;
}

void touchInit()
{
}
