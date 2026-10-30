#include "ota_update.h"
#include "serial_link.h"
#include "ui.h"
#include "log_tee.h"
#include "board_config.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SD_MMC.h>
#include <FS.h>
#include <NimBLEDevice.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>

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
static volatile bool bleActive = false;

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

// ---- 供 SD / HTTP 通道复用 ----
void otaReport(OtaState s, const String &msg, int pct)
{
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  setState(s, msg, pct);
}

void otaRequestRestart(uint32_t delayMs)
{
  restartAt = millis() + delayMs;
  restartPending = true;
}

size_t otaTargetSize()
{
  const esp_partition_t *n = esp_ota_get_next_update_partition(nullptr);
  return n ? (size_t)n->size : 0;
}

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
  if (webBusy || otaBusy() || bleActive) return false;
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

// ---------------------------------------------------------------------------
// SD card update: /update/fw.bin is flashed automatically on boot.
// ---------------------------------------------------------------------------
#define SD_FW_PATH "/update/fw.bin"
#define SD_FW_OK   "/update/fw.bin.ok"
#define SD_FW_BAD  "/update/fw.bin.bad"

static bool sdMountCard()
{
  if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0)) return false;
  if (!SD_MMC.begin("/sdcard", true, false, 20000, 4)) return false;
  return SD_MMC.cardType() != CARD_NONE;
}

static void sdMarkBad()
{
  SD_MMC.remove(SD_FW_BAD);
  SD_MMC.rename(SD_FW_PATH, SD_FW_BAD);
}

bool otaSdUpdate()
{
  if (!sdMountCard())
  {
    SD_MMC.end();
    return false;
  }

  File f = SD_MMC.open(SD_FW_PATH, FILE_READ);
  if (!f || f.isDirectory())
  {
    if (f) f.close();
    SD_MMC.end();
    return false;
  }

  const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
  size_t maxSize = slot ? slot->size : 0;
  size_t sz = f.size();

  auto fail = [&](const String &why) -> bool
  {
    Update.abort();
    f.close();
    bootSplash("SD 固件无效");
    delay(1500);
    sdMarkBad();
    SD_MMC.end();
    setState(OST_FAIL, "SD 升级失败：" + why);
    logTeePrintf("[ota] sd fail: %s\n", why.c_str());
    return false;
  };

  if (sz < 65536 || sz > maxSize) return fail("大小不正确");

  uint8_t hdr[24] = {0};
  if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) return fail("读取失败");
  uint16_t chip = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
  if (hdr[0] != 0xE9 || hdr[1] == 0 || hdr[1] > 16 || chip != 0x0009)
    return fail("不是 ESP32-S3 固件");

  f.seek(0);
  bootSplash("检测到 SD 卡固件，准备升级");
  delay(800);
  setState(OST_RUNNING, "从 SD 卡写入固件", 0);

  if (!Update.begin(sz, U_FLASH)) return fail("空间不足 " + String(Update.getError()));

  uint8_t *buf = (uint8_t *)malloc(8192);
  if (!buf) return fail("内存不足");

  size_t total = 0;
  int lastPct = -100;
  while (total < sz)
  {
    size_t want = sz - total;
    if (want > 8192) want = 8192;
    size_t n = f.read(buf, want);
    if (!n) break;
    if (Update.write(buf, n) != n)
    {
      free(buf);
      return fail("写入失败 " + String(Update.getError()));
    }
    total += n;
    int p = (int)((uint64_t)total * 100 / sz);
    if (p - lastPct >= 4)
    {
      lastPct = p;
      bootSplash("正在升级 " + String(p) + "%");
    }
  }
  free(buf);

  if (total != sz) return fail("读取不完整");
  if (!Update.end(true) || !Update.isFinished())
    return fail("校验失败 " + String(Update.getError()));

  f.close();
  SD_MMC.remove(SD_FW_OK);
  if (!SD_MMC.rename(SD_FW_PATH, SD_FW_OK)) SD_MMC.remove(SD_FW_PATH);
  SD_MMC.end();

  setState(OST_DONE, "SD 升级完成，即将重启", 100);
  bootSplash("升级完成，重启中...");
  delay(1200);
  ESP.restart();
  return true;
}

