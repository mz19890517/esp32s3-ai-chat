#include "keyboard.h"
#include "board_config.h"
#include "ui.h"
#include "pinyin_dict.h"
#include <vector>

static const int ROW_H_EN = 38;
static const int ROW_STEP_EN = 42;
static const int BAR_H = 24;
static const int ROW_H_CN = 33;
static const int ROW_STEP_CN = 36;
static const int PAGE_N = 8;

enum KeyId : uint8_t
{
  K_CHAR,
  K_SHIFT,
  K_MODE,
  K_CN,
  K_BACK,
  K_SPACE,
  K_SEND,
};

struct KeyRect
{
  int x, y, w, h;
  KeyId id;
  const char *label;
};

struct CandHit
{
  int x, w, i;
};

static String *target = nullptr;
static bool mask = false;
static char sendLabel[8] = "发送";
static bool symMode = false;
static bool upper = false;
static bool cnMode = true;
static bool hidden = false;
static int pressedKey = -1;
static uint32_t pressT = 0, lastRep = 0;

static String pyBuf;
static std::vector<String> cands;
static int candPage = 0;
static CandHit candHits[PAGE_N];
static int candHitsN = 0;

static KeyRect keys[40];
static int keyCount = 0;
static bool latch = false;

static int inputLineY()
{
  return hidden ? SCREEN_H - INPUT_H : INPUT_Y;
}

bool kbIsHidden()
{
  return hidden;
}

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
  pyBuf.clear();
  cands.clear();
  candPage = 0;
  if (target) target->clear();
}

static void addKey(int x, int y, int w, int h, KeyId id, const char *label)
{
  keys[keyCount] = {x, y, w, h, id, label};
  keyCount++;
}

static bool pyStartsWith(const char *s, const char *pre)
{
  size_t n = strlen(pre);
  return !strncmp(s, pre, n);
}

static void buildCands()
{
  cands.clear();
  candPage = 0;
  candHitsN = 0;
  if (!pyBuf.length()) return;
  const char *py = pyBuf.c_str();
  for (int i = 0; i < PY_WORD_N && (int)cands.size() < PAGE_N * 2; i++)
    if (!strcmp(PY_WORDS[i].py, py))
      cands.push_back(PY_WORDS[i].w);
  for (int i = 0; i < PY_CHAR_N && (int)cands.size() < PAGE_N * 2; i++)
  {
    if (strcmp(PY_CHARS[i].py, py)) continue;
    for (const char *p = PY_CHARS[i].hz; *p && (int)cands.size() < PAGE_N * 2; p += 3)
      cands.push_back(String(p).substring(0, 3));
  }
  for (int i = 0; i < PY_WORD_N && (int)cands.size() < PAGE_N * 2; i++)
    if (strlen(PY_WORDS[i].py) > pyBuf.length() && pyStartsWith(PY_WORDS[i].py, py))
      cands.push_back(PY_WORDS[i].w);
  for (int i = 0; i < PY_CHAR_N && (int)cands.size() < PAGE_N * 2; i++)
  {
    if (strlen(PY_CHARS[i].py) <= pyBuf.length() || !pyStartsWith(PY_CHARS[i].py, py)) continue;
    if (*PY_CHARS[i].hz)
      cands.push_back(String(PY_CHARS[i].hz).substring(0, 3));
  }
}

static void commitCand(int gi)
{
  if (gi < 0 || gi >= (int)cands.size()) return;
  if (target) *target += cands[gi];
  pyBuf.clear();
  cands.clear();
  candPage = 0;
}

static void drawBar()
{
  gfx->fillRect(0, KB_TOP, SCREEN_W, BAR_H, COL_PANEL);
  gfx->drawFastHLine(0, KB_TOP + BAR_H - 1, SCREEN_W, COL_LINE);
  candHitsN = 0;
  int x = 4;
  if (pyBuf.length())
  {
    String tag = "[" + pyBuf + "]";
    drawTextAt(x, centerBaseY(KB_TOP, BAR_H), tag, COL_ACCENT, COL_PANEL);
    x += textWidth(tag) + 6;
  }
  else
  {
    drawTextAt(4, centerBaseY(KB_TOP, BAR_H), "中文拼音", COL_SUB, COL_PANEL);
  }
  if (!cands.empty())
  {
    int start = candPage * PAGE_N;
    int end = min((int)cands.size(), start + PAGE_N);
    for (int i = start; i < end; i++)
    {
      int tw = textWidth(cands[i]);
      if (x + tw > SCREEN_W - 58) break;
      bool hl = (i == start);
      uint16_t bg = hl ? COL_ACCENT_DK : COL_PANEL;
      if (hl) gfx->fillRoundRect(x - 2, KB_TOP + 2, tw + 4, BAR_H - 4, 4, bg);
      drawTextAt(x, centerBaseY(KB_TOP, BAR_H), cands[i], hl ? 0xFFFF : COL_TEXT, bg);
      candHits[candHitsN++] = {x, tw, i};
      x += tw + 10;
    }
  }
  int np = (cands.size() + PAGE_N - 1) / PAGE_N;
  drawBtn(SCREEN_W - 52, KB_TOP + 2, 24, BAR_H - 4, "<", candPage > 0 ? COL_KEY_FN : COL_KEY, COL_TEXT);
  drawBtn(SCREEN_W - 26, KB_TOP + 2, 24, BAR_H - 4, ">", candPage < np - 1 ? COL_KEY_FN : COL_KEY, COL_TEXT);
}

