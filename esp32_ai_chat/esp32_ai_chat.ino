#include <WiFi.h>
#include <algorithm>
#include "board_config.h"
#include "ui.h"
#include "secrets.h"
#include "touch_driver.h"
#include "config_store.h"
#include "chat_store.h"
#include "keyboard.h"
#include "ai_client.h"

enum Screen : uint8_t { S_HOME, S_CHAT, S_SETTINGS, S_WIFI, S_WIFIPASS, S_API, S_XIAOZHI };
static Screen scr = S_CHAT;

static bool dirtyFull = true;
static bool dirtyChat = false;
static bool dirtyInput = false;

static int chatScroll = 0;
static String draft;

static String toastMsg;
static uint32_t toastUntil = 0;
static bool needRestore = false;

static bool dlgClear = false;

struct ApInfo { String ssid; int rssi; };
static std::vector<ApInfo> apList;
static bool scanPending = false;
static int wifiScroll = 0;

static String selSsid;
static bool wcConnecting = false;
static uint32_t wcStart = 0;
static String wcSsid, wcPw;

static int editField = -1;
static String editDraft;

static bool lastDown = false;
static int lastX = 0, lastY = 0, downX = 0, downY = 0;
static int movedDist = 0;
static bool chatDragging = false;
static bool listDragging = false;
static int dragStartY = 0, scrollAtDragStart = 0;

static uint8_t lastBusyPhase = 0xFF;
static uint8_t lastBlink = 0;
static uint32_t lastWatchdog = 0;

static const int LIST_TOP = 76;
static const int ITEM_H = 44;

static void gotoScreen(Screen s)
{
  scr = s;
  dirtyFull = true;
  chatDragging = false;
  listDragging = false;
}

static void showToast(const String &s, uint32_t ms = 2000)
{
  toastMsg = s;
  toastUntil = millis() + ms;
  needRestore = true;
}

static bool wifiUp()
{
  return WiFi.status() == WL_CONNECTED;
}

static void startWifiConnect(const String &ssid, const String &pw)
{
  WiFi.disconnect(false, false);
  delay(50);
  wcSsid = ssid;
  wcPw = pw;
  WiFi.begin(ssid.c_str(), pw.c_str());
  wcConnecting = true;
  wcStart = millis();
}

static void serviceWifiConnect()
{
  if (!wcConnecting) return;
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED)
  {
    wcConnecting = false;
    cfg.ssid = wcSsid;
    cfg.wpass = wcPw;
    cfgSaveWiFi();
    kbSetMask(false);
    kbSetSendLabel("发送");
    kbReset();
    showToast("已连接 " + WiFi.localIP().toString());
    gotoScreen(S_SETTINGS);
    return;
  }
  if (millis() - wcStart > 15000 || st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)
  {
    wcConnecting = false;
    showToast("连接失败，请检查密码");
    dirtyFull = true;
  }
}

static void scanStart()
{
  apList.clear();
  WiFi.scanNetworks(true, true);
  scanPending = true;
  dirtyFull = true;
}

static void serviceScan()
{
  if (!scanPending) return;
  int n = WiFi.scanComplete();
  if (n >= 0)
  {
    apList.clear();
    for (int i = 0; i < n; i++)
    {
      String s = WiFi.SSID(i);
      if (!s.length()) continue;
      ApInfo a{s, WiFi.RSSI(i)};
      apList.push_back(a);
    }
    sort(apList.begin(), apList.end(), [](const ApInfo &a, const ApInfo &b) { return a.rssi > b.rssi; });
    WiFi.scanDelete();
    scanPending = false;
    wifiScroll = 0;
    dirtyFull = true;
  }
  else if (n == WIFI_SCAN_FAILED)
  {
    scanPending = false;
  }
}

