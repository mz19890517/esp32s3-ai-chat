// ESP32-S3-Touch-LCD-3.5B 触摸 AI 助手（1.3.0）
// 升级通道：ArduinoOTA 网络推送 / URL 下载 / TF 卡开机检测 / 蓝牙推送（Web Bluetooth）
// 主循环：读触摸 → 分发 → 服务各子系统 → 差量重绘
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
#include "frame_log.h"
#include "serial_link.h"
#include "ota_update.h"
#include "ota_sd.h"
#include "ota_ble.h"

enum Screen : uint8_t { S_HOME, S_CHAT, S_SETTINGS, S_WIFI, S_WIFIPASS, S_API, S_XIAOZHI, S_SERIAL, S_FRAMES, S_AIANA, S_OTA, S_OTAURL, S_OTABLE };
static Screen scr = S_CHAT;

static bool dirtyFull = true;
static bool dirtyChat = false;
static bool dirtyInput = false;

static int chatScroll = 0;
static String draft;

static int chatViewBot()
{
  return kbIsHidden() ? SCREEN_H - INPUT_H : CHAT_BOT;
}

static String toastMsg;
static uint32_t toastUntil = 0;
static bool needRestore = false;

static bool dlgClear = false;

struct ApInfo { String ssid; int rssi; };
static std::vector<ApInfo> apList;
static bool scanPending = false;
static int scanRetry = 0;
static int wifiScroll = 0;

static String selSsid;
static bool wcConnecting = false;
static uint32_t wcStart = 0;
static String wcSsid, wcPw;
static String passDraft;      // WiFi 密码独立缓冲，不再借用聊天草稿

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

static const int TERM_TOP = 162;
static const int ANS_TOP = 126;
static const int ANS_BOT = 426;

static std::vector<String> termLines;
static std::vector<uint8_t> termTx;
static int serialScroll = 0;
static bool serialEol = true;
static size_t lastFrameCount = 0;
static String lastSlStatus;

static int framesScroll = 0;
static int anaIdx = -1;
static int anaScroll = 0;
static bool anaWorking = false;
static String anaErr;
static std::vector<String> anaLines;
static uint8_t aiOwner = 0;
static uint8_t lastOtaPhase = 0xFF;
static uint8_t lastOtaState = 0xFF;
static int lastOtaPct = -1;

static String otaUrl;
static String otaUrlEdit;     // 地址编辑缓冲（取消时丢弃）
static String sdInfo = "重启自动检测 firmware.bin";

static const char *AI_SYS_ANALYZE = "你是一位嵌入式通信协议分析专家，精通 BLE、串口、AT 指令和各类 IoT 设备协议解析。用简洁准确的中文回答，尽量给出字节级别的解释，不要编造不存在的协议。";

static const int LIST_TOP = 76;
static const int ITEM_H = 44;

#define AI_HIST_CHARS 6000   // 单次请求携带的历史正文字数上限

static void refreshSdInfo()
{
  otaSdScan();
  sdInfo = otaSdStatus();
}

static void gotoScreen(Screen s)
{
  scr = s;
  dirtyFull = true;
  chatDragging = false;
  listDragging = false;
  if (s == S_OTA) refreshSdInfo();
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

static void wrapLines(const String &s, int maxW, std::vector<String> &out)
{
  out.clear();
  String line;
  int w = 0;
  int i = 0;
  while (i < s.length())
  {
    uint8_t c = (uint8_t)s[i];
    int cl = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    if (i + cl > s.length()) cl = s.length() - i;
    int cw = (cl == 1) ? 9 : 17;
    if (w + cw > maxW && line.length())
    {
      out.push_back(line);
      line = "";
      w = 0;
    }
    line += s.substring(i, i + cl);
    w += cw;
    i += cl;
  }
  out.push_back(line);
}

static void buildTermLines()
{
  termLines.clear();
  termTx.clear();
  for (auto &f : frames)
  {
    String head = String(f.rx ? "RX " : "TX ") + flTime(f.ms) + " " + String(f.len) + "B ";
    String body = flHexMode ? f.hex : f.text;
    if (!body.length()) body = flHexMode ? "(空)" : "(无可打印内容)";
    std::vector<String> ls;
    wrapLines(head + body, 296, ls);
    for (auto &l : ls)
    {
      termLines.push_back(l);
      termTx.push_back(f.rx ? 0 : 1);
    }
  }
}

static int hexToBytes(const String &s, uint8_t *out, int maxLen)
{
  int n = 0;
  int hi = -1;
  for (int i = 0; i < s.length() && n < maxLen; i++)
  {
    int c = (uint8_t)s[i];
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c == ' ' || c == ',') continue;
    else return -1;
    if (hi < 0) hi = v;
    else
    {
      out[n++] = (uint8_t)((hi << 4) | v);
      hi = -1;
    }
  }
  return (hi < 0) ? n : -1;
}

