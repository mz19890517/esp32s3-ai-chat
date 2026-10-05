#include "frame_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

std::vector<FrameRec> frames;
bool flHexMode = false;

static SemaphoreHandle_t flMtx;

static String flTimeOf(uint32_t ms)
{
  uint32_t s = ms / 1000;
  uint32_t h = s / 3600;
  s %= 3600;
  uint32_t m = s / 60;
  s %= 60;
  char b[16];
  if (h) snprintf(b, sizeof(b), "%02u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
  else snprintf(b, sizeof(b), "%02u:%02u", (unsigned)m, (unsigned)s);
  return String(b);
}

String flTime(uint32_t ms) { return flTimeOf(ms); }

String flHexOf(const uint8_t *d, int n)
{
  String s;
  if (n > 0) s.reserve(n * 3);
  char b[4];
  for (int i = 0; i < n; i++)
  {
    if (i) s += ' ';
    snprintf(b, sizeof(b), "%02X", d[i]);
    s += b;
  }
  return s;
}

String flTextOf(const uint8_t *d, int n)
{
  String s;
  if (n > 0) s.reserve(n);
  for (int i = 0; i < n; i++)
  {
    uint8_t c = d[i];
    if (c == '\r' || c == '\n' || c == '\t') s += ' ';
    else if (c >= 0x20 && c < 0x7F) s += (char)c;
    else if (c >= 0xC2 && c < 0xE0 && i + 1 < n && d[i + 1] >= 0x80 && d[i + 1] < 0xC0)
    {
      s += (char)c;
      i++;
    }
    else s += '.';
  }
  s.trim();
  if (s.length() > FL_TEXT_MAX) s = s.substring(0, FL_TEXT_MAX);
  return s;
}

void flBegin()
{
  frames.clear();
  frames.reserve(FL_MAX_FRAMES);
  flMtx = xSemaphoreCreateMutex();
}

static void flPush(bool rx, int len, const String &hex, const String &text, uint32_t ms)
{
  if (flMtx) xSemaphoreTake(flMtx, portMAX_DELAY);
  FrameRec r;
  r.rx = rx;
  r.ms = ms;
  r.len = len;
  r.hex = hex;
  r.text = text;
  frames.push_back(r);
  while ((int)frames.size() > FL_MAX_FRAMES) frames.erase(frames.begin());
  if (flMtx) xSemaphoreGive(flMtx);
}

void flAddRx(const uint8_t *d, int n)
{
  if (n <= 0) return;
  flPush(true, n, flHexOf(d, n), flTextOf(d, n), millis());
}

void flAddRxText(const String &s)
{
  if (!s.length()) return;
  int n = s.length();
  flPush(true, n, flHexOf((const uint8_t *)s.c_str(), n), s, millis());
}

void flAddTx(const String &s)
{
  if (!s.length()) return;
  int n = s.length();
  flPush(false, n, flHexOf((const uint8_t *)s.c_str(), n), flTextOf((const uint8_t *)s.c_str(), n), millis());
}

void flAddTxRaw(const uint8_t *d, int n)
{
  if (n <= 0) return;
  flPush(false, n, flHexOf(d, n), flTextOf(d, n), millis());
}

void flClear()
{
  if (flMtx) xSemaphoreTake(flMtx, portMAX_DELAY);
  frames.clear();
  if (flMtx) xSemaphoreGive(flMtx);
}

void flToggleHex()
{
  flHexMode = !flHexMode;
}