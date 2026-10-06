#pragma once
#ifndef U8G2_FONT_SUPPORT
#define U8G2_FONT_SUPPORT
#endif
#ifndef U8G2_USE_LARGE_FONTS
#define U8G2_USE_LARGE_FONTS
#endif
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>

#define TITLE_H  36
#define CHAT_TOP TITLE_H
#define CHAT_BOT 276
#define INPUT_Y  276
#define INPUT_H  28
#define KB_TOP   306

#define FONT_ASC 13
#define FONT_ADV 17
#define LINE_H   22

extern const uint16_t COL_BG;
extern const uint16_t COL_PANEL;
extern const uint16_t COL_PANEL_HI;
extern const uint16_t COL_ACCENT;
extern const uint16_t COL_ACCENT_DK;
extern const uint16_t COL_TEXT;
extern const uint16_t COL_SUB;
extern const uint16_t COL_LINE;
extern const uint16_t COL_KEY;
extern const uint16_t COL_KEY_FN;
extern const uint16_t COL_ERR_BG;
extern const uint16_t COL_ERR_TX;
extern const uint16_t COL_OK;

extern Arduino_Canvas *gfx;

void ui_init();

#define BOOTLOG(fmt, ...)                \
  do                                     \
  {                                      \
    Serial.printf(fmt, ##__VA_ARGS__);   \
    Serial0.printf(fmt, ##__VA_ARGS__);  \
  } while (0)

int textWidth(const char *s);
int textWidth(const String &s);
int utf8Count(const String &s);
int prevUtf8(const String &s, int i);
void sanitizeText(String &s);
String fitTail(const String &s, int maxW);

bool ptIn(int tx, int ty, int x, int y, int w, int h);
int centerBaseY(int y, int h);
void drawBtn(int x, int y, int w, int h, const char *label, uint16_t bg, uint16_t fg, uint16_t border = 0);
void drawTextAt(int x, int baseY, const char *s, uint16_t fg, uint16_t bg);
void drawTextAt(int x, int baseY, const String &s, uint16_t fg, uint16_t bg);