// ---------------------------------------------------------------------------
// BLE OTA (Web Bluetooth)
//   RX 5f9b0002-... host -> device
//      0x01 + uint32 size   start
//      0x02 + data          firmware chunk
//      0x03                 end
//      0x04                 abort
//   TX 5f9b0003-... device -> host
//      0x81 begin ok / 0x82 progress(uint32 bytes) / 0x83 done / 0x84 error(uint32 code)
// ---------------------------------------------------------------------------
static const char *BLE_OTA_SVC = "5f9b0001-7a2c-4d3e-9c1e-9b6f5c1a2d01";
static const char *BLE_OTA_RX = "5f9b0002-7a2c-4d3e-9c1e-9b6f5c1a2d01";
static const char *BLE_OTA_TX = "5f9b0003-7a2c-4d3e-9c1e-9b6f5c1a2d01";
// 只读"信息"特征：内容形如 http://192.168.1.23:8080/ota ，网页连上后自动读取并填好 IP
static const char *BLE_OTA_INFO = "5f9b0004-7a2c-4d3e-9c1e-9b6f5c1a2d01";

static StreamBufferHandle_t bleSb = nullptr;
static QueueHandle_t bleQ = nullptr;
static NimBLECharacteristic *bleTx = nullptr;
static uint32_t bleSize = 0;
static uint32_t bleTotal = 0;
static volatile bool bleOverflow = false;
static volatile bool bleSvcOk = false;   // OTA 服务是否真的注册进了 GATT 表
static NimBLECharacteristic *bleInfo = nullptr;   // 只读信息特征（IP 等）

struct BleCmd
{
  uint8_t cmd;
  uint32_t arg;
};

static void bleEv(uint8_t ev, uint32_t v)
{
  if (!bleTx) return;
  uint8_t b[5];
  b[0] = ev;
  b[1] = (uint8_t)(v & 0xFF);
  b[2] = (uint8_t)((v >> 8) & 0xFF);
  b[3] = (uint8_t)((v >> 16) & 0xFF);
  b[4] = (uint8_t)((v >> 24) & 0xFF);
  bleTx->setValue(b, sizeof(b));
  bleTx->notify();
}

class BleOtaCb : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *c) override
  {
    NimBLEAttValue v = c->getValue();
    const uint8_t *d = v.data();
    size_t n = v.length();
    if (!n) return;
    if (d[0] == 0x01 && n >= 5)
    {
      BleCmd cmd;
      cmd.cmd = 1;
      memcpy(&cmd.arg, d + 1, 4);
      xQueueSend(bleQ, &cmd, 0);
    }
    else if (d[0] == 0x02)
    {
      if (!bleActive || n <= 1) return;
      size_t sent = xStreamBufferSend(bleSb, d + 1, n - 1, 0);
      if (sent != n - 1) bleOverflow = true;
    }
    else if (d[0] == 0x03 || d[0] == 0x04)
    {
      BleCmd cmd;
      cmd.cmd = d[0];
      cmd.arg = 0;
      xQueueSend(bleQ, &cmd, 0);
    }
  }
};

static void bleFail(uint8_t code, const String &why)
{
  Update.abort();
  bleActive = false;
  if (bleSb) xStreamBufferReset(bleSb);
  bleEv(0x84, code);
  setState(OST_FAIL, "蓝牙升级：" + why);
  logTeePrintf("[ota] ble fail code=%u: %s\n", (unsigned)code, why.c_str());
}