static void serialSend()
{
  String s = kbPeek();
  s.trim();
  if (!s.length()) { showToast("请输入要发送的内容"); return; }
  if (!slActive())
  {
    showToast(slMode() == LINK_BLE ? "BLE 尚未连接" : "TCP 尚未连接");
    return;
  }
  if (flHexMode)
  {
    uint8_t buf[256];
    int n = hexToBytes(s, buf, sizeof(buf));
    if (n <= 0) { showToast("HEX 格式错误，例如 AA 55 01"); return; }
    String bin;
    bin.reserve(n);
    bin.concat((const char *)buf, n);
    if (!slSend(bin)) { showToast("发送失败"); return; }
    kbTake();
    flAddTxRaw(buf, n);
  }
  else
  {
    String out = s;
    if (serialEol) out += "\r\n";
    if (!slSend(out)) { showToast("发送失败"); return; }
    kbTake();
    flAddTx(s);
  }
  dirtyInput = true;
  dirtyFull = true;
}

static void startAnalyze(int idx)
{
  if (idx < 0 || idx >= (int)frames.size()) { showToast("报文已过期"); return; }
  if (!wifiUp()) { showToast("未联网，请先在设置中配置 WiFi"); return; }
  if (otaBusy()) { showToast("固件升级中，请稍候"); return; }
  if (aiBusy()) { showToast("AI 正在回复，请稍候"); return; }
  FrameRec f = frames[idx];
  String p = "请分析这条串口报文。\n";
  p += String("方向: ") + (f.rx ? "设备接收" : "设备发送") + "\n";
  p += "时间: " + flTime(f.ms) + "\n";
  p += "长度: " + String(f.len) + " 字节\n";
  p += "十六进制: " + f.hex + "\n";
  p += "可打印文本: " + (f.text.length() ? f.text : String("无")) + "\n\n";
  p += "请用中文回答: 1) 这可能是哪个协议或哪条指令; 2) 逐字节拆解每个字段的含义; 3) 有无异常或风险; 4) 如果需要回复该指令，建议发送什么内容。";
  std::vector<AiMsg> hist;
  hist.push_back({"user", p});
  anaIdx = idx;
  anaScroll = 0;
  anaErr = "";
  anaLines.clear();
  anaWorking = true;
  aiOwner = 1;
  aiStartEx(hist, apiEndpoint(cfg.url), cfg.key, cfg.model, AI_SYS_ANALYZE);
  gotoScreen(S_AIANA);
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
    kbReset();
    kbAttach(&draft);
    kbSetMask(false);
    kbSetSendLabel("发送");
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
  scanRetry = 0;
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
    WiFi.scanDelete();
    if (scanRetry < 2)
    {
      scanRetry++;
      delay(50);
      WiFi.scanNetworks(true, true);
    }
    else scanPending = false;
  }
}

static void sendToAi()
{
  String text = kbPeek();
  text.trim();
  if (!text.length()) return;
  if (!wifiUp()) { showToast("未联网，请先在设置中配置 WiFi"); return; }
  if (otaBusy()) { showToast("固件升级中，请稍候"); return; }
  if (aiBusy()) { showToast("AI 正在回复，请稍候"); return; }
  kbTake();
  chatAdd(true, text);
  chatScroll = 0;
  dirtyChat = true;
  dirtyInput = true;
  // 历史按总字数从最近往回裁剪，避免一次请求把堆与上行带宽吃满
  std::vector<AiMsg> hist;
  int budget = AI_HIST_CHARS;
  for (int i = (int)msgs.size() - 1; i >= 0; i--)
  {
    ChatMsg &m = msgs[i];
    if (m.err) continue;
    if (!hist.empty() && (int)m.text.length() > budget) break;
    budget -= (int)m.text.length();
    AiMsg am;
    am.role = m.user ? "user" : "assistant";
    am.content = m.text;
    hist.insert(hist.begin(), am);
    if ((int)hist.size() >= 20 || budget <= 0) break;
  }
  aiStart(hist, apiEndpoint(cfg.url), cfg.key, cfg.model);
}

static void serviceAi()
{
  String rep, err;
  if (!aiPoll(rep, err)) return;
  if (aiOwner == 1)
  {
    anaWorking = false;
    if (err.length()) anaErr = err;
    else
    {
      wrapLines(rep, 296, anaLines);
      anaScroll = 0;
    }
    if (scr == S_AIANA) dirtyFull = true;
  }
  else
  {
    if (err.length()) chatAdd(false, "[错误] " + err, true);
    else chatAdd(false, rep);
    chatScroll = 0;
    dirtyChat = true;
  }
  aiOwner = 0;
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
    const char *back = (scr == S_OTAURL || scr == S_OTABLE || scr == S_AIANA) ? "返回" : "主页";
    drawBtn(3, 4, 56, 28, back, COL_KEY_FN, COL_TEXT);
const char *t = (scr == S_SETTINGS) ? "设置" : (scr == S_WIFI) ? "WiFi 设置"
                                     : (scr == S_WIFIPASS)        ? "输入密码"
                                     : (scr == S_XIAOZHI)         ? "小志"
                                     : (scr == S_SERIAL)          ? "串口助手"
                                     : (scr == S_FRAMES)          ? "报文记录"
                                     : (scr == S_AIANA)           ? "AI 报文分析"
                                     : (scr == S_OTA)             ? "固件升级"
                                     : (scr == S_OTAURL)          ? "升级地址"
                                     : (scr == S_OTABLE)          ? "蓝牙推送升级"
                                                                   : "API 设置";
    drawTextAt(70, centerBaseY(0, TITLE_H), t, COL_TEXT, COL_PANEL);
  }
}

