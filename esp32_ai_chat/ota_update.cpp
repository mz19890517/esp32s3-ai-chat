#include "ota_update.h"
#include "serial_link.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static OtaState st = OST_IDLE;
static volatile int pct = 0;
static String msg;
static SemaphoreHandle_t mtx;
static bool armed = true;
static volatile bool webBusy = false;
static volatile bool restartPending = false;
static uint32_t restartAt = 0;
static TaskHandle_t webTaskHandle = nullptr;
static String webUrlTmp;

static void setState(OtaState s, const String &m, int p = 0)
{
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  st = s;
  msg = m;
  pct = p;
  if (mtx) xSemaphoreGive(mtx);
}

OtaState otaState()
{
  OtaState s;
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  s = st;
  if (mtx) xSemaphoreGive(mtx);
  return s;
}

int otaPercent()
{
  return pct;
}

String otaMessage()
{
  String m;
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  m = msg;
  if (mtx) xSemaphoreGive(mtx);
  return m;
}

bool otaArmed() { return armed; }
void otaSetArmed(bool a) { armed = a; }

bool otaBusy() { return otaState() == OST_RUNNING; }
bool otaRestartPending() { return restartPending; }

String otaVersion()
{
  String v = String(FW_VERSION);
  const esp_app_desc_t *d = esp_ota_get_app_description();
  if (d && d->version) v += String("  ") + d->version;
  return v;
}

String otaSlot()
{
  const esp_partition_t *p = esp_ota_get_running_partition();
  const esp_partition_t *n = esp_ota_get_next_update_partition(nullptr);
  String s;
  if (p) s = (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? String("ota_0") : String("ota_1");
  s += "  ->  ";
  if (n) s += (n->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ? String("ota_0") : String("ota_1");
  return s;
}

String otaFreeSpace()
{
  const esp_partition_t *n = esp_ota_get_next_update_partition(nullptr);
  if (!n) return "-";
  float mb = n->size / 1048576.0f;
  return String(mb, 1) + " MB";
}

static void webFail(const String &m)
{
  Update.abort();
  setState(OST_FAIL, m);
  webBusy = false;
  slOtaResume();
}

static void webTask(void *)
{
  String url;
  xSemaphoreTake(mtx, portMAX_DELAY);
  url = webUrlTmp;
  xSemaphoreGive(mtx);

  slOtaSuspend();
  setState(OST_RUNNING, "连接升级服务器", 0);

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
  http.setTimeout(60000);
  if (!http.begin(cli, url))
  {
    webFail("无法连接升级服务器");
    vTaskDelete(nullptr);
    return;
  }

  int code = http.GET();
  if (code != 200)
  {
    http.end();
    webFail("HTTP " + String(code));
    vTaskDelete(nullptr);
    return;
  }

  int len = (int)http.getSize();
  if (len <= 0)
  {
    http.end();
    webFail("固件大小无效");
    vTaskDelete(nullptr);
    return;
  }

  if (!Update.begin(len, U_FLASH))
  {
    http.end();
    webFail(String("空间不足，错误 ") + String(Update.getError()));
    vTaskDelete(nullptr);
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  int total = 0;
  uint8_t buf[1024];
  while (http.connected() && total < len)
  {
    size_t avail = stream->available();
    if (avail)
    {
      int n = stream->readBytes(buf, (size_t)min((size_t)avail, sizeof(buf)));
      if (n <= 0) break;
      if (Update.write(buf, n) != (size_t)n)
      {
        http.end();
        webFail(String("写入失败，错误 ") + String(Update.getError()));
        vTaskDelete(nullptr);
        return;
      }
      total += n;
      pct = total * 100 / len;
    }
    else delay(2);
  }
  http.end();

  if (total != len)
  {
    webFail("数据不完整");
    vTaskDelete(nullptr);
    return;
  }
  if (!Update.end(true) || !Update.isFinished())
  {
    webFail(String("校验失败，错误 ") + String(Update.getError()));
    vTaskDelete(nullptr);
    return;
  }

  setState(OST_DONE, "升级完成，即将重启", 100);
  webBusy = false;
  restartPending = true;
  vTaskDelete(nullptr);
}

bool otaWebStart(const String &urlIn)
{
  if (webBusy || otaBusy()) return false;
  if (ESP.getFreeHeap() < 60000)
  {
    setState(OST_FAIL, "可用内存不足，请重启后重试");
    return false;
  }
  String url = urlIn;
  url.trim();
  if (!url.startsWith("http://") && !url.startsWith("https://"))
  {
    setState(OST_FAIL, "地址需以 http:// 或 https:// 开头");
    return false;
  }
  webUrlTmp = url;
  webBusy = true;
  if (xTaskCreate(webTask, "otaweb", 16384, nullptr, 2, &webTaskHandle) != pdPASS)
  {
    webBusy = false;
    setState(OST_FAIL, "无法启动升级任务");
    return false;
  }
  return true;
}

static void onStart()
{
  slOtaSuspend();
  setState(OST_RUNNING, "已连接推送端，接收固件", 0);
}

static void onProgress(unsigned cur, unsigned total)
{
  pct = (int)(cur * 100 / total);
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  msg = "写入固件 " + String(cur / 1024) + "/" + String(total / 1024) + " KB";
  if (mtx) xSemaphoreGive(mtx);
}

static void onEndOk()
{
  setState(OST_DONE, "升级完成，即将重启", 100);
  restartPending = true;
  restartAt = millis() + 1500;
}

static void onError(ota_error_t e)
{
  String m = "推送升级失败，错误码 " + String((int)e);
  setState(OST_FAIL, m);
  slOtaResume();
}

void otaBegin()
{
  mtx = xSemaphoreCreateMutex();
  ArduinoOTA.setHostname("ESP32-AI-S3");
  ArduinoOTA.onStart(onStart);
  ArduinoOTA.onProgress(onProgress);
  ArduinoOTA.onEnd(onEndOk);
  ArduinoOTA.onError(onError);
  ArduinoOTA.begin();
}

void otaPoll()
{
  if (armed && WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();
  if (restartPending && millis() >= restartAt) ESP.restart();
}