static void sendToAi()
{
  String text = kbPeek();
  text.trim();
  if (!text.length()) return;
  if (!wifiUp()) { showToast("未联网，请先在设置中配置 WiFi"); return; }
  if (aiBusy()) { showToast("AI 正在回复，请稍候"); return; }
  kbTake();
  chatAdd(true, text);
  chatScroll = 0;
  dirtyChat = true;
  dirtyInput = true;
  std::vector<AiMsg> hist;
  for (auto &m : msgs)
  {
    if (m.err) continue;
    hist.push_back({m.user ? "user" : "assistant", m.text});
  }
  if (hist.size() > 20) hist.erase(hist.begin(), hist.end() - 20);
  aiStart(hist, apiEndpoint(cfg.url), cfg.key, cfg.model);
}

static void serviceAi()
{
  String rep, err;
  if (!aiPoll(rep, err)) return;
  if (err.length()) chatAdd(false, "[错误] " + err, true);
  else chatAdd(false, rep);
  chatScroll = 0;
  dirtyChat = true;
}

static void drawSplash(const String &msg)
{
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
  tw = textWidth(msg.c_str());
  drawTextAt((SCREEN_W - tw) / 2, 300, msg.c_str(), COL_SUB, COL_BG);
  gfx->flush();
}

static void drawTitleBar()
{
  gfx->fillRect(0, 0, SCREEN_W, TITLE_H, COL_PANEL);
  gfx->drawFastHLine(0, TITLE_H, SCREEN_W, COL_LINE);
  if (scr == S_CHAT)
  {
    drawTextAt(12, centerBaseY(0, TITLE_H), "AI 聊天", COL_TEXT, COL_PANEL);
    bool up = wifiUp();
    gfx->fillCircle(236, TITLE_H / 2, 5, up ? COL_OK : 0xF980);
    drawBtn(254, 4, 62, 28, "主页", COL_KEY_FN, COL_TEXT);
  }
  else if (scr == S_HOME)
  {
    drawTextAt(12, centerBaseY(0, TITLE_H), "应用", COL_TEXT, COL_PANEL);
    bool up = wifiUp();
    gfx->fillCircle(236, TITLE_H / 2, 5, up ? COL_OK : 0xF980);
  }
  else
  {
    drawBtn(3, 4, 56, 28, "主页", COL_KEY_FN, COL_TEXT);
    const char *t = (scr == S_SETTINGS) ? "设置" : (scr == S_WIFI) ? "WiFi 设置"
                                     : (scr == S_WIFIPASS)        ? "输入密码"
                                     : (scr == S_XIAOZHI)         ? "小志"
                                                                  : "API 设置";
    drawTextAt(70, centerBaseY(0, TITLE_H), t, COL_TEXT, COL_PANEL);
  }
}

static void drawSettingsBody()
{
  struct Row { int y; const char *label; };
  Row rows[5] = {
    {44, "WiFi 设置"},
    {104, "AI 接口设置"},
    {164, "清除聊天记录"},
    {224, "重启设备"},
    {284, "返回聊天"},
  };
  for (auto &r : rows)
  {
    gfx->fillRoundRect(3, r.y, 314, 48, 8, COL_PANEL);
    drawTextAt(14, r.y + 19, r.label, COL_TEXT, COL_PANEL);
    drawTextAt(296, r.y + 19, ">", COL_SUB, COL_PANEL);
  }
  drawTextAt(14, 344, "触摸 AI 助手 v1.1", COL_SUB, COL_BG);
}

static void drawRssi(int x, int y, int rssi)
{
  int lvl = rssi > -60 ? 4 : rssi > -70 ? 3 : rssi > -80 ? 2 : 1;
  for (int i = 0; i < 4; i++)
  {
    int bh = 4 + i * 3;
    uint16_t c = (i < lvl) ? COL_OK : COL_LINE;
    gfx->fillRect(x + i * 6, y + 12 - bh, 4, bh, c);
  }
}