static void drawSettingsBody()
{
  struct Row { int y; const char *label; };
  Row rows[6] = {
    {42, "WiFi 设置"},
    {110, "AI 接口设置"},
    {178, "固件升级 (OTA)"},
    {246, "清除聊天记录"},
    {314, "重启设备"},
    {382, "返回聊天"},
  };
  for (auto &r : rows)
  {
    gfx->fillRoundRect(3, r.y, 314, 58, 8, COL_PANEL);
    drawTextAt(14, r.y + 21, r.label, COL_TEXT, COL_PANEL);
    drawTextAt(296, r.y + 21, ">", COL_SUB, COL_PANEL);
  }
  drawTextAt(14, 462, "固件 " FW_VERSION, COL_SUB, COL_BG);
}

static const int OTA_ROW_Y[4] = {124, 180, 236, 292};

static void drawOtaRow(int y, const char *title, const String &sub, const char *badge, bool badgeOn)
{
  gfx->fillRoundRect(3, y, 314, 52, 8, COL_PANEL);
  drawTextAt(12, y + 20, title, COL_TEXT, COL_PANEL);
  drawTextAt(12, y + 40, fitTail(sub, 208), COL_SUB, COL_PANEL);
  if (badge)
    drawBtn(232, y + 11, 82, 30, badge, badgeOn ? COL_ACCENT : COL_KEY_FN, badgeOn ? 0xFFFF : COL_TEXT);
  else
    drawTextAt(296, y + 20, ">", COL_SUB, COL_PANEL);
}

