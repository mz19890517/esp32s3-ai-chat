#include "ui.h"
#include "board_config.h"
#include <Wire.h>
#include <esp_task_wdt.h>

#if __has_include(<font/u8g2_font_unifont_h_chinese4.h>)
#define AI_FONT u8g2_font_unifont_h_chinese4
#else
#define AI_FONT u8g2_font_unifont_t_chinese4
#endif

const uint16_t COL_BG       = 0x1082;
const uint16_t COL_PANEL    = 0x18E4;
const uint16_t COL_PANEL_HI = 0x31A8;
const uint16_t COL_ACCENT   = 0x0C1F;
const uint16_t COL_ACCENT_DK= 0x0B39;
const uint16_t COL_TEXT     = 0xEF3E;
const uint16_t COL_SUB      = 0x94B4;
const uint16_t COL_LINE     = 0x39E9;
const uint16_t COL_KEY      = 0x2966;
const uint16_t COL_KEY_FN   = 0x420A;
const uint16_t COL_ERR_BG   = 0x6105;
const uint16_t COL_ERR_TX   = 0xFD55;
const uint16_t COL_OK       = 0x362F;

Arduino_DataBus *lcdBus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
static Arduino_AXS15231B lcdPanel(lcdBus, -1, 0, false, SCREEN_W, SCREEN_H);
Arduino_Canvas *gfx = nullptr;

static void tcaWr(uint8_t reg, uint8_t v)
{
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(reg);
  Wire.write(v);
  Wire.endTransmission();
}

static uint8_t tcaRd(uint8_t reg)
{
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)TCA_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

static void wdt_feed_cb(TimerHandle_t xTimer)
{
  (void)xTimer;
  esp_task_wdt_reset();
}

void ui_init()
{
  Serial0.begin(115200);
  Serial.begin(115200);
  delay(300);
  BOOTLOG("\n[boot] ui_init start\n");
  BOOTLOG("[boot] PSRAM size=%u\n", (unsigned)ESP.getPsramSize());
  BOOTLOG("[boot] chip rev=%d cores=%d\n", (int)ESP.getChipRevision(), (int)ESP.getChipCores());
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  BOOTLOG("[boot] backlight ON\n");
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(50);
  logTeePrint("[boot] I2C scan:");
  for (uint8_t a = 1; a < 127; a++)
  {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) logTeePrintf(" 0x%02X", a);
  }
  logTeePrintf("\n");
  uint8_t cfg = tcaRd(TCA_REG_CFG) & ~TCA_BIT_RST;
  tcaWr(TCA_REG_CFG, cfg);
  uint8_t out = tcaRd(TCA_REG_OUT);
  BOOTLOG("[boot] TCA cfg=0x%02X out=0x%02X\n", cfg, out);
  tcaWr(TCA_REG_OUT, out | TCA_BIT_RST);
  delay(10);
  tcaWr(TCA_REG_OUT, out & ~TCA_BIT_RST);
  delay(10);
  tcaWr(TCA_REG_OUT, out | TCA_BIT_RST);
  delay(200);

  gfx = new Arduino_Canvas(SCREEN_W, SCREEN_H, &lcdPanel, 0, 0, 0);
  bool bok = gfx->begin();
  BOOTLOG("[boot] gfx begin=%d psram=%u heap=%u\n", (int)bok, (unsigned)ESP.getFreePsram(),
          (unsigned)ESP.getFreeHeap());
  if (!bok)
  {
    BOOTLOG("[boot] WARN: gfx begin failed, continuing without display\n");
  }
  gfx->fillScreen(COL_BG);
  gfx->setTextSize(1);
  gfx->setFont(AI_FONT);
  gfx->setUTF8Print(true);

  gfx->flush();
  BOOTLOG("[boot] ui_init done\n");
}

