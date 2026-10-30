#include "serial_link.h"
#include "frame_log.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#if __has_include("esp32-hal-alloc-ble-mem.h")
#include "esp32-hal-alloc-ble-mem.h"
#endif
// NimBLE-Arduino 1.4.x 只提供 NimBLE* 头文件，没有 BLE2902.h / BLEDevice.h 之类兼容头
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>

static const char *SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char *TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static const int RX_MAX = 256;     // 一次 BLE 写的最大字节数
static const int TX_CHUNK = 128;   // BLE 通知分片（≤ MTU-3）
static const int TX_QUEUE_N = 16;  // 待发分片队列深度
static const int TX_GAP_MS = 10;   // 分片间隔，避免打爆 BLE 发送缓冲

struct RxItem
{
  uint16_t len;
  uint8_t data[RX_MAX];
};

struct TxItem
{
  uint16_t len;
  uint8_t data[TX_CHUNK];
};

static QueueHandle_t rxQ = nullptr;
static QueueHandle_t txQ = nullptr;
static NimBLECharacteristic *txChar = nullptr;
static NimBLEServer *bleSrv = nullptr;
static volatile bool bleConnected = false;
static WiFiServer *tcpSrv = nullptr;
static WiFiClient tcpCli;
static bool tcpListening = false;
static LinkMode mode = LINK_BLE;
static char devName[24] = "ESP32-AI";
static bool bleReady = false;
static uint32_t lastTxMs = 0;
static uint8_t tcpRxBuf[1024];

class RxCb : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *c) override
  {
    // 官方推荐的取法：经 operator std::string() 拷贝，避免 NimBLEAttValue 的浅拷贝析构问题
    std::string v = c->getValue();
    if (v.empty()) return;
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

void slBegin(const char *name)
{
  snprintf(devName, sizeof(devName), "%s", name);
  rxQ = xQueueCreate(16, sizeof(RxItem));
  txQ = xQueueCreate(TX_QUEUE_N, sizeof(TxItem));

  NimBLEDevice::init(devName);
  NimBLEDevice::setMTU(247);
  bleSrv = NimBLEDevice::createServer();
  bleSrv->setCallbacks(new SrvCb());
  NimBLEService *svc = bleSrv->createService(SVC_UUID);
  txChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rxChar = svc->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCb());
  svc->start();
  NimBLEDevice::startAdvertising();
  bleReady = true;

  tcpSrv = new WiFiServer(SL_TCP_PORT);
  tcpSrv->setNoDelay(true);
}

LinkMode slMode() { return mode; }
void slSetMode(LinkMode m) { mode = m; }
void slToggleMode() { mode = (mode == LINK_BLE) ? LINK_TCP : LINK_BLE; }

bool slBleConnected() { return bleConnected; }
const char *slDeviceName() { return devName; }

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
    if (!bleConnected || !txChar || !txQ) return false;
    for (int i = 0; i < n; i += TX_CHUNK)
    {
      TxItem it;
      it.len = (uint16_t)min(TX_CHUNK, n - i);
      memcpy(it.data, d + i, it.len);
      if (xQueueSend(txQ, &it, 0) != pdTRUE) return i > 0; // 队列满：已入队部分算成功
    }
    return true;
  }
  if (!tcpCli.connected()) return false;
  return tcpCli.write(d, n) == (size_t)n;
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

// BLE 发送分片：每 10ms 送一片，不阻塞主循环
static void serviceBleTx()
{
  if (!txQ) return;
  if (mode != LINK_BLE || !bleConnected || !txChar)
  {
    TxItem drop;
    while (xQueueReceive(txQ, &drop, 0) == pdTRUE) {}
    return;
  }
  uint32_t now = millis();
  if (now - lastTxMs < TX_GAP_MS) return;
  TxItem it;
  if (xQueueReceive(txQ, &it, 0) != pdTRUE) return;
  lastTxMs = now;
  txChar->setValue(it.data, it.len);
  txChar->notify();
}

void slPoll()
{
  serviceBleTx();

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
    // 一次尽量读完整包，避免把一帧拆成多条记录
    int total = 0;
    while (total < (int)sizeof(tcpRxBuf) && tcpCli.available() > 0)
    {
      int n = tcpCli.read(tcpRxBuf + total, sizeof(tcpRxBuf) - total);
      if (n <= 0) break;
      total += n;
    }
    if (total > 0 && mode == LINK_TCP) flAddRx(tcpRxBuf, total);
  }
}
