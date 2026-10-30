#include "ai_client.h"
#include "log_tee.h"
#include "serial_link.h"
#include "ota_update.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *SYSTEM_PROMPT = "你是一个运行在ESP32设备上的AI助手。请用简洁准确的中文回答。";
static const int MAX_HIST = 20;
static const size_t MAX_REPLY = 8000;

static SemaphoreHandle_t mtx;
static SemaphoreHandle_t sem;
static std::vector<AiMsg> reqHist;
static String reqUrl, reqKey, reqModel, reqSys;
static volatile bool busy = false;
static volatile bool done = false;
static String resultReply, resultErr;

bool aiBusy() { return busy; }

static void doRequest(const std::vector<AiMsg> &hist, const String &url, const String &key, const String &model, const String &sysPrompt, String &reply, String &err)
{
  JsonDocument doc;
  doc["model"] = model;
  doc["stream"] = false;
  JsonArray arr = doc["messages"].to<JsonArray>();
  JsonObject sys = arr.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = sysPrompt.length() ? sysPrompt : SYSTEM_PROMPT;
  for (auto &m : hist)
  {
    JsonObject o = arr.add<JsonObject>();
    o["role"] = m.role;
    o["content"] = m.content;
  }
  String body;
  serializeJson(doc, body);

  // TLS 握手峰值内存大，BLE 常驻时必然失败：先临时释放 BLE，退出路径由 BleGuard 恢复
  struct BleGuard
  {
    bool down = false;
    ~BleGuard()
    {
      if (!down) return;
      slBleResume();
      otaBleBegin();
      slStartAdv();
      logTeePrintf("[ai] ble resumed (chat)\n");
    }
  } bg;
  if (url.startsWith("https") && !otaBleActive())
  {
    slBleSuspend();
    bg.down = true;
  }

  WiFiClientSecure scl;
  WiFiClient pcl;
  bool tls = url.startsWith("https");
  WiFiClient &cli = tls ? static_cast<WiFiClient &>(scl) : static_cast<WiFiClient &>(pcl);
  if (tls)
  {
    scl.setInsecure();
    scl.setHandshakeTimeout(15);
  }
  HTTPClient http;
  http.setTimeout(90000);
  if (!http.begin(cli, url))
  {
    err = "无法连接到服务器";
    return;
  }
  http.addHeader("Content-Type", "application/json");
  if (key.length()) http.addHeader("Authorization", "Bearer " + key);
  int code = http.POST(body);
  if (code <= 0)
  {
    err = "网络错误: " + http.errorToString(code);
  }
  else
  {
    String resp = http.getString();
    if (code == 200)
    {
      JsonDocument rd;
      DeserializationError e = deserializeJson(rd, resp);
      if (e) { err = String("解析失败: ") + e.c_str(); }
      else
      {
        const char *c = rd["choices"][0]["message"]["content"] | "";
        reply = String(c);
        reply.trim();
        if (!reply.length()) err = "AI 返回了空回复";
        if (reply.length() > MAX_REPLY) reply = reply.substring(0, MAX_REPLY);
      }
    }
    else
    {
      String msg;
      JsonDocument rd;
      if (!deserializeJson(rd, resp))
      {
        const char *emsg = rd["error"]["message"] | "";
        msg = String(emsg);
      }
      if (msg.length()) err = "HTTP " + String(code) + ": " + msg;
      else err = "HTTP " + String(code);
      if (err.length() > 300) err = err.substring(0, 300);
    }
  }
  http.end();
}

static bool aiFetchModelsImpl(const String &baseUrl, const String &apiKey, String &models, String &err);

// 请求种类：0=聊天，1=拉取模型列表（共用同一个 16KB 栈任务，不额外占内存）
static int reqKind = 0;
static String resultModels;
static bool resultOk = false;

static void taskFn(void *)
{
  for (;;)
  {
    xSemaphoreTake(sem, portMAX_DELAY);
    std::vector<AiMsg> hist;
    String url, key, model, sysp;
    int kind;
    xSemaphoreTake(mtx, portMAX_DELAY);
    hist = reqHist;
    url = reqUrl;
    key = reqKey;
    model = reqModel;
    sysp = reqSys;
    kind = reqKind;
    xSemaphoreGive(mtx);

    String reply, err, models;
    bool ok = true;
    if (kind == 1)
      ok = aiFetchModelsImpl(url, key, models, err);
    else
      doRequest(hist, url, key, model, sysp, reply, err);

    xSemaphoreTake(mtx, portMAX_DELAY);
    resultReply = reply;
    resultErr = err;
    resultModels = models;
    resultOk = ok;
    done = true;
    busy = false;
    xSemaphoreGive(mtx);
  }
}

void aiBegin()
{
  mtx = xSemaphoreCreateMutex();
  sem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(taskFn, "ai", 16384, nullptr, 1, nullptr, 0);
}

void aiStart(const std::vector<AiMsg> &history, const String &url, const String &key, const String &model)
{
  aiStartEx(history, url, key, model, "");
}

void aiStartEx(const std::vector<AiMsg> &history, const String &url, const String &key, const String &model, const String &sysPrompt)
{
  xSemaphoreTake(mtx, portMAX_DELAY);
  reqHist = history;
  reqUrl = url;
  reqKey = key;
  reqModel = model;
  reqSys = sysPrompt;
  done = false;
  busy = true;
  xSemaphoreGive(mtx);
  xSemaphoreGive(sem);
}