static void bleOtaTask(void *)
{
  uint8_t *buf = (uint8_t *)malloc(4096);
  if (!buf)
  {
    vTaskDelete(nullptr);
    return;
  }

  bool endReq = false;
  uint32_t lastNotify = 0;
  uint32_t lastRx = 0;

  for (;;)
  {
    BleCmd c;
    if (!bleActive)
    {
      if (xQueueReceive(bleQ, &c, portMAX_DELAY) != pdTRUE) continue;
      if (c.cmd == 1)
      {
        const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
        uint32_t maxSize = slot ? (uint32_t)slot->size : 0;
        if (c.arg < 65536 || c.arg > maxSize)
        {
          bleEv(0x84, 5);
          continue;
        }
        xStreamBufferReset(bleSb);
        setState(OST_RUNNING, "蓝牙接收固件", 0);
        if (!Update.begin((size_t)c.arg, U_FLASH))
        {
          bleEv(0x84, 1);
          setState(OST_FAIL, "蓝牙升级：无法开始写入");
          continue;
        }
        bleSize = c.arg;
        bleTotal = 0;
        lastNotify = 0;
        lastRx = millis();
        endReq = false;
        bleOverflow = false;
        bleActive = true;
        bleEv(0x81, 0);
      }
      continue;
    }

    size_t n = xStreamBufferReceive(bleSb, buf, 4096, 20);
    if (n)
    {
      lastRx = millis();
      if (Update.write(buf, n) != n)
      {
        bleFail(2, "写入失败");
        continue;
      }
      bleTotal += n;
      if (bleTotal > bleSize)
      {
        bleFail(5, "数据超长");
        continue;
      }
      if (bleTotal - lastNotify >= 8192 || bleTotal >= bleSize)
      {
        lastNotify = bleTotal;
        bleEv(0x82, bleTotal);
        setState(OST_RUNNING,
                 "蓝牙写入 " + String(bleTotal / 1024) + "/" + String(bleSize / 1024) + " KB",
                 (int)((uint64_t)bleTotal * 100 / bleSize));
      }
    }

    while (xQueueReceive(bleQ, &c, 0) == pdTRUE)
    {
      if (c.cmd == 0x03) endReq = true;
      else if (c.cmd == 0x04)
      {
        bleFail(7, "已被取消");
        break;
      }
    }
    if (!bleActive) continue;
    if (bleOverflow)
    {
      bleFail(4, "数据溢出");
      continue;
    }
    if (endReq && xStreamBufferIsEmpty(bleSb))
    {
      bleActive = false;
      if (bleTotal != bleSize)
      {
        Update.abort();
        bleEv(0x84, 5);
        setState(OST_FAIL, "蓝牙升级：数据不完整");
        continue;
      }
      if (!Update.end(true) || !Update.isFinished())
      {
        bleEv(0x84, 3);
        setState(OST_FAIL, "蓝牙升级：校验失败");
        continue;
      }
      bleEv(0x83, bleSize);
      setState(OST_DONE, "蓝牙升级完成，即将重启", 100);
      restartPending = true;
      restartAt = millis() + 1800;
      continue;
    }
    if (millis() - lastRx > 30000) bleFail(6, "接收超时");
  }
}

static String otaLastInfo;   // 供 BLE 栈重建（slBleResume）后恢复“信息”特征

void otaBleBegin()
{
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (!srv) return;
  if (!bleSb) bleSb = xStreamBufferCreate(16384, 1);
  if (!bleQ) bleQ = xQueueCreate(8, sizeof(BleCmd));
  if (!bleSb || !bleQ) return;
  NimBLEService *svc = srv->createService(BLE_OTA_SVC);
  if (!svc) return;
  bleTx = svc->createCharacteristic(BLE_OTA_TX, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rx = svc->createCharacteristic(BLE_OTA_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  if (!bleTx || !rx)
  {
    logTeePrintf("[ota] ble char create failed tx=%p rx=%p\n", (void *)bleTx, (void *)rx);
    return;
  }
  rx->setCallbacks(new BleOtaCb());
  bleInfo = svc->createCharacteristic(BLE_OTA_INFO, NIMBLE_PROPERTY::READ);
  if (bleInfo && otaLastInfo.length())
    bleInfo->setValue((const uint8_t *)otaLastInfo.c_str(), otaLastInfo.length());
  bleSvcOk = svc->start();
  logTeePrintf("[ota] ble ota svc start=%d info=%p\n", (int)bleSvcOk, (void *)bleInfo);
  static bool otaTaskMade = false;
  if (!otaTaskMade)
  {
    otaTaskMade = true;
    xTaskCreate(bleOtaTask, "otable", 8192, nullptr, 2, nullptr);
  }
}

void otaBleSetInfo(const String &s)
{
  otaLastInfo = s;
  if (!bleInfo) return;
  bleInfo->setValue((const uint8_t *)s.c_str(), s.length());
}

bool otaBleReady() { return bleSvcOk; }
bool otaBleActive() { return bleActive; }

void otaBegin()
{
  logTeePrintf("[ota] enter heap=%u stack=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
  Serial0.printf("[ota] enter heap=%u stack=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
  mtx = xSemaphoreCreateMutex();
  ArduinoOTA.setHostname("ESP32-AI-S3");
  ArduinoOTA.onStart(onStart);
  ArduinoOTA.onProgress(onProgress);
  ArduinoOTA.onEnd(onEndOk);
  ArduinoOTA.onError(onError);
  logTeePrintf("[ota] pre-begin stack=%u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  Serial0.printf("[ota] pre-begin stack=%u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  ArduinoOTA.begin();
  // 蓝牙 OTA 服务与广播由 setup() 显式按顺序调用：
  //   slBegin() -> otaBleBegin() -> otaBegin() -> slStartAdv()
  // 必须先建完所有 GATT 服务再开广播，否则 ble_gatts_add_svcs() 会返回 EBUSY。
  logTeePrintf("[ota] begin ok heap=%u stack=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
  Serial0.printf("[ota] begin ok heap=%u stack=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void otaPoll()
{
  if (armed && !bleActive && WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();
  if (restartPending && millis() >= restartAt) ESP.restart();
}