static void buildLayout()
{
  keyCount = 0;
  static const char *r1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
  static const char *r2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
  static const char *r3[] = {"z", "x", "c", "v", "b", "n", "m"};
  static const char *s1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
  static const char *s2[] = {"@", "#", "$", "%", "&", "*", "-", "+", "(", ")"};
  static const char *s3[] = {"?", "!", ":", "=", "\"", "_", "/"};
  if (cnMode)
  {
    int top = KB_TOP + BAR_H + 2;
    const int r[4] = {top, top + ROW_STEP_CN, top + 2 * ROW_STEP_CN, top + 3 * ROW_STEP_CN};
    for (int i = 0; i < 10; i++)
      addKey(6 + i * 31, r[0], 29, ROW_H_CN, K_CHAR, r1[i]);
    for (int i = 0; i < 9; i++)
      addKey(21 + i * 31, r[1], 29, ROW_H_CN, K_CHAR, r2[i]);
    addKey(10, r[2], 40, ROW_H_CN, K_CN, "中");
    for (int i = 0; i < 7; i++)
      addKey(52 + i * 31, r[2], 29, ROW_H_CN, K_CHAR, r3[i]);
    addKey(269, r[2], 41, ROW_H_CN, K_BACK, "删");
    addKey(51, r[3], 29, ROW_H_CN, K_CHAR, ",");
    addKey(82, r[3], 144, ROW_H_CN, K_SPACE, "空格");
    addKey(228, r[3], 29, ROW_H_CN, K_CHAR, ".");
    addKey(259, r[3], 58, ROW_H_CN, K_SEND, sendLabel);
  }
  else
  {
    const int r[4] = {KB_TOP + 4, KB_TOP + 4 + ROW_STEP_EN, KB_TOP + 4 + 2 * ROW_STEP_EN, KB_TOP + 4 + 3 * ROW_STEP_EN};
    for (int i = 0; i < 10; i++)
      addKey(6 + i * 31, r[0], 29, ROW_H_EN, K_CHAR, symMode ? s1[i] : r1[i]);
    for (int i = 0; i < 9; i++)
      addKey(21 + i * 31, r[1], 29, ROW_H_EN, K_CHAR, symMode ? s2[i] : r2[i]);
    if (symMode)
    {
      for (int i = 0; i < 7; i++)
        addKey(52 + i * 31, r[2], 29, ROW_H_EN, K_CHAR, s3[i]);
    }
    else
    {
      addKey(10, r[2], 40, ROW_H_EN, K_SHIFT, "Aa");
      for (int i = 0; i < 7; i++)
        addKey(52 + i * 31, r[2], 29, ROW_H_EN, K_CHAR, r3[i]);
    }
    addKey(269, r[2], 41, ROW_H_EN, K_BACK, "删");
    addKey(3, r[3], 44, ROW_H_EN, K_MODE, symMode ? "ABC" : "123");
    addKey(49, r[3], 34, ROW_H_EN, K_CN, "中");
    addKey(85, r[3], 24, ROW_H_EN, K_CHAR, ",");
    addKey(111, r[3], 114, ROW_H_EN, K_SPACE, "空格");
    addKey(227, r[3], 24, ROW_H_EN, K_CHAR, ".");
    addKey(253, r[3], 64, ROW_H_EN, K_SEND, sendLabel);
  }
}

