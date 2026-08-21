#pragma once
#include <Arduino.h>

#define PIN_LCD_CS   12
#define PIN_LCD_CLK  5
#define PIN_LCD_D0   1
#define PIN_LCD_D1   2
#define PIN_LCD_D2   3
#define PIN_LCD_D3   4
#define PIN_LCD_BL   6
#define PIN_I2C_SDA  21
#define PIN_I2C_SCL  22

#define TCA_ADDR     0x20
#define TCA_REG_OUT  0x02
#define TCA_REG_CFG  0x06
#define TCA_BIT_RST  0x02

#define TOUCH_ADDR   0x3B

#define SCREEN_W     320
#define SCREEN_H     480