static void drawOtaBody()
{
  gfx->fillRoundRect(3, 42, 314, 76, 8, COL_PANEL);
  drawTextAt(12, 62, "固件版本", COL_SUB, COL_PANEL);
  drawTextAt(92, 62, fitTail(otaVersion(), 214), COL_TEXT, COL_PANEL);
  drawTextAt(12, 84, "运行槽位", COL_SUB, COL_PANEL);
  drawTextAt(92, 84, otaSlot(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 106, "目标分区", COL_SUB, COL_PANEL);
  drawTextAt(92, 106, otaFreeSpace(), COL_SUB, COL_PANEL);
  drawTextAt(190, 106, "堆 " + String(ESP.getFreeHeap() / 1024) + "K", COL_SUB, COL_PANEL);

  drawOtaRow(OTA_ROW_Y[0], "网络推送 OTA", otaArmed() ? "ArduinoOTA 已开启" : "已关闭，右侧可开启",
             otaArmed() ? "关闭" : "开启", otaArmed());
  drawOtaRow(OTA_ROW_Y[1], "URL 地址升级", otaUrl.length() ? otaUrl : String("未设置地址，点击填写"), nullptr, false);
  drawOtaRow(OTA_ROW_Y[2], "TF 卡升级", sdInfo, nullptr, false);
  drawOtaRow(OTA_ROW_Y[3], "蓝牙推送升级", otaBleStatus(), nullptr, false);

  gfx->fillRoundRect(3, 350, 314, 72, 8, COL_PANEL);
  OtaState s = otaState();
  const char *t = "当前状态";
  uint16_t c = COL_SUB;
  if (s == OST_RUNNING) { t = "升级进行中"; c = COL_ACCENT; }
  else if (s == OST_DONE) { t = "升级完成"; c = COL_OK; }
  else if (s == OST_FAIL) { t = "升级失败"; c = COL_ERR_TX; }
  drawTextAt(12, 370, t, c, COL_PANEL);
  if (s == OST_RUNNING) drawTextAt(240, 370, String(otaPercent()) + "%", COL_ACCENT, COL_PANEL);
  String m = (s == OST_IDLE) ? String("空闲") : otaMessage();
  drawTextAt(12, 390, fitTail(m, 296), (s == OST_IDLE) ? COL_SUB : COL_TEXT, COL_PANEL);
  drawProgressBar(12, 400, 296, 14, otaPercent());

  drawTextAt(12, 440, "蓝牙升级：电脑浏览器打开 tools/ota_web/index.html", COL_SUB, COL_BG);
  drawTextAt(12, 460, "TF 卡升级：固件放卡根目录 firmware.bin 后重启", COL_SUB, COL_BG);
}

static void drawOtaBleBody()
{
  gfx->fillRoundRect(3, 42, 314, 84, 8, COL_PANEL);
  drawTextAt(12, 62, "蓝牙名称", COL_SUB, COL_PANEL);
  drawTextAt(92, 62, slDeviceName(), COL_TEXT, COL_PANEL);
  drawTextAt(12, 84, "连接状态", COL_SUB, COL_PANEL);
  drawTextAt(92, 84, slBleConnected() ? "已连接" : "等待连接", slBleConnected() ? COL_OK : COL_SUB, COL_PANEL);
  drawTextAt(12, 106, "校验方式", COL_SUB, COL_PANEL);
  drawTextAt(92, 106, "CRC32 + 长度", COL_TEXT, COL_PANEL);

  gfx->fillRoundRect(3, 132, 314, 100, 8, COL_PANEL);
  drawTextAt(12, 152, fitTail(otaBleStatus(), 296), otaBleRunning() ? COL_ACCENT : COL_TEXT, COL_PANEL);
  drawProgressBar(12, 164, 296, 16, otaPercent());
  drawTextAt(12, 202, String(otaPercent()) + " %", COL_SUB, COL_PANEL);
  drawTextAt(80, 202, otaChannelName(), COL_SUB, COL_PANEL);
  drawTextAt(12, 222, fitTail(otaMessage(), 296), COL_SUB, COL_PANEL);

  gfx->fillRoundRect(3, 240, 314, 150, 8, COL_PANEL);
  drawTextAt(12, 260, "使用步骤", COL_TEXT, COL_PANEL);
  drawTextAt(12, 284, "1 电脑用 Chrome / Edge 打开网页", COL_SUB, COL_PANEL);
  drawTextAt(12, 306, "2 网页目录 tools/ota_web 内执行", COL_SUB, COL_PANEL);
  drawTextAt(12, 328, "  python -m http.server 8000", COL_SUB, COL_PANEL);
  drawTextAt(12, 350, "3 点连接设备，选择 firmware.bin", COL_SUB, COL_PANEL);
  drawTextAt(12, 372, "4 别断开，传完设备自动重启", COL_SUB, COL_PANEL);

  drawBtn(3, 400, 150, 40, "取消升级", otaBleRunning() ? COL_ERR_BG : COL_KEY_FN, otaBleRunning() ? COL_ERR_TX : COL_SUB);
  drawBtn(162, 400, 155, 40, "返回", COL_KEY_FN, COL_TEXT);
}

static void drawOtaUrlBody()
{
  drawTextAt(14, 58, "填写 firmware.bin 的下载地址", COL_SUB, COL_BG);
  gfx->fillRoundRect(3, 70, 314, 40, 8, COL_PANEL);
  String v = otaUrlEdit;
  v += ((millis() / 500) & 1) ? "_" : "";
  drawTextAt(12, centerBaseY(70, 40), fitTail(v, 290), COL_TEXT, COL_PANEL);
  drawTextAt(14, 136, "本机 IP: " + slLocalIp(), COL_SUB, COL_BG);
  drawTextAt(14, 162, "电脑上执行 python -m http.server 8000", COL_SUB, COL_BG);
  drawTextAt(14, 186, "再把电脑 IP 和端口填到上面", COL_SUB, COL_BG);
  drawTextAt(14, 210, "只能刷 firmware.bin，不要刷 merged_flash.bin", COL_SUB, COL_BG);
  drawBtn(3, 246, 100, 44, "返回", COL_KEY_FN, COL_TEXT);
  drawBtn(110, 246, 100, 44, "保存", COL_KEY_FN, COL_TEXT);
  drawBtn(217, 246, 100, 44, "开始升级", COL_ACCENT, 0xFFFF);
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
  drawScrollbar(SCREEN_W - 4, LIST_TOP, viewH, contentH, wifiScroll);
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

static void drawTermIcon(int cx, int cy)
{
  gfx->drawRoundRect(cx - 24, cy - 16, 48, 32, 6, COL_ACCENT);
  gfx->fillRect(cx - 22, cy - 7, 44, 2, COL_ACCENT);
  for (int i = 0; i < 2; i++) gfx->fillRect(cx - 18, cy + 2 + i * 8, 22 - i * 8, 2, COL_ACCENT);
  gfx->fillTriangle(cx + 2, cy - 4, cx + 18, cy + 4, cx + 2, cy + 12, COL_ACCENT);
}

static void drawHomeBody()
{
  struct Tile { int x; const char *label; void (*icon)(int, int); };
  Tile tiles[4] = {
    {6, "聊天", drawChatIcon},
    {86, "串口", drawTermIcon},
    {166, "小志", drawMicIcon},
    {246, "设置", drawGearIcon},
  };
  for (auto &t : tiles)
  {
    gfx->fillRoundRect(t.x, 96, 72, 118, 14, COL_PANEL);
    gfx->drawRoundRect(t.x, 96, 72, 118, 14, COL_LINE);
    t.icon(t.x + 36, 142);
    int tw = textWidth(t.label);
    drawTextAt(t.x + (72 - tw) / 2, 196, t.label, COL_TEXT, COL_PANEL);
  }
  drawTextAt(14, 260, "更多应用敬请期待", COL_SUB, COL_BG);
}

static void drawSerialBody()
{
  bool ble = (slMode() == LINK_BLE);
  bool on = slActive();
  drawBtn(3, 40, 100, 32, "BLE 模式", ble ? COL_ACCENT : COL_KEY_FN, 0xFFFF);
  drawBtn(110, 40, 100, 32, "TCP 模式", ble ? COL_KEY_FN : COL_ACCENT, 0xFFFF);
  drawBtn(217, 40, 100, 32, "清空记录", COL_KEY_FN, COL_TEXT);

  gfx->fillCircle(12, 82, 5, on ? COL_OK : 0xF980);
  drawTextAt(24, 87, fitTail(slStatus(), 280), on ? COL_TEXT : COL_SUB, COL_BG);
  String hint = ble ? "手机用 BLE 串口助手搜索并连接本设备"
                    : String("电脑或手机用 TCP 工具连 ") + slLocalIp() + ":" + String(SL_TCP_PORT);
  drawTextAt(10, 110, fitTail(hint, 300), COL_SUB, COL_BG);

  drawBtn(3, 122, 76, 32, flHexMode ? "HEX 开" : "HEX 关", flHexMode ? COL_ACCENT : COL_KEY_FN, 0xFFFF);
  drawBtn(83, 122, 76, 32, serialEol ? "换行 开" : "换行 关", serialEol ? COL_ACCENT : COL_KEY_FN, 0xFFFF);
  drawBtn(163, 122, 76, 32, "发送", COL_ACCENT, 0xFFFF);
  drawBtn(243, 122, 74, 32, "报文记录", COL_KEY_FN, COL_TEXT);

  int cb = chatViewBot();
  gfx->fillRoundRect(3, TERM_TOP, 314, cb - TERM_TOP, 8, COL_PANEL);
  buildTermLines();
  int viewH = cb - TERM_TOP - 6;
  int maxScroll = max(0, (int)termLines.size() * LINE_H - viewH);
  if (serialScroll > maxScroll) serialScroll = maxScroll;
  if (serialScroll < 0) serialScroll = 0;
  if (!termLines.size())
  {
    drawTextAt((SCREEN_W - textWidth("暂无数据")) / 2, TERM_TOP + 28, "暂无数据", COL_SUB, COL_PANEL);
    return;
  }
  int i0 = max(0, (serialScroll + LINE_H - 1) / LINE_H);
  for (int i = i0; i < (int)termLines.size(); i++)
  {
    int ly = TERM_TOP + 4 + i * LINE_H - serialScroll;
    if (ly + LINE_H > cb - 4) break;
    drawTextAt(10, ly + 17, termLines[i], termTx[i] ? COL_ACCENT : COL_TEXT, COL_PANEL);
  }
  drawScrollbar(SCREEN_W - 5, TERM_TOP + 4, viewH - 8, (int)termLines.size() * LINE_H, serialScroll);
}

static void drawFramesBody()
{
  String head = "共 " + String((int)frames.size()) + " 条记录，点击查看并 AI 分析";
  drawTextAt(12, 58, fitTail(head, 296), COL_SUB, COL_BG);
  int contentH = (int)frames.size() * ITEM_H;
  int viewH = SCREEN_H - 6 - LIST_TOP;
  int maxScroll = max(0, contentH - viewH);
  if (framesScroll > maxScroll) framesScroll = maxScroll;
  for (int i = 0; i < (int)frames.size(); i++)
  {
    int iy = LIST_TOP + i * ITEM_H - framesScroll;
    if (iy + ITEM_H < LIST_TOP || iy > SCREEN_H) continue;
    FrameRec &f = frames[i];
    gfx->fillRoundRect(3, iy, 314, ITEM_H - 6, 8, COL_PANEL);
    String l1 = String(f.rx ? "RX " : "TX ") + flTime(f.ms) + "  " + String(f.len) + " 字节";
    drawTextAt(12, iy + 15, l1, f.rx ? COL_TEXT : COL_ACCENT, COL_PANEL);
    String body = flHexMode ? f.hex : f.text;
    drawTextAt(12, iy + 32, fitTail(body.length() ? body : (flHexMode ? "(空)" : "(无可打印内容)"), 296), COL_SUB, COL_PANEL);
  }
  drawScrollbar(SCREEN_W - 4, LIST_TOP, viewH, contentH, framesScroll);
}

static void drawAnaBody()
{
  if (anaIdx < 0 || anaIdx >= (int)frames.size())
  {
    drawTextAt(12, 120, "报文已过期，请重新选择", COL_SUB, COL_BG);
    drawBtn(3, SCREEN_H - 48, 314, 40, "返回报文记录", COL_KEY_FN, COL_TEXT);
    return;
  }
  FrameRec f = frames[anaIdx];
  gfx->fillRoundRect(3, 42, 314, 76, 8, COL_PANEL);
  drawTextAt(12, 62, String(f.rx ? "接收 RX " : "发送 TX ") + flTime(f.ms) + "  " + String(f.len) + " 字节", COL_SUB, COL_PANEL);
  std::vector<String> hl;
  wrapLines(f.hex.length() ? f.hex : "(空)", 296, hl);
  for (int i = 0; i < (int)hl.size() && i < 3; i++)
    drawTextAt(12, 80 + i * 18, hl[i], COL_TEXT, COL_PANEL);

  gfx->fillRoundRect(3, ANS_TOP, 314, ANS_BOT - ANS_TOP, 8, COL_PANEL);
  if (anaWorking)
  {
    drawTextAt(12, centerBaseY(ANS_TOP, 40), "AI 分析中，请稍候...", COL_ACCENT, COL_PANEL);
  }
  else if (anaErr.length())
  {
    drawTextAt(12, ANS_TOP + 20, fitTail("[错误] " + anaErr, 296), COL_ERR_TX, COL_PANEL);
  }
  else if (!anaLines.size())
  {
    drawTextAt(12, centerBaseY(ANS_TOP, 40), "点击下方按钮开始分析", COL_SUB, COL_PANEL);
  }
  else
  {
    int viewH = ANS_BOT - ANS_TOP - 8;
    int maxScroll = max(0, (int)anaLines.size() * LINE_H - viewH);
    if (anaScroll > maxScroll) anaScroll = maxScroll;
    if (anaScroll < 0) anaScroll = 0;
    int i0 = max(0, (anaScroll + LINE_H - 1) / LINE_H);
    for (int i = i0; i < (int)anaLines.size(); i++)
    {
      int ly = ANS_TOP + 4 + i * LINE_H - anaScroll;
      if (ly + LINE_H > ANS_BOT - 4) break;
      drawTextAt(10, ly + 17, anaLines[i], COL_TEXT, COL_PANEL);
    }
    drawScrollbar(SCREEN_W - 5, ANS_TOP + 4, viewH - 8, (int)anaLines.size() * LINE_H, anaScroll);
  }
  drawBtn(3, SCREEN_H - 48, 150, 40, "重新分析", COL_ACCENT, 0xFFFF);
  drawBtn(162, SCREEN_H - 48, 155, 40, "返回报文记录", COL_KEY_FN, COL_TEXT);
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
  case S_SERIAL:
    drawSerialBody();
    break;
  case S_FRAMES:
    drawFramesBody();
    break;
  case S_AIANA:
    drawAnaBody();
    break;
  case S_OTA:
    drawOtaBody();
    break;
  case S_OTABLE:
    drawOtaBleBody();
    break;
  case S_OTAURL:
    drawOtaUrlBody();
    break;
  case S_CHAT:
    chatRender(CHAT_TOP, chatViewBot() - CHAT_TOP, chatScroll, aiBusy(), lastBusyPhase % 3);
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
  return scr == S_CHAT || scr == S_WIFIPASS || (scr == S_API && editField >= 0) || scr == S_SERIAL || scr == S_OTAURL;
}

static bool inputLineVisible()
{
  return scr == S_CHAT || scr == S_WIFIPASS || scr == S_SERIAL || scr == S_OTAURL;
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
      chatRender(CHAT_TOP, chatViewBot() - CHAT_TOP, chatScroll, aiBusy(), lastBusyPhase % 3);
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
  int cb = chatViewBot();
  if (down && !press && y >= CHAT_TOP && y < cb)
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
      int maxScroll = max(0, contentH - (cb - CHAT_TOP));
      chatScroll = scrollAtDragStart + dy;
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
  if (ptIn(x, y, 3, 42, 314, 58)) { scanStart(); gotoScreen(S_WIFI); return; }
  if (ptIn(x, y, 3, 110, 314, 58)) { gotoScreen(S_API); return; }
  if (ptIn(x, y, 3, 178, 314, 58)) { gotoScreen(S_OTA); return; }
  if (ptIn(x, y, 3, 246, 314, 58)) { dlgClear = true; dirtyFull = true; return; }
  if (ptIn(x, y, 3, 314, 314, 58))
  {
    showToast("正在重启...");
    gfx->flush();
    delay(500);
    ESP.restart();
  }
  if (ptIn(x, y, 3, 382, 314, 58)) { gotoScreen(S_CHAT); }
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
      kbAttach(&passDraft);
      kbReset();
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
      kbReset();
      kbAttach(&draft);
      kbSetMask(false);
      kbSetSendLabel("发送");
      gotoScreen(S_WIFI);
      return;
    }
    if (ptIn(x, y, 227, 48, 82, 40))
    {
      String pw = passDraft;
      pw.trim();
      if (!pw.length()) { showToast("请输入密码"); return; }
      passDraft.clear();
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
    String pw = passDraft;
    pw.trim();
    if (pw.length())
    {
      passDraft.clear();
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

static void handleSerialTouch(bool press, bool release, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_HOME); return; }
    if (ptIn(x, y, 3, 40, 100, 32)) { slSetMode(LINK_BLE); serialScroll = 0; dirtyFull = true; return; }
    if (ptIn(x, y, 110, 40, 100, 32)) { slSetMode(LINK_TCP); serialScroll = 0; dirtyFull = true; return; }
    if (ptIn(x, y, 217, 40, 100, 32)) { flClear(); serialScroll = 0; dirtyFull = true; showToast("记录已清空"); return; }
    if (ptIn(x, y, 3, 122, 76, 32)) { flToggleHex(); dirtyFull = true; return; }
    if (ptIn(x, y, 83, 122, 76, 32)) { serialEol = !serialEol; dirtyFull = true; return; }
    if (ptIn(x, y, 163, 122, 76, 32)) { serialSend(); return; }
    if (ptIn(x, y, 243, 122, 74, 32)) { framesScroll = 0; gotoScreen(S_FRAMES); return; }
  }
  int cb = chatViewBot();
  if (down && !press && y >= TERM_TOP && y < cb)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = serialScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      buildTermLines();
      int viewH = cb - TERM_TOP - 6;
      int maxScroll = max(0, (int)termLines.size() * LINE_H - viewH);
      serialScroll = scrollAtDragStart + dy;
      if (serialScroll < 0) serialScroll = 0;
      if (serialScroll > maxScroll) serialScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyInput = true;
  if (ev & KB_SEND) serialSend();
}

static void handleFramesTouch(bool press, bool release, bool down, int x, int y)
{
  if (press && ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SERIAL); return; }
  if (down && y >= LIST_TOP)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = framesScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int viewH = SCREEN_H - 6 - LIST_TOP;
      int maxScroll = max(0, (int)frames.size() * ITEM_H - viewH);
      framesScroll = scrollAtDragStart - dy;
      if (framesScroll < 0) framesScroll = 0;
      if (framesScroll > maxScroll) framesScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
  if (release && movedDist < 10 && y >= LIST_TOP)
  {
    int idx = (y + framesScroll - LIST_TOP) / ITEM_H;
    if (idx >= 0 && idx < (int)frames.size()) startAnalyze(idx);
  }
}

