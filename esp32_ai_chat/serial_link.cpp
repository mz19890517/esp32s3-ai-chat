#include "serial_link.h"
#include "frame_log.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#if __has_include("esp32-hal-alloc-ble-mem.h")
#include "esp32-hal-alloc-ble-mem.h"
#endif
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

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
static BLECharacteristic *txChar = nullptr;
static BLEServer *bleSrv = nullptr;
static bool bleConnected = false;
static WiFiServer *tcpSrv = nullptr;
static WiFiClient tcpCli;
static bool tcpListening = false;
static LinkMode mode = LINK_BLE;
static char devName[24] = "ESP32-AI";
static bool bleReady = false;

class RxCb : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *c) override
  {
    String v = c->getValue();
    if (!v.length()) return;
    RxItem it;
    it.len = (uint16_t)min((int)v.length(), RX_MAX);
    memcpy(it.data, v.c_str(), it.len);
    xQueueSend(rxQ, &it, 0);
  }
};

class SrvCb : public BLEServerCallbacks
{
  void onConnect(BLEServer *s) override { bleConnected = true; }
  void onDisconnect(BLEServer *s) override
  {
    bleConnected = false;
    BLEDevice::startAdvertising();
  }
};

void slBegin(const char *name)
{
  snprintf(devName, sizeof(devName), "%s", name);
  rxQ = xQueueCreate(24, sizeof(RxItem));

  BLEDevice::init(devName);
  BLEDevice::setMTU(185);
  bleSrv = BLEDevice::createServer();
  bleSrv->setCallbacks(new SrvCb());
  BLEService *svc = bleSrv->createService(SVC_UUID);
  txChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());
  BLECharacteristic *rxChar = svc->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCb());
  svc->start();
  BLEDevice::startAdvertising();
  bleReady = true;

  tcpSrv = new WiFiServer(SL_TCP_PORT);
  tcpSrv->setNoDelay(true);
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
  String s = "TCP 服务器 " + slLocalIp() + ":" + String(SL_TCP_PORT);
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
  BLEDevice::stopAdvertising();
}

void slOtaResume()
{
  if (!bleReady) return;
  BLEDevice::startAdvertising();
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