static void drawWifiBody()
{
  drawBtn(3, 40, 110, 32, scanPending ? "扫描中.." : "重新扫描", COL_KEY_FN, COL_TEXT);
  String hint = scanPending ? "正在搜索附近网络..." : (apList.size() ? String("共 ") + apList.size() + " 个网络，点击选择" : String("未发现网络"));
  drawTextAt(122, centerBaseY(40, 32), hint.c_str(), COL_SUB, COL_BG);

  int contentH = (int)apList.size() * ITEM_H;
  int viewH = SCREEN_H - 6 - LIST_TOP;
  int maxScroll = max(0, contentH - viewH);
  if (wifiScroll > maxScroll) wifiScroll = maxScroll;
  for (int i = 0; i < (int)apList.size(); i++)
  {
    int iy = LIST_TOP + i * ITEM_H - wifiScroll;
    if (iy + ITEM_H < LIST_TOP || iy > SCREEN_H) continue;
    gfx->fillRoundRect(3, iy, 314, ITEM_H - 6, 8, COL_PANEL);
    drawTextAt(14, iy + 27, fitTail(apList[i].ssid, 190), COL_TEXT, COL_PANEL);
    String dbm = String(apList[i].rssi) + "dBm";
    drawTextAt(232, iy + 27, dbm, COL_SUB, COL_PANEL);
    drawRssi(292, iy + 10, apList[i].rssi);
  }
}

static void drawWifiPassBody()
{
  gfx->fillRoundRect(3, 42, 314, 52, 8, COL_PANEL);
  drawTextAt(14, 64, "网络:", COL_SUB, COL_PANEL);
  drawTextAt(14, 86, fitTail(selSsid, 180), COL_TEXT, COL_PANEL);
  drawBtn(227, 48, 82, 40, "连接", COL_ACCENT, 0xFFFF);
  drawTextAt(14, 130, "请在下方输入框输入密码", COL_SUB, COL_BG);
}

static void drawApiBody()
{
  if (editField >= 0)
  {
    static const char *titles[] = {"编辑接口地址", "编辑模型名称", "编辑 API Key"};
    drawTextAt(14, 58, titles[editField], COL_SUB, COL_BG);
    gfx->fillRoundRect(3, 70, 314, 40, 8, COL_PANEL);
    String v = (editField == 2) ? [] { String s; int n = utf8Count(editDraft); for (int i = 0; i < n; i++) s += '*'; return s; }() : editDraft;
    v += ((millis() / 500) & 1) ? "_" : "";
    drawTextAt(12, centerBaseY(70, 40), fitTail(v, 290), COL_TEXT, COL_PANEL);
    drawBtn(3, 246, 100, 44, "取消", COL_KEY_FN, COL_TEXT);
    drawBtn(217, 246, 100, 44, "保存", COL_ACCENT, 0xFFFF);
    return;
  }
  struct Card { int y; const char *label; const String *value; bool mask; };
  String keyMasked = cfg.key.length() ? cfg.key.substring(0, min(6, (int)cfg.key.length())) + "****" : "未设置";
  Card cards[3] = {
    {44, "接口地址", &cfg.url, false},
    {100, "模型名称", &cfg.model, false},
    {156, "API Key", &keyMasked, false},
  };
  for (auto &c : cards)
  {
    gfx->fillRoundRect(3, c.y, 314, 48, 8, COL_PANEL);
    drawTextAt(14, c.y + 19, c.label, COL_SUB, COL_PANEL);
    drawTextAt(90, c.y + 19, fitTail(*c.value, 215), COL_TEXT, COL_PANEL);
    drawTextAt(296, c.y + 19, ">", COL_SUB, COL_PANEL);
  }
  drawBtn(3, 216, 314, 44, "保存设置", COL_ACCENT, 0xFFFF);
  drawBtn(3, 264, 155, 36, "预设:云端", COL_KEY_FN, COL_TEXT);
  drawBtn(162, 264, 155, 36, "预设:本地", COL_KEY_FN, COL_TEXT);
}

static void drawChatIcon(int cx, int cy)
{
  gfx->fillRoundRect(cx - 20, cy - 16, 40, 28, 8, COL_ACCENT);
  gfx->fillTriangle(cx - 12, cy + 10, cx - 2, cy + 10, cx - 12, cy + 22, COL_ACCENT);
  gfx->fillCircle(cx - 9, cy - 2, 3, 0xFFFF);
  gfx->fillCircle(cx + 1, cy - 2, 3, 0xFFFF);
  gfx->fillCircle(cx + 11, cy - 2, 3, 0xFFFF);
}