static void handleAnaTouch(bool press, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_FRAMES); return; }
    if (ptIn(x, y, 3, SCREEN_H - 48, 150, 40)) { startAnalyze(anaIdx); return; }
    if (ptIn(x, y, 162, SCREEN_H - 48, 155, 40)) { gotoScreen(S_FRAMES); return; }
  }
  if (down && !press && y >= ANS_TOP && y < ANS_BOT)
  {
    if (!listDragging) { listDragging = true; dragStartY = y; scrollAtDragStart = anaScroll; }
    int dy = y - dragStartY;
    if (dy)
    {
      int viewH = ANS_BOT - ANS_TOP - 8;
      int maxScroll = max(0, (int)anaLines.size() * LINE_H - viewH);
      anaScroll = scrollAtDragStart - dy;
      if (anaScroll < 0) anaScroll = 0;
      if (anaScroll > maxScroll) anaScroll = maxScroll;
      dirtyFull = true;
    }
  }
  if (!down) listDragging = false;
}

static void handleOtaTouch(bool press, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28)) { gotoScreen(S_SETTINGS); return; }

    // 网络推送开关
    if (ptIn(x, y, 232, OTA_ROW_Y[0] + 11, 82, 30))
    {
      otaSetArmed(!otaArmed());
      showToast(otaArmed() ? "已开启推送接收" : "已关闭推送接收");
      dirtyFull = true;
      return;
    }
    // URL 升级
    if (ptIn(x, y, 3, OTA_ROW_Y[1], 314, 52))
    {
      if (otaBusy()) { showToast("升级进行中"); return; }
      kbAttach(&otaUrlEdit);
      kbSetMask(false);
      kbSetSendLabel("保存");
      kbReset();
      otaUrlEdit = otaUrl;      // kbReset 会清空绑定缓冲，之后重新赋值
      dirtyFull = true;
      gotoScreen(S_OTAURL);
      return;
    }
    // TF 卡升级：点一下重新扫卡，有固件就直接开始
    if (ptIn(x, y, 3, OTA_ROW_Y[2], 314, 52))
    {
      if (otaBusy()) { showToast("升级进行中"); return; }
      refreshSdInfo();
      if (!otaSdFileSize())
      {
        showToast(sdInfo, 2600);
        dirtyFull = true;
        return;
      }
      if (otaSdStartUpdate()) showToast("已开始 TF 卡升级", 1600);
      else showToast(otaSdStatus(), 2600);
      dirtyFull = true;
      return;
    }
    // 蓝牙推送
    if (ptIn(x, y, 3, OTA_ROW_Y[3], 314, 52))
    {
      dirtyFull = true;
      gotoScreen(S_OTABLE);
      return;
    }
  }
  if (otaBusy() && otaState() == OST_RUNNING)
  {
    uint16_t ph = (uint16_t)((millis() / 400) % 4);
    if (ph != lastOtaPhase) { lastOtaPhase = (uint8_t)ph; dirtyFull = true; }
  }
}

