#include "chat_store.h"
#include "board_config.h"
#include "ui.h"

std::vector<ChatMsg> msgs;

static const int BUB_MAX_W = 244;
static const int BUB_PAD = 10;
static const int BUB_VPAD = 6;
static const int MSG_GAP = 10;

static bool breakable(const String &s, unsigned i)
{
  uint8_t c = (uint8_t)s[i];
  if (c == ' ') return true;
  if (c < 0x80) return false;
  uint32_t cp = 0;
  int cl = 1;
  if (c >= 0xF0) { cp = c & 0x07; cl = 4; }
  else if (c >= 0xE0) { cp = c & 0x0F; cl = 3; }
  else if (c >= 0xC0) { cp = c & 0x1F; cl = 2; }
  for (int k = 1; k < cl && i + k < s.length(); k++)
    cp = (cp << 6) | (((uint8_t)s[i + k]) & 0x3F);
  return cp >= 0x2E80;
}

static void wrapSegment(const String &seg, std::vector<String> &out)
{
  if (!seg.length()) { out.push_back(""); return; }
  String line;
  line.reserve(160);
  int lastOk = -1;
  unsigned i = 0;
  while (i < seg.length())
  {
    uint8_t c = (uint8_t)seg[i];
    int cl = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    if (i + cl > seg.length()) cl = seg.length() - i;
    String chunk = seg.substring(i, i + cl);
    if (line.length() && textWidth(line + chunk) > BUB_MAX_W)
    {
      if (lastOk > 0 && lastOk < (int)line.length())
      {
        out.push_back(line.substring(0, lastOk));
        String rest = line.substring(lastOk);
        rest.trim();
        line = rest;
      }
      else
      {
        out.push_back(line);
        line = "";
      }
      lastOk = -1;
    }
    line += chunk;
    if (breakable(seg, i)) lastOk = line.length();
    i += cl;
  }
  out.push_back(line);
}

static void wrapMsg(ChatMsg &m)
{
  m.lines.clear();
  unsigned start = 0;
  while (start <= m.text.length())
  {
    int nl = m.text.indexOf('\n', start);
    String seg = (nl < 0) ? m.text.substring(start) : m.text.substring(start, nl);
    wrapSegment(seg, m.lines);
    if (nl < 0) break;
    start = nl + 1;
  }
  int maxW = 0;
  for (auto &l : m.lines) maxW = max(maxW, textWidth(l));
  m.bw = min(maxW, BUB_MAX_W) + 2 * BUB_PAD;
  m.bh = (int)m.lines.size() * LINE_H + 2 * BUB_VPAD;
}

void chatAdd(bool user, const String &text, bool err)
{
  ChatMsg m;
  m.user = user;
  m.err = err;
  m.text = text;
  sanitizeText(m.text);
  if (m.text.length() > 8000) m.text = m.text.substring(0, 8000);
  wrapMsg(m);
  msgs.push_back(m);
}

void chatClear()
{
  msgs.clear();
}

int chatContentHeight(bool busy)
{
  int hgt = 0;
  for (auto &m : msgs) hgt += m.bh + MSG_GAP;
  if (busy) hgt += LINE_H + 2 * BUB_VPAD + MSG_GAP;
  return hgt > 0 ? hgt - MSG_GAP : 0;
}

static void drawBubble(ChatMsg &m, int by)
{
  int bx = m.user ? (SCREEN_W - 10 - m.bw) : 10;
  uint16_t bg = m.err ? COL_ERR_BG : (m.user ? COL_ACCENT_DK : COL_PANEL_HI);
  uint16_t fg = m.err ? COL_ERR_TX : COL_TEXT;
  int cy0 = max(by, CHAT_TOP);
  int cy1 = min(by + m.bh, CHAT_BOT);
  if (cy1 <= cy0) return;
  if (by >= CHAT_TOP && by + m.bh <= CHAT_BOT)
    gfx->fillRoundRect(bx, by, m.bw, m.bh, 8, bg);
  else
    gfx->fillRect(bx, cy0, m.bw, cy1 - cy0, bg);
  int ty = by + BUB_VPAD;
  for (auto &l : m.lines)
  {
    int base = ty + FONT_ASC;
    if (base - FONT_ASC >= CHAT_TOP && base + 4 <= CHAT_BOT && l.length())
      drawTextAt(bx + BUB_PAD, base, l, fg, bg);
    ty += LINE_H;
  }
}

void chatRender(int top, int h, int scroll, bool busy, int busyPhase)
{
  gfx->fillRect(0, top, SCREEN_W, h, COL_BG);
  int contentH = chatContentHeight(busy);
  int maxScroll = max(0, contentH - h);
  if (scroll > maxScroll) scroll = maxScroll;
  if (scroll < 0) scroll = 0;
  int y = top + h - scroll - contentH;
  for (auto &m : msgs)
  {
    if (y + m.bh >= top && y <= top + h) drawBubble(m, y);
    y += m.bh + MSG_GAP;
  }
  if (busy)
  {
    String t = "思考中";
    for (int i = 0; i <= busyPhase % 3; i++) t += '.';
    ChatMsg tmp;
    tmp.user = false;
    tmp.err = false;
    tmp.text = t;
    wrapMsg(tmp);
    if (y + tmp.bh >= top && y <= top + h) drawBubble(tmp, y);
  }
}