static void drawMicIcon(int cx, int cy)
{
  uint16_t c = COL_ACCENT;
  gfx->fillRoundRect(cx - 8, cy - 20, 16, 26, 8, c);
  gfx->fillRect(cx - 13, cy - 6, 3, 10, c);
  gfx->fillRect(cx + 10, cy - 6, 3, 10, c);
  gfx->fillRect(cx - 13, cy + 2, 27, 3, c);
  gfx->fillRect(cx - 2, cy + 5, 4, 8, c);
  gfx->fillRect(cx - 8, cy + 13, 16, 3, c);
}

static void drawGearIcon(int cx, int cy)
{
  uint16_t c = COL_ACCENT;
  gfx->fillCircle(cx, cy, 13, c);
  for (int i = 0; i < 8; i++)
  {
    float a = i * 0.785398f;
    int x = cx + (int)(cosf(a) * 15.5f);
    int y = cy + (int)(sinf(a) * 15.5f);
    gfx->fillCircle(x, y, 4, c);
  }
  gfx->fillCircle(cx, cy, 6, COL_PANEL);
}

static void drawHomeBody()
{
  struct Tile { int x; const char *label; void (*icon)(int, int); };
  Tile tiles[3] = {
    {12, "AI 聊天", drawChatIcon},
    {116, "小志", drawMicIcon},
    {220, "设置", drawGearIcon},
  };
  for (auto &t : tiles)
  {
    gfx->fillRoundRect(t.x, 96, 88, 118, 14, COL_PANEL);
    gfx->drawRoundRect(t.x, 96, 88, 118, 14, COL_LINE);
    t.icon(t.x + 44, 142);
    int tw = textWidth(t.label);
    drawTextAt(t.x + (88 - tw) / 2, 196, t.label, COL_TEXT, COL_PANEL);
  }
  drawTextAt(14, 260, "更多应用敬请期待", COL_SUB, COL_BG);
}

static void drawXiaozhiBody()
{
  gfx->fillRoundRect(3, 60, 314, 200, 12, COL_PANEL);
  drawMicIcon(160, 140);
  const char *l1 = "小志语音助手";
  const char *l2 = "集成开发中，敬请期待";
  drawTextAt((SCREEN_W - textWidth(l1)) / 2, 190, l1, COL_TEXT, COL_PANEL);
  drawTextAt((SCREEN_W - textWidth(l2)) / 2, 220, l2, COL_SUB, COL_PANEL);
}

static void drawBody()
{
  switch (scr)
  {
  case S_HOME:
    drawHomeBody();
    break;
  case S_XIAOZHI:
    drawXiaozhiBody();
    break;
  case S_CHAT:
    chatRender(CHAT_TOP, CHAT_BOT - CHAT_TOP, chatScroll, aiBusy(), lastBusyPhase % 3);
    break;
  case S_SETTINGS:
    drawSettingsBody();
    break;
  case S_WIFI:
    drawWifiBody();
    break;
  case S_WIFIPASS:
    drawWifiPassBody();
    break;
  case S_API:
    drawApiBody();
    break;
  }
}

static void drawToast()
{
  int tw = textWidth(toastMsg);
  int w = min(300, tw + 28);
  int x = (SCREEN_W - w) / 2;
  gfx->fillRoundRect(x, 54, w, 36, 10, COL_PANEL_HI);
  gfx->drawRoundRect(x, 54, w, 36, 10, COL_ACCENT);
  drawTextAt(x + (w - tw) / 2, centerBaseY(54, 36), toastMsg.c_str(), 0xFFFF, COL_PANEL_HI);
}