static void handleOtaBleTouch(bool press, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28) || ptIn(x, y, 162, 400, 155, 40))
    {
      dirtyFull = true;
      gotoScreen(S_OTA);
      return;
    }
    if (ptIn(x, y, 3, 400, 150, 40))
    {
      if (otaBleRunning())
      {
        otaBleCancel();
        showToast("已取消蓝牙升级");
        dirtyFull = true;
      }
      else showToast("当前没有蓝牙升级任务");
      return;
    }
  }
  if (otaBleRunning())
  {
    uint16_t ph = (uint16_t)((millis() / 400) % 4);
    if (ph != lastOtaPhase) { lastOtaPhase = (uint8_t)ph; dirtyFull = true; }
  }
}

static void leaveOtaUrl()
{
  // 先重置键盘（会清空当前绑定的编辑缓冲），再绑回聊天草稿，避免清掉用户输入
  kbReset();
  kbAttach(&draft);
  kbSetMask(false);
  kbSetSendLabel("发送");
  dirtyFull = true;
}

static void otaUrlCommit(bool alsoStart)
{
  otaUrlEdit.trim();
  otaUrl = otaUrlEdit;
  cfg.otaUrl = otaUrl;
  cfgSaveOtaUrl();
  leaveOtaUrl();
  gotoScreen(S_OTA);
  if (!alsoStart)
  {
    showToast("地址已保存");
    return;
  }
  if (!wifiUp())
  {
    showToast("未联网，请先配置 WiFi", 2600);
    return;
  }
  if (otaWebStart(otaUrl)) showToast("已开始 URL 升级", 1500);
  else showToast(otaMessage().length() ? otaMessage() : "无法开始升级", 2600);
  dirtyFull = true;
}

