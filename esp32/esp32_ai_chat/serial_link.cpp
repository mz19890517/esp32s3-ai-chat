#include "serial_link.h"
#include "frame_log.h"
#include "log_tee.h"
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

static const int RX_MAX = 256;     // 一次 BLE 写的最大字节数（MTU 247 → 244 字节，不能截断）
static const int TX_CHUNK = 128;   // BLE 通知分片
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
static volatile bool bleDown = false;   // BLE 栈挂起中（TLS 握手期间），回调须跳过 BLE 操作
static volatile bool advSuspended = false;
static QueueHandle_t txQ = nullptr;
static uint32_t lastTxMs = 0;
static uint8_t tcpRxBuf[1024];

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
    if (!bleDown) NimBLEDevice::startAdvertising();   // 挂起拆栈期间禁止重启广播
  }
};

#define SLOG(fmt, ...)                       \
  do                                         \
  {                                          \
    logTeePrintf(fmt, ##__VA_ARGS__);       \
    Serial0.printf(fmt, ##__VA_ARGS__);      \
  } while (0)

static void bleInitCore();

void slBegin(const char *name)
{
  snprintf(devName, sizeof(devName), "%s", name);
  SLOG("[ble] enter heap=%u int=%u largest=%u\n",
       (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  rxQ = xQueueCreate(16, sizeof(RxItem));
  txQ = xQueueCreate(TX_QUEUE_N, sizeof(TxItem));
  SLOG("[ble] queue ok\n");

  bleInitCore();

  tcpSrv = new WiFiServer(SL_TCP_PORT);
  tcpSrv->setNoDelay(true);
  SLOG("[ble] tcp ok\n");
}

// NimBLE 栈初始化：init + NUS 服务（队列/tcpSrv 不在这里，保证可重复调用）
// 注意：这里不启动广播，必须等所有 GATT 服务（NUS + 蓝牙 OTA）注册完再 slStartAdv()
static void bleInitCore()
{
  NimBLEDevice::init(devName);
  NimBLEDevice::setDeviceName(devName);
  SLOG("[ble] init ok\n");
  NimBLEDevice::setMTU(247);
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
  adv->setName(devName);
  adv->setScanResponse(false);
  bleReady = true;
  SLOG("[ble] server ready, adv deferred\n");
}

// TLS 握手前临时释放 BLE 内存（控制器+主机 ~50KB），握完 slBleResume 重建。
// 挂起期间网页蓝牙连接会断开，恢复后需重新连接。
bool slBleSuspend()
{
  if (bleDown) return true;
  if (!bleReady) { bleDown = true; return true; }
  // 标志必须先置位：deinit 拆连接会同步触发 onDisconnect 回调，回调里要跳过 BLE 操作
  bleDown = true;
  bleReady = false;
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (adv) adv->stop();
  txChar = nullptr;
  NimBLEDevice::deinit(true);
  SLOG("[ble] suspended heap=%u largest=%u\n",
       (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
}

void slBleResume()
{
  if (!bleDown) return;
  bleDown = false;
  bleInitCore();
  SLOG("[ble] resumed heap=%u largest=%u\n",
       (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

bool slBleDown() { return bleDown; }

// 所有 GATT 服务注册完之后再调用（见 otaBegin）
void slStartAdv()
{
  if (!bleReady) return;
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  bool advOk = false;
  for (int i = 0; i < 50 && !advOk; i++)
  {
    advOk = adv->start();
    if (!advOk) vTaskDelay(pdMS_TO_TICKS(20));
  }
  SLOG("[ble] adv ok=%d active=%d\n", (int)advOk, (int)adv->isAdvertising());
}

LinkMode slMode() { return mode; }
void slSetMode(LinkMode m) { mode = m; }
void slToggleMode() { mode = (mode == LINK_BLE) ? LINK_TCP : LINK_BLE; }

bool slActive()
{
  return (mode == LINK_BLE) ? bleConnected : tcpCli.connected();
}

String slLocalIp() { return WiFi.localIP().toString(); }

bool slBleConnected() { return bleConnected; }

const char *slDeviceName() { return devName; }

String slBleAddr()
{
  return String(NimBLEDevice::getAddress().toString().c_str());
}

bool slBleAdvActive()
{
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  return adv && adv->isAdvertising();
}

String slStatus()
{
  if (mode == LINK_BLE)
  {
    String st = bleConnected ? String("BLE 已连接 ") + devName : String("BLE 等待连接 ") + devName;
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    st += (adv && adv->isAdvertising()) ? " 广播中" : " 未广播";
    return st;
  }
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
    if (!bleConnected || !txChar || !txQ) return false;
    // 只入队，由 slPoll() 每 10ms 送一片，避免 delay(20) 卡住主循环
    for (int i = 0; i < n; i += TX_CHUNK)
    {
      TxItem it;
      it.len = (uint16_t)min(TX_CHUNK, n - i);
      memcpy(it.data, d + i, it.len);
      if (xQueueSend(txQ, &it, 0) != pdTRUE) return i > 0;
    }
    return true;
  }
  if (!tcpCli.connected()) return false;
  return tcpCli.write(d, n) == (size_t)n;
}

void slOtaSuspend()
{
  advSuspended = true;
  if (!bleReady) return;
  NimBLEDevice::stopAdvertising();
}

void slOtaResume()
{
  advSuspended = false;
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

  // 广播自愈：连接断开、主机复位后广播可能丢，没连接时每 3 秒检查并重开
  static uint32_t lastAdv = 0;
  if (bleReady && !advSuspended && !bleConnected && millis() - lastAdv > 3000)
  {
    lastAdv = millis();
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    if (adv && !adv->isAdvertising())
    {
      bool ok = adv->start();
      SLOG("[ble] adv retry ok=%d\n", (int)ok);
    }
  }

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
    WiFiClient c = tcpSrv->accept();   // 3.x 里 available() 已改名 accept()
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