static void drawClearDialog()
{
  gfx->fillRoundRect(30, 150, 260, 150, 12, COL_PANEL);
  gfx->drawRoundRect(30, 150, 260, 150, 12, COL_ACCENT);
  const char *msg = "确定清除所有聊天记录？";
  int tw = textWidth(msg);
  drawTextAt((SCREEN_W - tw) / 2, 205, msg, COL_TEXT, COL_PANEL);
  drawBtn(50, 240, 100, 44, "取消", COL_KEY_FN, COL_TEXT);
  drawBtn(170, 240, 100, 44, "确定", COL_ERR_BG, COL_ERR_TX);
}

static void drawConnectingOverlay()
{
  gfx->fillRoundRect(30, 170, 260, 120, 12, COL_PANEL);
  gfx->drawRoundRect(30, 170, 260, 120, 12, COL_ACCENT);
  const char *l1 = "正在连接网络...";
  int tw = textWidth(l1);
  drawTextAt((SCREEN_W - tw) / 2, 220, l1, COL_TEXT, COL_PANEL);
  drawTextAt((SCREEN_W - textWidth(selSsid)) / 2, 250, selSsid, COL_SUB, COL_PANEL);
}

static bool kbVisible()
{
  return scr == S_CHAT || scr == S_WIFIPASS || (scr == S_API && editField >= 0);
}

static bool inputLineVisible()
{
  return scr == S_CHAT || scr == S_WIFIPASS;
}

static void compose()
{
  bool need = false;
  if (dirtyFull)
  {
    gfx->fillScreen(COL_BG);
    drawTitleBar();
    drawBody();
    if (inputLineVisible()) kbDrawInputLine();
    if (kbVisible()) kbDraw();
    dirtyChat = dirtyInput = false;
    need = true;
  }
  else
  {
    if (dirtyChat && scr == S_CHAT)
    {
      chatRender(CHAT_TOP, CHAT_BOT - CHAT_TOP, chatScroll, aiBusy(), lastBusyPhase % 3);
      need = true;
    }
    if (dirtyInput && inputLineVisible())
    {
      kbDrawInputLine();
      need = true;
    }
  }
  if (wcConnecting && scr == S_WIFIPASS) { drawConnectingOverlay(); need = true; }
  if (dlgClear) { drawClearDialog(); need = true; }
  if (millis() < toastUntil) { drawToast(); need = true; }
  else if (needRestore) { needRestore = false; dirtyFull = true; }
  if (need) gfx->flush();
}

static void enterEdit(int field)
{
  editField = field;
  kbSetMask(field == 2);
  kbSetSendLabel("完成");
  kbAttach(&editDraft);
  kbReset();
  editDraft = (field == 0) ? cfg.url : (field == 1) ? cfg.model : cfg.key;
  dirtyFull = true;
}

static void exitEdit(bool save)
{
  if (save)
  {
    editDraft.trim();
    if (editField == 0) cfg.url = editDraft;
    else if (editField == 1) cfg.model = editDraft;
    else cfg.key = editDraft;
    cfgSaveApi();
    showToast("已保存");
  }
  editField = -1;
  kbAttach(&draft);
  kbSetMask(false);
  kbSetSendLabel("发送");
  dirtyFull = true;
}

static void handleChatTouch(bool press, bool release, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 254, 4, 62, 28) || ptIn(x, y, 222, 8, 28, 20))
    {
      gotoScreen(S_HOME);
      return;
    }
  }
  if (down && !press && y >= CHAT_TOP && y < CHAT_BOT)
  {
    if (!chatDragging)
    {
      chatDragging = true;
      dragStartY = y;
      scrollAtDragStart = chatScroll;
    }
    int dy = y - dragStartY;
    if (dy)
    {
      int contentH = chatContentHeight(aiBusy());
      int maxScroll = max(0, contentH - (CHAT_BOT - CHAT_TOP));
      chatScroll = scrollAtDragStart - dy;
      if (chatScroll < 0) chatScroll = 0;
      if (chatScroll > maxScroll) chatScroll = maxScroll;
      dirtyChat = true;
    }
  }
  if (!down) chatDragging = false;
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyInput = true;
  if (ev & KB_SEND) sendToAi();
}