int textWidth(const char *s)
{
  if (!s || !*s) return 0;
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

int textWidth(const String &s)
{
  return s.length() ? textWidth(s.c_str()) : 0;
}

int utf8Count(const String &s)
{
  int n = 0;
  for (unsigned i = 0; i < s.length(); i++)
    if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
  return n;
}

int prevUtf8(const String &s, int i)
{
  if (i <= 0) return 0;
  int j = i - 1;
  while (j > 0 && ((uint8_t)s[j] & 0xC0) == 0x80) j--;
  return j;
}

void sanitizeText(String &s)
{
  s.replace("\r", "");
  s.replace("\xc2\xa0", " ");
  s.replace("…", "...");
  s.replace("—", "--");
  s.replace("·", "-");
  s.replace("×", "x");
  s.replace("→", "->");
  s.replace("←", "<-");
  s.replace("•", "-");
  s.trim();
}

String fitTail(const String &s, int maxW)
{
  if (!s.length()) return s;
  if (maxW <= 0) return String();
  if (textWidth(s) <= maxW) return s;
  int budget = maxW - textWidth("..");
  if (budget <= 0) return String("..");
  // 从尾部逐字累加宽度（u8g2 字体字宽可加，无字距），避免每字符都重测整串
  int i = (int)s.length();
  int start = i;
  while (i > 0)
  {
    int j = prevUtf8(s, i);
    int w = textWidth(s.substring(j, i));
    if (w > budget) break;
    budget -= w;
    start = j;
    i = j;
  }
  return ".." + s.substring(start);
}

void drawScrollbar(int x, int top, int viewH, int contentH, int scroll)
{
  if (contentH <= viewH || viewH <= 0 || contentH <= 0) return;
  int maxScroll = contentH - viewH;
  if (maxScroll <= 0) return;
  if (scroll < 0) scroll = 0;
  if (scroll > maxScroll) scroll = maxScroll;
  int th = (int)((long)viewH * viewH / contentH);
  if (th < 18) th = 18;
  int ty = top + (int)((long)(viewH - th) * scroll / maxScroll);
  gfx->fillRect(x, top, 3, viewH, COL_PANEL);
  gfx->fillRect(x, ty, 3, th, COL_SUB);
}

bool ptIn(int tx, int ty, int x, int y, int w, int h)
{
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

int centerBaseY(int y, int h)
{
  return y + (h - 16) / 2 + FONT_ASC;
}

void drawTextAt(int x, int baseY, const char *s, uint16_t fg, uint16_t bg)
{
  gfx->setCursor(x, baseY);
  gfx->setTextColor(fg, bg);
  gfx->print(s);
}

void drawTextAt(int x, int baseY, const String &s, uint16_t fg, uint16_t bg)
{
  drawTextAt(x, baseY, s.c_str(), fg, bg);
}

void drawBtn(int x, int y, int w, int h, const char *label, uint16_t bg, uint16_t fg, uint16_t border)
{
  gfx->fillRoundRect(x, y, w, h, 8, bg);
  if (border) gfx->drawRoundRect(x, y, w, h, 8, border);
  int tw = textWidth(label);
  drawTextAt(x + (w - tw) / 2, centerBaseY(y, h), label, fg, bg);
}

void bootSplash(const String &msg)
{
  if (!gfx) return;
  gfx->fillScreen(COL_BG);
  gfx->fillCircle(SCREEN_W / 2, 150, 46, COL_ACCENT);
  gfx->setTextSize(2, 2);
  gfx->setCursor(SCREEN_W / 2 - 30, 160);
  gfx->setTextColor(0xFFFF, COL_ACCENT);
  gfx->print("AI");
  gfx->setTextSize(1);
  const char *name = "触摸 AI 助手";
  int tw = textWidth(name);
  drawTextAt((SCREEN_W - tw) / 2, 240, name, COL_TEXT, COL_BG);
  tw = textWidth(msg);
  drawTextAt((SCREEN_W - tw) / 2, 300, msg.c_str(), COL_SUB, COL_BG);
  gfx->flush();
}
