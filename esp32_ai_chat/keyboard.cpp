#include "keyboard.h"
#include "board_config.h"
#include "ui.h"

static const int ROW_H = 38;
static const int ROW_STEP = 42;

enum KeyId : uint8_t
{
  K_CHAR,
  K_SHIFT,
  K_MODE,
  K_BACK,
  K_SPACE,
  K_SEND,
};

struct KeyRect
{
  int x, y, w, h;
  KeyId id;
  const char *label;
  const char *labelUp;
};

static String *target = nullptr;
static bool mask = false;
static char sendLabel[8] = "发送";
static bool symMode = false;
static bool upper = false;
static int pressedKey = -1;
static uint32_t pressT = 0, lastRep = 0;

static KeyRect keys[40];
static int keyCount = 0;

void kbAttach(String *buf) { target = buf; }
void kbSetMask(bool m) { mask = m; }
void kbSetSendLabel(const char *l) { strncpy(sendLabel, l, sizeof(sendLabel) - 1); }

String kbTake()
{
  String s = target ? *target : String();
  if (target) target->clear();
  return s;
}

String kbPeek()
{
  return target ? *target : String();
}

void kbReset()
{
  symMode = false;
  upper = false;
  pressedKey = -1;
  if (target) target->clear();
}

static void addKey(int x, int y, int w, int h, KeyId id, const char *label, const char *labelUp = "")
{
  keys[keyCount] = {x, y, w, h, id, label, labelUp};
  keyCount++;
}

static void buildLayout()
{
  keyCount = 0;
  const int r[4] = {KB_TOP + 4, KB_TOP + 4 + ROW_STEP, KB_TOP + 4 + 2 * ROW_STEP, KB_TOP + 4 + 3 * ROW_STEP};
  static const char *r1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
  static const char *r2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
  static const char *r3[] = {"z", "x", "c", "v", "b", "n", "m"};
  static const char *s1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
  static const char *s2[] = {"@", "#", "$", "%", "&", "*", "-", "+", "(", ")"};
  static const char *s3[] = {"?", "!", ":", "=", "\"", "_", "/"};

  for (int i = 0; i < 10; i++)
    addKey(6 + i * 31, r[0], 29, ROW_H, K_CHAR, symMode ? s1[i] : r1[i]);
  for (int i = 0; i < 9; i++)
    addKey(21 + i * 31, r[1], 29, ROW_H, K_CHAR, symMode ? s2[i] : r2[i]);

  if (symMode)
  {
    for (int i = 0; i < 7; i++)
      addKey(52 + i * 31, r[2], 29, ROW_H, K_CHAR, s3[i]);
  }
  else
  {
    addKey(10, r[2], 40, ROW_H, K_SHIFT, "Aa");
    for (int i = 0; i < 7; i++)
      addKey(52 + i * 31, r[2], 29, ROW_H, K_CHAR, r3[i], "");
  }
  addKey(269, r[2], 41, ROW_H, K_BACK, "\xe5\x88\xa0");

  addKey(3, r[3], 46, ROW_H, K_MODE, symMode ? "ABC" : "123");
  addKey(51, r[3], 29, ROW_H, K_CHAR, ",");
  addKey(82, r[3], 144, ROW_H, K_SPACE, "\xe7\xa9\xba\xe6\xa0\xbc");
  addKey(228, r[3], 29, ROW_H, K_CHAR, ".");
  addKey(259, r[3], 58, ROW_H, K_SEND, sendLabel);
}

static void drawKey(int i, bool hl)
{
  KeyRect &k = keys[i];
  uint16_t bg = COL_KEY;
  if (hl) bg = COL_ACCENT;
  else if (k.id == K_SHIFT && upper) bg = COL_ACCENT_DK;
  else if (k.id != K_CHAR) bg = COL_KEY_FN;
  uint16_t fg = hl ? 0xFFFF : COL_TEXT;
  gfx->fillRoundRect(k.x, k.y, k.w, k.h, 6, bg);
  const char *lab = k.label;
  char up[2] = {0, 0};
  if (!symMode && upper && k.id == K_CHAR && k.label[0] >= 'a' && k.label[0] <= 'z')
  {
    up[0] = k.label[0] - 32;
    lab = up;
  }
  int tw = textWidth(lab);
  drawTextAt(k.x + (k.w - tw) / 2, centerBaseY(k.y, k.h), lab, fg, bg);
}

void kbDraw()
{
  buildLayout();
  gfx->fillRect(0, KB_TOP - 2, SCREEN_W, SCREEN_H - KB_TOP + 2, COL_BG);
  gfx->drawFastHLine(0, KB_TOP - 2, SCREEN_W, COL_LINE);
  for (int i = 0; i < keyCount; i++) drawKey(i, false);
}

void kbDrawInputLine()
{
  gfx->fillRect(0, INPUT_Y, SCREEN_W, INPUT_H, COL_PANEL);
  gfx->drawFastHLine(0, INPUT_Y, SCREEN_W, COL_LINE);
  if (!target) return;
  String show;
  if (mask)
  {
    int n = utf8Count(*target);
    for (int i = 0; i < n; i++) show += '*';
  }
  else show = *target;
  show += ((millis() / 500) & 1) ? "_" : "";
  show = fitTail(show, SCREEN_W - 16);
  drawTextAt(8, centerBaseY(INPUT_Y, INPUT_H), show, COL_TEXT, COL_PANEL);
}

static void backspace()
{
  if (!target || !target->length()) return;
  int i = prevUtf8(*target, target->length());
  target->remove(i);
}

uint8_t kbHandle(bool down, int x, int y)
{
  uint8_t ev = 0;
  if (!down)
  {
    if (pressedKey >= 0)
    {
      drawKey(pressedKey, false);
      pressedKey = -1;
    }
    return 0;
  }
  if (y < KB_TOP) return 0;
  buildLayout();
  int hitIdx = -1;
  for (int i = 0; i < keyCount; i++)
    if (ptIn(x, y, keys[i].x, keys[i].y, keys[i].w, keys[i].h)) { hitIdx = i; break; }
  if (hitIdx < 0) return 0;

  if (pressedKey < 0)
  {
    pressedKey = hitIdx;
    pressT = lastRep = millis();
    drawKey(hitIdx, true);
    KeyRect &k = keys[hitIdx];
    switch (k.id)
    {
    case K_CHAR:
      if (target)
      {
        if (!symMode && upper && k.label[0] >= 'a' && k.label[0] <= 'z')
          *target += (char)(k.label[0] - 32);
        else
          *target += k.label;
        ev |= KB_CHANGED;
      }
      break;
    case K_SHIFT:
      upper = !upper;
      kbDraw();
      break;
    case K_MODE:
      symMode = !symMode;
      upper = false;
      kbDraw();
      break;
    case K_BACK:
      backspace();
      ev |= KB_CHANGED;
      break;
    case K_SPACE:
      if (target) { *target += ' '; ev |= KB_CHANGED; }
      break;
    case K_SEND:
      pressedKey = -1;
      drawKey(hitIdx, false);
      ev |= KB_SEND;
      break;
    }
  }
  else if (hitIdx == pressedKey && keys[hitIdx].id == K_BACK)
  {
    uint32_t now = millis();
    if (now - pressT > 350 && now - lastRep > 130)
    {
      lastRep = now;
      backspace();
      ev |= KB_CHANGED;
    }
  }
  return ev;
}