static void handleSettingsTouch(bool press, int x, int y)
{
  if (!press) return;
  if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_HOME); return; }
  if (ptIn(x, y, 3, 44, 314, 48)) { scanStart(); gotoScreen(S_WIFI); return; }
  if (ptIn(x, y, 3, 104, 314, 48)) { gotoScreen(S_API); return; }
  if (ptIn(x, y, 3, 164, 314, 48)) { dlgClear = true; dirtyFull = true; return; }
  if (ptIn(x, y, 3, 224, 314, 48))
  {
    showToast("正在重启...");
    gfx->flush();
    delay(500);
    ESP.restart();
  }
  if (ptIn(x, y, 3, 284, 314, 48)) { gotoScreen(S_CHAT); }
}

static void handleWifiTouch(bool press, bool release, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SETTINGS); return; }
    if (ptIn(x, y, 3, 40, 110, 32)) { scanStart(); return; }
  }
  if (down && y >= LIST_TOP)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = wifiScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int contentH = (int)apList.size() * ITEM_H;
      int viewH = SCREEN_H - 6 - LIST_TOP;
      int maxScroll = max(0, contentH - viewH);
      wifiScroll = scrollAtDragStart - dy;
      if (wifiScroll < 0) wifiScroll = 0;
      if (wifiScroll > maxScroll) wifiScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
  if (release && movedDist < 10 && y >= LIST_TOP)
  {
    int idx = (y + wifiScroll - LIST_TOP) / ITEM_H;
    if (idx >= 0 && idx < (int)apList.size())
    {
      selSsid = apList[idx].ssid;
      kbReset();
      kbAttach(&draft);
      kbSetMask(true);
      kbSetSendLabel("完成");
      gotoScreen(S_WIFIPASS);
    }
  }
}

static void handleWifiPassTouch(bool press, bool down, int x, int y)
{
  if (wcConnecting) return;
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28))
    {
      kbSetMask(false);
      kbSetSendLabel("发送");
      kbReset();
      gotoScreen(S_WIFI);
      return;
    }
    if (ptIn(x, y, 227, 48, 82, 40))
    {
      String pw = draft;
      pw.trim();
      if (!pw.length()) { showToast("请输入密码"); return; }
      draft.clear();
      dirtyInput = true;
      startWifiConnect(selSsid, pw);
      dirtyFull = true;
      return;
    }
  }
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyInput = true;
  if (ev & KB_SEND)
  {
    String pw = draft;
    pw.trim();
    if (pw.length())
    {
      draft.clear();
      dirtyInput = true;
      startWifiConnect(selSsid, pw);
      dirtyFull = true;
    }
  }
}

static void handleApiTouch(bool press, bool down, int x, int y)
{
  if (editField >= 0)
  {
    if (press)
    {
      if (ptIn(x, y, 3, 246, 100, 44)) { exitEdit(false); return; }
      if (ptIn(x, y, 217, 246, 100, 44)) { exitEdit(true); return; }
    }
    uint8_t ev = kbHandle(down, x, y);
    if (ev & KB_CHANGED) dirtyFull = true;
    if (ev & KB_SEND) exitEdit(true);
    return;
  }
  if (!press) return;
  if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SETTINGS); return; }
  if (ptIn(x, y, 3, 44, 314, 48)) { enterEdit(0); return; }
  if (ptIn(x, y, 3, 100, 314, 48)) { enterEdit(1); return; }
  if (ptIn(x, y, 3, 156, 314, 48)) { enterEdit(2); return; }
  if (ptIn(x, y, 3, 216, 314, 44)) { cfgSaveApi(); showToast("已保存"); return; }
  if (ptIn(x, y, 3, 264, 155, 36))
  {
    cfg.url = "https://api.deepseek.com";
    cfg.model = "deepseek-chat";
    cfgSaveApi();
    showToast("已切换云端 DeepSeek");
    dirtyFull = true;
    return;
  }
  if (ptIn(x, y, 162, 264, 155, 36))
  {
    cfg.url = DEFAULT_URL;
    cfg.model = DEFAULT_MODEL;
    cfgSaveApi();
    showToast("已切换本地服务");
    dirtyFull = true;
    return;
  }
}