static void drawKey(int i, bool hl)
{
  KeyRect &k = keys[i];
  uint16_t bg = COL_KEY;
  if (hl) bg = COL_ACCENT;
  else if (k.id == K_SHIFT && upper) bg = COL_ACCENT_DK;
  else if (k.id == K_CN && cnMode) bg = COL_ACCENT_DK;
  else if (k.id != K_CHAR) bg = COL_KEY_FN;
  uint16_t fg = hl ? 0xFFFF : COL_TEXT;
  gfx->fillRoundRect(k.x, k.y, k.w, k.h, 6, bg);
  const char *lab = k.label;
  char up[2] = {0, 0};
  if (!cnMode && !symMode && upper && k.id == K_CHAR && k.label[0] >= 'a' && k.label[0] <= 'z')
  {
    up[0] = k.label[0] - 32;
    lab = up;
  }
  int tw = textWidth(lab);
  drawTextAt(k.x + (k.w - tw) / 2, centerBaseY(k.y, k.h), lab, fg, bg);
}

void kbDraw()
{
  if (hidden)
  {
    gfx->fillRect(0, inputLineY() + INPUT_H, SCREEN_W, SCREEN_H - inputLineY() - INPUT_H, COL_BG);
    return;
  }
  buildLayout();
  gfx->fillRect(0, KB_TOP - 2, SCREEN_W, SCREEN_H - KB_TOP + 2, COL_BG);
  gfx->drawFastHLine(0, KB_TOP - 2, SCREEN_W, COL_LINE);
  if (cnMode) drawBar();
  for (int i = 0; i < keyCount; i++) drawKey(i, false);
}

void kbDrawInputLine()
{
  int iy = inputLineY();
  gfx->fillRect(0, iy, SCREEN_W, INPUT_H, COL_PANEL);
  gfx->drawFastHLine(0, iy, SCREEN_W, COL_LINE);
  drawBtn(274, iy + 3, 42, 22, hidden ? "键盘" : "收起", COL_KEY_FN, COL_TEXT);
  if (!target) return;
  String show;
  if (mask)
  {
    int n = utf8Count(*target);
    for (int i = 0; i < n; i++) show += '*';
  }
  else show = *target;
  show += ((millis() / 500) & 1) ? "_" : "";
  show = fitTail(show, SCREEN_W - 70);
  drawTextAt(8, centerBaseY(iy, INPUT_H), show, COL_TEXT, COL_PANEL);
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
    latch = false;
    return 0;
  }
  if (latch) return 0;
  if (ptIn(x, y, 274, inputLineY() + 3, 42, 22))
  {
    hidden = !hidden;
    pyBuf.clear();
    cands.clear();
    candPage = 0;
    latch = true;
    return KB_LAYOUT;
  }
  if (hidden) return 0;
  if (cnMode && y >= KB_TOP && y < KB_TOP + BAR_H)
  {
    if (ptIn(x, y, SCREEN_W - 52, KB_TOP + 2, 24, BAR_H - 4))
    {
      if (candPage > 0) { candPage--; kbDraw(); }
      return 0;
    }
    if (ptIn(x, y, SCREEN_W - 26, KB_TOP + 2, 24, BAR_H - 4))
    {
      int np = (cands.size() + PAGE_N - 1) / PAGE_N;
      if (candPage < np - 1) { candPage++; kbDraw(); }
      return 0;
    }
    for (int i = 0; i < candHitsN; i++)
    {
      if (x >= candHits[i].x && x <= candHits[i].x + candHits[i].w)
      {
        commitCand(candHits[i].i);
        kbDraw();
        return KB_CHANGED;
      }
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
      if (cnMode)
      {
        if (pyBuf.length() < 12)
        {
          pyBuf += k.label;
          buildCands();
        }
      }
      else if (target)
      {
        if (!symMode && upper && k.label[0] >= 'a' && k.label[0] <= 'z')
          *target += (char)(k.label[0] - 32);
        else
          *target += k.label;
      }
      ev |= KB_CHANGED;
      kbDraw();
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
    case K_CN:
      cnMode = !cnMode;
      upper = false;
      symMode = false;
      pyBuf.clear();
      cands.clear();
      candPage = 0;
      ev |= KB_LAYOUT;
      latch = true;
      kbDraw();
      break;
    case K_BACK:
      if (cnMode && pyBuf.length())
      {
        pyBuf.remove(pyBuf.length() - 1);
        buildCands();
      }
      else backspace();
      ev |= KB_CHANGED;
      kbDraw();
      break;
    case K_SPACE:
      if (cnMode)
      {
        if (!cands.empty()) commitCand(candPage * PAGE_N);
      }
      else if (target) *target += ' ';
      ev |= KB_CHANGED;
      kbDraw();
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
      if (cnMode && pyBuf.length())
      {
        pyBuf.remove(pyBuf.length() - 1);
        buildCands();
      }
      else backspace();
      ev |= KB_CHANGED;
      kbDraw();
    }
  }
  return ev;
}
