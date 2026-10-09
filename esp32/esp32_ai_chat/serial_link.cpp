#include "serial_link.h"
#include "frame_log.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#if __has_include("esp32-hal-alloc-ble-mem.h")
#include "esp32-hal-alloc-ble-mem.h"
#endif
#include <NimBLEDevice.h>

static const char *SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char *TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static const int RX_MAX = 160;

struct RxItem
{
  uint16_t len;
  uint8_t data[RX_MAX];
};

static QueueHandle_t rxQ;
static NimBLECharacteristic *txChar = nullptr;
static NimBLEServer *bleSrv = nullptr;
static bool bleConnected = false;
static WiFiServer *tcpSrv = nullptr;
static WiFiClient tcpCli;
static bool tcpListening = false;
static LinkMode mode = LINK_BLE;
static char devName[24] = "ESP32-AI";
static bool bleReady = false;

class RxCb : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *c) override
  {
    NimBLEAttValue v = c->getValue();
    if (!v.length()) return;
    RxItem it;
    it.len = (uint16_t)min((int)v.length(), RX_MAX);
    memcpy(it.data, v.data(), it.len);
    xQueueSend(rxQ, &it, 0);
  }
};

class SrvCb : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *s) override { bleConnected = true; }
  void onDisconnect(NimBLEServer *s) override
  {
    bleConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

#define SLOG(fmt, ...)                       \
  do                                         \
  {                                          \
    Serial.printf(fmt, ##__VA_ARGS__);       \
    Serial0.printf(fmt, ##__VA_ARGS__);      \
  } while (0)

void slBegin(const char *name)
{
  snprintf(devName, sizeof(devName), "%s", name);
  SLOG("[ble] enter heap=%u int=%u largest=%u\n",
       (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  rxQ = xQueueCreate(24, sizeof(RxItem));
  SLOG("[ble] queue ok\n");

  NimBLEDevice::init(devName);
  SLOG("[ble] init ok\n");
  NimBLEDevice::setMTU(185);
  SLOG("[ble] mtu ok\n");
  bleSrv = NimBLEDevice::createServer();
  bleSrv->setCallbacks(new SrvCb());
  SLOG("[ble] server ok\n");
  NimBLEService *svc = bleSrv->createService(SVC_UUID);
  SLOG("[ble] service ok\n");
  txChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rxChar = svc->createCharacteristic(RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCb());
  SLOG("[ble] chars ok\n");
  svc->start();
  SLOG("[ble] svc start ok\n");
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  adv->start();
  SLOG("[ble] adv ok\n");
  bleReady = true;

  tcpSrv = new WiFiServer(SL_TCP_PORT);
  tcpSrv->setNoDelay(true);
  SLOG("[ble] tcp ok\n");
}

LinkMode slMode() { return mode; }
void slSetMode(LinkMode m) { mode = m; }
void slToggleMode() { mode = (mode == LINK_BLE) ? LINK_TCP : LINK_BLE; }

bool slActive()
{
  return (mode == LINK_BLE) ? bleConnected : tcpCli.connected();
}

String slLocalIp() { return WiFi.localIP().toString(); }

String slStatus()
{
  if (mode == LINK_BLE) return bleConnected ? String("BLE 已连接 ") + devName : String("BLE 等待连接 ") + devName;
  String s = "TCP 服务 " + slLocalIp() + ":" + String(SL_TCP_PORT);
  s += tcpCli.connected() ? " 已连接" : " 等待连接";
  return s;
}

bool slSend(const String &s)
{
  if (!s.length()) return false;
  const uint8_t *d = (const uint8_t *)s.c_str();
  int n = s.length();
  if (mode == LINK_BLE)
  {
    if (!bleConnected || !txChar) return false;
    for (int i = 0; i < n; i += 128)
    {
      int chunk = min(128, n - i);
      txChar->setValue(d + i, chunk);
      txChar->notify();
      delay(20);
    }
    return true;
  }
  if (!tcpCli.connected()) return false;
  return tcpCli.write(d, n) == n;
}

void slOtaSuspend()
{
  if (!bleReady) return;
  NimBLEDevice::stopAdvertising();
}

void slOtaResume()
{
  if (!bleReady) return;
  NimBLEDevice::startAdvertising();
}

void slPoll()
{
  if (rxQ)
  {
    RxItem it;
    while (xQueueReceive(rxQ, &it, 0) == pdTRUE)
    {
      if (mode == LINK_BLE) flAddRx(it.data, it.len);
    }
  }

  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !tcpListening)
  {
    tcpSrv->begin();
    tcpSrv->setNoDelay(true);
    tcpListening = true;
  }
  else if (!up && tcpListening)
  {
    tcpSrv->stop();
    tcpListening = false;
    tcpCli.stop();
  }

  if (tcpListening && !tcpCli.connected())
  {
    WiFiClient c = tcpSrv->available();
    if (c)
    {
      if (tcpCli && !tcpCli.connected()) tcpCli.stop();
      tcpCli = c;
    }
  }

  if (tcpCli.connected())
  {
    uint8_t buf[RX_MAX];
    int total = 0;
    while (tcpCli.available() && total < RX_MAX)
    {
      int n = tcpCli.readBytes(buf + total, RX_MAX - total);
      if (n <= 0) break;
      total += n;
    }
    if (total > 0 && mode == LINK_TCP) flAddRx(buf, total);
  }
}