static void handleDialogTouch(bool press, int x, int y)
{
  if (!press) return;
  if (ptIn(x, y, 50, 240, 100, 44)) { dlgClear = false; dirtyFull = true; return; }
  if (ptIn(x, y, 170, 240, 100, 44))
  {
    dlgClear = false;
    chatClear();
    chatScroll = 0;
    showToast("聊天记录已清除");
    dirtyFull = true;
  }
}

static void handleHomeTouch(bool press, int x, int y)
{
  if (!press) return;
  if (ptIn(x, y, 12, 96, 88, 118)) { gotoScreen(S_CHAT); return; }
  if (ptIn(x, y, 116, 96, 88, 118)) { gotoScreen(S_XIAOZHI); return; }
  if (ptIn(x, y, 220, 96, 88, 118)) { gotoScreen(S_SETTINGS); return; }
}

static void routeTouch(bool press, bool release, bool down, int x, int y)
{
  if (dlgClear) { handleDialogTouch(press, x, y); return; }
  switch (scr)
  {
  case S_HOME: handleHomeTouch(press, x, y); break;
  case S_XIAOZHI:
    if (press && ptIn(x, y, 3, 4, 56, 28)) gotoScreen(S_HOME);
    break;
  case S_CHAT: handleChatTouch(press, release, down, x, y); break;
  case S_SETTINGS: handleSettingsTouch(press, x, y); break;
  case S_WIFI: handleWifiTouch(press, release, down, x, y); break;
  case S_WIFIPASS: handleWifiPassTouch(press, down, x, y); break;
  case S_API: handleApiTouch(press, down, x, y); break;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[boot] setup enter");
  cfgLoad();
  ui_init();
  touchInit();
  aiBegin();
  kbAttach(&draft);
  kbSetSendLabel("发送");

  drawSplash("正在启动...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  bool ok = false;
  if (cfg.ssid.length())
  {
    drawSplash("正在连接 " + cfg.ssid);
    WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());
    uint32_t t0 = millis();
    while (millis() - t0 < 12000)
    {
      if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
      delay(150);
    }
  }
  scr = S_HOME;
  if (ok)
  {
    drawSplash("IP: " + WiFi.localIP().toString());
    delay(1200);
    showToast("WiFi 已连接");
  }
  else
  {
    showToast(cfg.ssid.length() ? "WiFi 连接失败，请在设置中检查" : "未联网，请在设置中配置 WiFi", 3000);
  }
  dirtyFull = true;
}

void loop()
{
  TouchPoint tp;
  bool down = touchRead(tp) && tp.pressed;
  int x, y;
  if (down) { x = tp.x; y = tp.y; }
  else { x = lastX; y = lastY; }

  bool press = down && !lastDown;
  bool release = !down && lastDown;
  if (press) { downX = x; downY = y; movedDist = 0; }
  if (down)
  {
    int dx = abs(x - downX) + abs(y - downY);
    if (dx > movedDist) movedDist = dx;
  }

  routeTouch(press, release, down, x, y);
  lastDown = down;
  lastX = x;
  lastY = y;

  serviceAi();
  serviceScan();
  serviceWifiConnect();

  if (aiBusy() && scr == S_CHAT)
  {
    uint8_t phase = (millis() / 450) % 3;
    if (phase != lastBusyPhase) { lastBusyPhase = phase; dirtyChat = true; }
  }
  else lastBusyPhase = 0;

  uint8_t blink = (millis() / 500) & 1;
  if (blink != lastBlink)
  {
    lastBlink = blink;
    if (inputLineVisible() || editField >= 0) dirtyInput = true;
    if (editField >= 0) dirtyFull = true;
  }

  if (millis() - lastWatchdog > 15000)
  {
    lastWatchdog = millis();
    if (cfg.ssid.length() && !wifiUp() && !wcConnecting)
      WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());
  }

  compose();
}
