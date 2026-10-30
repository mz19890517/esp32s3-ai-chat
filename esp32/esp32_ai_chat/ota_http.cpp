#include "ota_http.h"
#include "ota_update.h"
#include "cfg_table.h"
#include "log_tee.h"

#include <WiFi.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define HDR_MAX 2048
#define BUF_MAX 4096

static WiFiServer *srv = nullptr;
static volatile bool writing = false;
static bool bound = false;
static String lastInfo;

static const char *reason(int code)
{
  switch (code)
  {
  case 200: return "OK";
  case 204: return "No Content";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 404: return "Not Found";
  case 409: return "Conflict";
  default: return "Error";
  }
}

static void reply(WiFiClient &c, int code, const char *body)
{
  c.printf("HTTP/1.1 %d %s\r\n", code, reason(code));
  c.print("Access-Control-Allow-Origin: *\r\n");
  c.print("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
  c.print("Access-Control-Allow-Headers: Content-Type, X-Pin\r\n");
  // Chrome 的私有网络访问（localhost 页面访问内网 IP）需要这个响应头
  c.print("Access-Control-Allow-Private-Network: true\r\n");
  c.print("Access-Control-Max-Age: 86400\r\n");
  c.print("Content-Type: application/json; charset=utf-8\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n");
  c.printf("Content-Length: %u\r\n\r\n", (unsigned)(body ? strlen(body) : 0));
  if (body && code != 204) c.print(body);
  c.flush();
}

static void replyText(WiFiClient &c, const char *body)
{
  c.printf("HTTP/1.1 200 OK\r\n");
  c.print("Access-Control-Allow-Origin: *\r\n");
  c.print("Access-Control-Allow-Private-Network: true\r\n");
  c.print("Content-Type: text/plain; charset=utf-8\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.print("Connection: close\r\n");
  c.printf("Content-Length: %u\r\n\r\n", (unsigned)(body ? strlen(body) : 0));
  if (body) c.print(body);
  c.flush();
}

static bool readHeaders(WiFiClient &c, String &head)
{
  head = "";
  head.reserve(HDR_MAX);
  uint32_t t0 = millis();
  while (millis() - t0 < 5000)
  {
    while (c.available() > 0)
    {
      int v = c.read();
      if (v < 0) break;
      head += (char)v;
      if (head.endsWith("\r\n\r\n")) return true;
      if ((int)head.length() >= HDR_MAX) return false;
    }
    if (!c.connected() && c.available() <= 0) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return false;
}

static int headerInt(const String &head, const char *name)
{
  String low = head;
  low.toLowerCase();
  String key = String(name);
  key.toLowerCase();
  int p = low.indexOf(key + ":");
  if (p < 0) return -1;
  int e = head.indexOf("\r\n", p);
  String v = head.substring(p + key.length() + 1, e < 0 ? head.length() : e);
  v.trim();
  return v.toInt();
}

static String headerStr(const String &head, const char *name)
{
  String low = head;
  low.toLowerCase();
  String key = String(name);
  key.toLowerCase();
  int p = low.indexOf(key + ":");
  if (p < 0) return String();
  int e = head.indexOf("\r\n", p);
  String v = head.substring(p + key.length() + 1, e < 0 ? head.length() : e);
  v.trim();
  return v;
}

// ---- 配置接口（/api/*）辅助 ----

static bool pinOk(const String &head)
{
  if (!cfgWebPinRequired()) return true;
  String p = headerStr(head, "X-Pin");
  char want[8];
  snprintf(want, sizeof(want), "%04u", (unsigned)cfgPin());
  return p == want;
}

static int hexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static String urlDecode(const String &s)
{
  String o;
  o.reserve(s.length());
  for (int i = 0; i < s.length(); i++)
  {
    char c = s[i];
    if (c == '+') o += ' ';
    else if (c == '%' && i + 2 < s.length())
    {
      int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0)
      {
        o += (char)((hi << 4) | lo);
        i += 2;
      }
      else o += c;
    }
    else o += c;
  }
  return o;
}

static String formParam(const String &body, const char *name)
{
  int i = 0;
  int n = body.length();
  while (i < n)
  {
    int amp = body.indexOf('&', i);
    if (amp < 0) amp = n;
    int eq = body.indexOf('=', i);
    if (eq > i && eq < amp && body.substring(i, eq) == name)
      return urlDecode(body.substring(eq + 1, amp));
    i = amp + 1;
  }
  return String();
}

static bool readBody(WiFiClient &c, int len, String &body)
{
  body = "";
  if (len > 2048) return false;
  body.reserve(len);
  uint32_t t0 = millis();
  while ((int)body.length() < len && millis() - t0 < 5000)
  {
    while (c.available() > 0 && (int)body.length() < len)
    {
      int v = c.read();
      if (v < 0) break;
      body += (char)v;
    }
    if (!c.connected() && c.available() <= 0) break;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return (int)body.length() >= len;
}

static void handleSettingsPost(WiFiClient &c, const String &head)
{
  if (!pinOk(head))
  {
    reply(c, 401, "{\"err\":\"需要 PIN\"}");
    return;
  }
  int cl = headerInt(head, "Content-Length");
  if (cl <= 0)
  {
    reply(c, 400, "{\"err\":\"no content-length\"}");
    return;
  }
  String body;
  if (!readBody(c, cl, body))
  {
    reply(c, 400, "{\"err\":\"body 不完整\"}");
    return;
  }
  String k = formParam(body, "k");
  String v = formParam(body, "v");
  if (!k.length())
  {
    reply(c, 400, "{\"err\":\"缺少 k\"}");
    return;
  }
  for (int i = 0; i < cfgCount(); i++)
  {
    const CfgItem &it = cfgAt(i);
    if (k == it.key)
    {
      String err;
      if (cfgSet(it, v, err))
      {
        String out = "{\"ok\":true,\"v\":\"" + cfgJsonEsc(cfgGetShow(it)) + "\"}";
        reply(c, 200, out.c_str());
      }
      else
      {
        String out = "{\"err\":\"" + cfgJsonEsc(err) + "\"}";
        reply(c, 400, out.c_str());
      }
      return;
    }
  }
  reply(c, 404, "{\"err\":\"未知的配置项\"}");
}

static void handlePost(WiFiClient &c, const String &head)
{
  int cl = headerInt(head, "Content-Length");
  if (cl <= 0)
  {
    reply(c, 400, "{\"err\":\"no content-length\"}");
    return;
  }
  if (otaBusy() || otaBleActive() || writing)
  {
    reply(c, 409, "{\"err\":\"busy\"}");
    return;
  }
  if ((size_t)cl < 65536 || (size_t)cl > otaTargetSize())
  {
    reply(c, 400, "{\"err\":\"bad size\"}");
    return;
  }
  if (!Update.begin((size_t)cl, U_FLASH))
  {
    reply(c, 400, "{\"err\":\"begin failed\"}");
    return;
  }

  uint8_t *buf = (uint8_t *)malloc(BUF_MAX);
  if (!buf)
  {
    Update.abort();
    reply(c, 400, "{\"err\":\"no mem\"}");
    return;
  }

  writing = true;
  otaReport(OST_RUNNING, "WiFi 推送升级", 0);
  logTeePrintf("[http] ota start %d bytes\n", cl);

  size_t got = 0;
  int lastPct = -1;
  uint32_t lastData = millis();
  while (got < (size_t)cl)
  {
    int avail = c.available();
    if (avail <= 0)
    {
      if (!c.connected()) break;
      if (millis() - lastData > 15000) break;
      vTaskDelay(pdMS_TO_TICKS(4));
      continue;
    }
    size_t want = (size_t)avail;
    if (want > BUF_MAX) want = BUF_MAX;
    int n = c.read(buf, want);
    if (n <= 0)
    {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    lastData = millis();
    if (Update.write(buf, (size_t)n) != (size_t)n) break;
    got += (size_t)n;
    int p = (int)((uint64_t)got * 100 / (uint64_t)cl);
    if (p != lastPct)
    {
      lastPct = p;
      otaReport(OST_RUNNING, "WiFi 写入 " + String((unsigned)(got / 1024)) + "/" +
                                 String((unsigned)((size_t)cl / 1024)) + " KB", p);
    }
  }
  free(buf);

  if (got != (size_t)cl || !Update.end(true) || !Update.isFinished())
  {
    Update.abort();
    writing = false;
    otaReport(OST_FAIL, "WiFi 推送失败", 0);
    logTeePrintf("[http] ota fail got=%u/%d\n", (unsigned)got, cl);
    reply(c, 400, "{\"err\":\"write failed\"}");
    return;
  }

  writing = false;
  otaReport(OST_DONE, "WiFi 推送升级完成，即将重启", 100);
  logTeePrintf("[http] ota done %u bytes\n", (unsigned)got);
  reply(c, 200, "{\"ok\":true}");
  otaRequestRestart(1500);
}

static void handleClient(WiFiClient &c)
{
  String head;
  if (!readHeaders(c, head))
  {
    reply(c, 400, "{\"err\":\"bad request\"}");
    return;
  }
  if (head.startsWith("OPTIONS"))
  {
    reply(c, 204, nullptr);
  }
  else if (head.startsWith("GET /info"))
  {
    reply(c, 200, "{\"ok\":true,\"ver\":\"" FW_VERSION "\",\"port\":8080}");
  }
  else if (head.startsWith("POST /ota"))
  {
    handlePost(c, head);
  }
  // ---- 网页控制台 API（P1）----
  // caps 不做 PIN 校验：网页要先从它拿到 pinreq 才知道要不要输 PIN，
  // 且它只暴露固件版本与能力列表，不泄露配置。
  else if (head.startsWith("GET /api/caps"))
  {
    String out = "{\"api\":1,\"fw\":\"" FW_VERSION "\",\"caps\":[\"settings\",\"status\"],\"pinreq\":";
    out += cfgWebPinRequired() ? "true" : "false";
    out += "}";
    reply(c, 200, out.c_str());
  }
  else if (head.startsWith("GET /api/settings"))
  {
    if (!pinOk(head)) { reply(c, 401, "{\"err\":\"需要 PIN\"}"); return; }
    String out;
    cfgBuildSettingsJson(out);
    reply(c, 200, out.c_str());
  }
  else if (head.startsWith("POST /api/settings"))
  {
    handleSettingsPost(c, head);
  }
  else if (head.startsWith("GET /api/status"))
  {
    if (!pinOk(head)) { reply(c, 401, "{\"err\":\"需要 PIN\"}"); return; }
    String out;
    cfgBuildStatusJson(out);
    reply(c, 200, out.c_str());
  }
  else if (head.startsWith("GET /api/logs"))
  {
    // 网页日志：浏览器直接开 http://设备IP:8080/api/logs；开关 = 设置→配置项→调试「网页日志」
    if (!cfgFlag("log.http")) { reply(c, 404, "{\"err\":\"网页日志已关闭\"}"); return; }
    if (!pinOk(head)) { reply(c, 401, "{\"err\":\"需要 PIN\"}"); return; }
    if (head.indexOf("clear=1") >= 0)
    {
      logTeeClear();
      replyText(c, "(已清空)\n");
      return;
    }
    String out = logTeeDump(24000);
    if (!out.length()) out = "(暂无日志)\n";
    replyText(c, out.c_str());
  }
  else
  {
    reply(c, 400, "{\"err\":\"unknown path\"}");
  }
}

static void httpTask(void *)
{
  for (;;)
  {
    bool up = (WiFi.status() == WL_CONNECTED);
    if (up && !bound)
    {
      srv->begin();
      srv->setNoDelay(true);
      bound = true;
      logTeePrintf("[http] ota server http://%s:%u/ota\n",
                    WiFi.localIP().toString().c_str(), (unsigned)OTA_HTTP_PORT);
      // ArduinoOTA 在 WiFi 连上之前就调过 MDNS.begin()，那时可能没网卡而失败；
      // 这里补一次，保证 esp32-ai-s3.local 可解析（同名，重复调用是幂等的）
      static bool mdnsTried = false;
      if (!mdnsTried)
      {
        mdnsTried = true;
        if (MDNS.begin("esp32-ai-s3"))
        {
          MDNS.addService("http", "tcp", OTA_HTTP_PORT);
          logTeePrintf("[mdns] http://esp32-ai-s3.local:%u/ota\n", (unsigned)OTA_HTTP_PORT);
        }
        else
        {
          logTeePrintf("[mdns] begin failed（用 IP 访问即可）\n");
        }
      }
    }
    else if (!up && bound)
    {
      srv->end();
      bound = false;
    }

    if (bound)
    {
      WiFiClient c = srv->accept();
      if (c)
      {
        handleClient(c);
        c.stop();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void otaHttpBegin()
{
  if (srv) return;
  srv = new WiFiServer(OTA_HTTP_PORT);
  srv->begin();
  BaseType_t tok = xTaskCreate(httpTask, "otahttp", 8192, nullptr, 2, nullptr);
  if (tok != pdPASS)
    logTeePrintf("[http] task create FAILED（内部内存不足）\n");
  else
    logTeePrintf("[http] task started, port %u\n", (unsigned)OTA_HTTP_PORT);
}

bool otaHttpWriting() { return writing; }

String otaHttpUrl()
{
  if (WiFi.status() != WL_CONNECTED) return String();
  return "http://" + WiFi.localIP().toString() + ":" + String(OTA_HTTP_PORT) + "/ota";
}

void otaHttpPoll()
{
  String u = otaHttpUrl();
  if (u.length() && u != lastInfo)
  {
    lastInfo = u;
    otaBleSetInfo(u);   // 网页连上蓝牙后读这个特征，自动填 IP
  }
}