static void handleOtaUrlTouch(bool press, bool down, int x, int y)
{
  if (press)
  {
    if (ptIn(x, y, 3, 4, 56, 28) || ptIn(x, y, 3, 246, 100, 44))
    {
      leaveOtaUrl();      // 取消：丢弃这次编辑
      gotoScreen(S_OTA);
      return;
    }
    if (ptIn(x, y, 110, 246, 100, 44))
    {
      otaUrlCommit(false);
      return;
    }
    if (ptIn(x, y, 217, 246, 100, 44))
    {
      otaUrlCommit(true);
      return;
    }
  }
  uint8_t ev = kbHandle(down, x, y);
  if (ev & KB_LAYOUT) { dirtyFull = true; return; }
  if (ev & KB_CHANGED) dirtyFull = true;
  if (ev & KB_SEND) otaUrlCommit(false);
}

static void handleHomeTouch(bool press, int x, int y)
{
  if (!press) return;
  if (ptIn(x, y, 6, 96, 72, 118)) { gotoScreen(S_CHAT); return; }
  if (ptIn(x, y, 86, 96, 72, 118)) { serialScroll = 0; gotoScreen(S_SERIAL); return; }
  if (ptIn(x, y, 166, 96, 72, 118)) { gotoScreen(S_XIAOZHI); return; }
  if (ptIn(x, y, 246, 96, 72, 118)) { gotoScreen(S_SETTINGS); return; }
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
  case S_SERIAL: handleSerialTouch(press, release, down, x, y); break;
  case S_FRAMES: handleFramesTouch(press, release, down, x, y); break;
  case S_AIANA: handleAnaTouch(press, down, x, y); break;
  case S_OTA: handleOtaTouch(press, down, x, y); break;
  case S_OTAURL: handleOtaUrlTouch(press, down, x, y); break;
  case S_OTABLE: handleOtaBleTouch(press, down, x, y); break;
  case S_SETTINGS: handleSettingsTouch(press, x, y); break;
  case S_WIFI: handleWifiTouch(press, release, down, x, y); break;
  case S_WIFIPASS: handleWifiPassTouch(press, down, x, y); break;
  case S_API: handleApiTouch(press, down, x, y); break;
  }
}

