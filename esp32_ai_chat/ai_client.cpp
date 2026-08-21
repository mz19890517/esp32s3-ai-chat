#include "ai_client.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *SYSTEM_PROMPT = "你是一个运行在ESP32设备上的AI助手。请用简洁准确的中文回答。";
static const int MAX_HIST = 20;
static const size_t MAX_REPLY = 8000;

static SemaphoreHandle_t mtx;
static SemaphoreHandle_t sem;
static std::vector<AiMsg> reqHist;
static String reqUrl, reqKey, reqModel;
static volatile bool busy = false;
static volatile bool done = false;
static String resultReply, resultErr;

bool aiBusy() { return busy; }

static void doRequest(const std::vector<AiMsg> &hist, const String &url, const String &key, const String &model, String &reply, String &err)
{
  JsonDocument doc;
  doc["model"] = model;
  doc["stream"] = false;
  JsonArray arr = doc["messages"].to<JsonArray>();
  JsonObject sys = arr.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = SYSTEM_PROMPT;
  for (auto &m : hist)
  {
    JsonObject o = arr.add<JsonObject>();
    o["role"] = m.role;
    o["content"] = m.content;
  }
  String body;
  serializeJson(doc, body);

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

static void taskFn(void *)
{
  for (;;)
  {
    xSemaphoreTake(sem, portMAX_DELAY);
    std::vector<AiMsg> hist;
    String url, key, model;
    xSemaphoreTake(mtx, portMAX_DELAY);
    hist = reqHist;
    url = reqUrl;
    key = reqKey;
    model = reqModel;
    xSemaphoreGive(mtx);

    String reply, err;
    doRequest(hist, url, key, model, reply, err);

    xSemaphoreTake(mtx, portMAX_DELAY);
    resultReply = reply;
    resultErr = err;
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
  xSemaphoreTake(mtx, portMAX_DELAY);
  reqHist = history;
  reqUrl = url;
  reqKey = key;
  reqModel = model;
  done = false;
  busy = true;
  xSemaphoreGive(mtx);
  xSemaphoreGive(sem);
}

bool aiPoll(String &reply, String &err)
{
  if (!done) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  reply = resultReply;
  err = resultErr;
  done = false;
  xSemaphoreGive(mtx);
  return true;
}