bool aiPoll(String &reply, String &err)
{
  if (!done) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!done)
  {
    xSemaphoreGive(mtx);
    return false;
  }
  reply = resultReply;
  err = resultErr;
  done = false;
  xSemaphoreGive(mtx);
  return true;
}

// 实际拉取逻辑（必须在足够大的任务栈里跑：TLS 握手吃栈，loop 栈 8KB 会握不上手）
static bool aiFetchModelsImpl(const String &baseUrl, const String &apiKey, String &models, String &err)
{
  models = "";
  String u = baseUrl;
  u.trim();
  while (u.length() && u.endsWith("/")) u.remove(u.length() - 1);
  if (!u.length()) { err = "地址为空"; return false; }

  // 先单独诊断 DNS：-1 连接失败时先分清是“解析不到域名”还是“TLS 握手失败”
  String host = u;
  host.remove(0, host.indexOf("//") + 2);
  int sp = host.indexOf('/');
  if (sp >= 0) host.remove(sp);
  int cp = host.indexOf(':');
  if (cp >= 0) host.remove(cp);
  IPAddress rip;
  unsigned long t0 = millis();
  int dnsr = WiFi.hostByName(host.c_str(), rip);
  logTeePrintf("[ai] dns %s -> %s (r=%d, %lums)\n", host.c_str(),
               rip.toString().c_str(), dnsr, (unsigned long)(millis() - t0));
  if (dnsr != 1)
  {
    err = "域名解析失败（检查设备能否上网）";
    return false;
  }

  // TLS 握手峰值要 ~40KB 内部内存，BLE 常驻时最大空闲块只有 ~16KB，握手必然 -1。
  // 所以先临时释放 BLE（网页蓝牙会断开，几秒后自动恢复），所有退出路径由 BleGuard 恢复。
  struct BleGuard
  {
    bool down = false;
    ~BleGuard()
    {
      if (!down) return;
      slBleResume();
      otaBleBegin();   // 新 BLE 栈上重新注册蓝牙 OTA 服务
      slStartAdv();
      logTeePrintf("[ai] ble resumed\n");
    }
  } bg;

  if (u.startsWith("https"))
  {
    if (otaBleActive()) { err = "蓝牙升级进行中，请稍后再试"; return false; }
    slBleSuspend();
    bg.down = true;
    logTeePrintf("[ai] ble dropped, largest=%u\n",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }

  WiFiClientSecure scl;
  WiFiClient pcl;
  bool tls = u.startsWith("https");
  WiFiClient &cli = tls ? static_cast<WiFiClient &>(scl) : static_cast<WiFiClient &>(pcl);
  if (tls)
  {
    scl.setInsecure();
    scl.setHandshakeTimeout(15);
  }
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(cli, u + "/models"))
  {
    err = "地址无效";
    return false;
  }
  if (apiKey.length()) http.addHeader("Authorization", "Bearer " + apiKey);
  logTeePrintf("[ai] pre-GET heap=%u largest=%u\n",
               (unsigned)ESP.getFreeHeap(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  int code = http.GET();
  logTeePrintf("[ai] models GET %s -> %d\n", u.c_str(), code);
  if (code <= 0)
  {
    err = "网络错误: " + http.errorToString(code);
    http.end();
    return false;
  }
  if (code != 200)
  {
    err = "HTTP " + String(code) + (code == 401 ? "（Key 不对？）" : "");
    http.end();
    return false;
  }
  String resp = http.getString();
  http.end();
  if (resp.length() > 120) logTeePrintf("[ai] models resp head: %.120s\n", resp.c_str());

  JsonDocument rd;
  DeserializationError e = deserializeJson(rd, resp);
  if (e)
  {
    err = "响应解析失败";
    return false;
  }
  int cnt = 0;
  const char *paths[] = {"data", "models", "model"};
  for (const char *path : paths)
  {
    JsonArray arr = rd[path].as<JsonArray>();
    if (arr.isNull()) continue;
    for (JsonVariant m : arr)
    {
      String id;
      if (m.is<JsonObject>()) id = m["id"] | "";
      else if (m.is<const char *>()) id = m.as<const char *>();
      if (!id.length()) continue;
      if (models.length()) models += "\n";
      models += id;
      if (++cnt >= 24 || models.length() > 1500) break;   // 控制体积（NVS blob 有限）
    }
    if (cnt) break;
  }
  if (!cnt)
  {
    err = "未找到模型列表";
    return false;
  }
  return true;
}

// —— 模型拉取：复用 AI 任务（16KB 栈），loop 投递请求并等待结果 ——
bool aiFetchModels(const String &baseUrl, const String &apiKey, String &models, String &err)
{
  if (!sem || !mtx)
  {
    err = "内部错误：未初始化";
    return false;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (busy)
  {
    xSemaphoreGive(mtx);
    err = "AI 正忙，稍后再试";
    return false;
  }
  reqHist.clear();
  reqSys = "";
  reqUrl = baseUrl;
  reqKey = apiKey;
  reqModel = "";
  reqKind = 1;
  done = false;
  busy = true;
  xSemaphoreGive(mtx);
  xSemaphoreGive(sem);

  uint32_t t0 = millis();
  bool d = false;
  while (millis() - t0 < 40000)
  {
    vTaskDelay(pdMS_TO_TICKS(60));
    xSemaphoreTake(mtx, portMAX_DELAY);
    d = done;
    xSemaphoreGive(mtx);
    if (d) break;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool ok = d && resultOk;
  models = resultModels;
  err = d ? resultErr : String("等待超时");
  done = false;   // 结果已消费
  xSemaphoreGive(mtx);
  return ok;
}