void setup()
{
  Serial0.begin(115200);
  Serial.begin(115200);
  delay(300);
  BOOTLOG("\n[boot] setup enter\n");
  cfgLoad();
  otaUrl = cfg.otaUrl;
  BOOTLOG("[boot] stage: cfg ok\n");
  ui_init();
  BOOTLOG("[boot] stage: ui ok\n");
  touchInit();
  // TF 卡升级：开机检测卡根目录固件；升级成功会直接重启，不返回
  otaSdBootCheck();
  aiBegin();
  flBegin();
  BOOTLOG("[boot] stage: ai ok\n");
  slBegin("ESP32-S3-AI");
  otaBleBegin();
  otaBegin();
  BOOTLOG("[boot] stage: ble ota ok\n");
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
  slPoll();
  otaBlePoll();
  otaSdPoll();
  otaPoll();

  uint8_t os = (uint8_t)otaState();
  int op = otaPercent();
  if (os != lastOtaState || op != lastOtaPct)
  {
    lastOtaState = os;
    lastOtaPct = op;
    if (scr == S_OTA || scr == S_OTABLE) dirtyFull = true;
  }

  if (frames.size() != lastFrameCount)
  {
    lastFrameCount = frames.size();
    if (scr == S_SERIAL || scr == S_FRAMES) dirtyFull = true;
  }
  if (scr == S_SERIAL)
  {
    String st = slStatus();
    if (st != lastSlStatus)
    {
      lastSlStatus = st;
      dirtyFull = true;
    }
  }

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
    if (cfg.ssid.length() && !wifiUp() && !wcConnecting && !scanPending)
      WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());
  }

  compose();
}

// 备注：本文件是唯一需要手改的源码入口；esp32_ai_chat.ino.cpp 由 PlatformIO 生成，勿手改。
