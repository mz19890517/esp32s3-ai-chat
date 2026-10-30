#include "ota_ble.h"
#include "ota_update.h"
#include "serial_link.h"

#include <Update.h>
// NimBLE-Arduino 1.4.x：只有 NimBLE* 头文件，CCCD 由协议栈按 NOTIFY 属性自动创建
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>

/*
 * 蓝牙 OTA 协议
 *
 * CTRL 特征（写）：
 *   [0x01][size:u32 LE][crc32:u32 LE]  开始：校验大小，Update.begin(size)
 *   [0x02]                             结束：校验长度/CRC，Update.end(true) 并切换启动分区
 *   [0x03]                             中止：Update.abort()
 *   [0x04]                             查询：立即回一帧状态
 * DATA 特征（写无响应）：固件原始字节，顺序追加
 * STAT 特征（通知/读）：[state:u8][received:u32 LE][total:u32 LE]
 *   state: 0 空闲 1 就绪 2 接收中 3 完成 4 失败 5 已中止
 *
 * 流控：设备每写入 4KB 闪存通知一次已收字节数；网页保证“已发未确认”不超过 4KB，
 *       因此 12KB 的环形缓冲不会被写爆（真溢出说明对端不守规矩，直接中止）。
 */

#define RING_CAP 12288
#define FLUSH_SZ 4096

enum BleSt : uint8_t
{
  BS_IDLE = 0,
  BS_READY = 1,
  BS_RUN = 2,
  BS_DONE = 3,
  BS_ERR = 4,
  BS_ABORT = 5,
};

static uint8_t ring[RING_CAP];
static volatile size_t ringHead = 0; // 生产者（BLE 主机任务）写入位置
static volatile size_t ringTail = 0; // 消费者（主循环）读出位置

static volatile uint8_t bstate = BS_IDLE;
static volatile uint8_t pendingCmd = 0;
static volatile bool overflow = false;
static volatile uint32_t wantSize = 0;
static volatile uint32_t wantCrc = 0;

static uint32_t gotBytes = 0;
static uint32_t gotCrc = 0;
static uint32_t totalBytes = 0;
static int lastPct = -1;

static String errTxt;
static NimBLECharacteristic *statChar = nullptr;
static bool inited = false;

static inline size_t ringUsed()
{
  return (ringHead + RING_CAP - ringTail) % RING_CAP;
}

static inline size_t ringFree()
{
  return RING_CAP - 1 - ringUsed();
}

static bool ringPush(const uint8_t *d, size_t n)
{
  if (ringFree() < n) return false;
  for (size_t i = 0; i < n; i++)
  {
    ring[ringHead] = d[i];
    ringHead = (ringHead + 1) % RING_CAP;
  }
  return true;
}

static uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n)
{
  crc = ~crc;
  while (n--)
  {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
  }
  return ~crc;
}

static void putU32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t getU32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void notifyStatus()
{
  if (!statChar) return;
  uint8_t b[9];
  b[0] = bstate;
  putU32(b + 1, gotBytes);
  putU32(b + 5, totalBytes);
  statChar->setValue(b, sizeof(b));
  statChar->notify();
}

static void doAbort(const String &why, uint8_t newState)
{
  bool wasRunning = (bstate == BS_RUN);
  if (wasRunning)
  {
    Update.abort();
    otaReport(OST_FAIL, "蓝牙升级失败: " + why, 0);
  }
  bstate = newState;
  errTxt = why;
  ringHead = ringTail = 0;
  overflow = false;
  totalBytes = gotBytes;
  notifyStatus();
}

static void beginSession()
{
  if (bstate == BS_RUN) doAbort("被新的开始指令打断", BS_ABORT);
  if (otaBusy())
  {
    bstate = BS_ERR;
    errTxt = "其他升级任务正在进行";
    notifyStatus();
    return;
  }
  uint32_t sz = wantSize;
  if (sz < 16384)
  {
    bstate = BS_ERR;
    errTxt = "固件尺寸过小";
    notifyStatus();
    return;
  }
  if (sz > (uint32_t)otaTargetSize())
  {
    bstate = BS_ERR;
    errTxt = "固件超出分区容量";
    notifyStatus();
    return;
  }
  if (!Update.begin(sz, U_FLASH))
  {
    bstate = BS_ERR;
    errTxt = String("无法写入分区，错误 ") + String(Update.getError());
    notifyStatus();
    return;
  }
  ringHead = ringTail = 0;
  overflow = false;
  gotBytes = 0;
  gotCrc = 0;
  totalBytes = sz;
  lastPct = 0;
  bstate = BS_RUN;
  otaSetChannel(OCH_BLE);
  otaReport(OST_RUNNING, "蓝牙接收固件", 0);
  notifyStatus();
}

static bool flushBlock(const uint8_t *p, size_t n)
{
  if (gotBytes + n > wantSize)
  {
    doAbort("收到数据超出声明长度", BS_ERR);
    return false;
  }
  if (Update.write((uint8_t *)p, n) != n)
  {
    doAbort(String("写入闪存失败，错误 ") + String(Update.getError()), BS_ERR);
    return false;
  }
  gotBytes += n;
  gotCrc = crc32Update(gotCrc, p, n);
  int pct = (int)((uint64_t)gotBytes * 100 / (uint64_t)wantSize);
  if (pct != lastPct)
  {
    lastPct = pct;
    otaReport(OST_RUNNING, "蓝牙接收固件", pct);
  }
  notifyStatus(); // 每 4KB 一次流控回执
  return true;
}

// 从环形缓冲搬出数据写闪存；all=false 时只在攒满一块时写
static bool drainRing(bool all)
{
  uint8_t tmp[FLUSH_SZ];
  while (ringUsed() >= (all ? (size_t)1 : (size_t)FLUSH_SZ))
  {
    size_t n = ringUsed();
    if (n > (size_t)FLUSH_SZ) n = FLUSH_SZ;
    for (size_t i = 0; i < n; i++)
    {
      tmp[i] = ring[ringTail];
      ringTail = (ringTail + 1) % RING_CAP;
    }
    if (!flushBlock(tmp, n)) return false;
  }
  return true;
}

static void finishSession()
{
  if (bstate != BS_RUN)
  {
    notifyStatus();
    return;
  }
  if (!drainRing(true)) return;
  if (bstate != BS_RUN) return;
  if (gotBytes != wantSize)
  {
    doAbort("数据长度不足", BS_ERR);
    return;
  }
  if (gotCrc != wantCrc)
  {
    doAbort("CRC 校验不通过", BS_ERR);
    return;
  }
  if (!Update.end(true) || !Update.isFinished())
  {
    doAbort(String("固件校验失败，错误 ") + String(Update.getError()), BS_ERR);
    return;
  }
  bstate = BS_DONE;
  totalBytes = wantSize;
  notifyStatus();
  otaReport(OST_DONE, "蓝牙升级完成，即将重启", 100);
  otaRequestRestart(1500);
}

class OtaCtrlCb : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *c) override
  {
    std::string v = c->getValue();
    const uint8_t *p = (const uint8_t *)v.data();
    size_t n = v.length();
    if (!n) return;
    if (p[0] == 0x01 && n >= 9)
    {
      wantSize = getU32(p + 1);
      wantCrc = getU32(p + 5);
      pendingCmd = 0x01;
    }
    else if (p[0] == 0x02) pendingCmd = 0x02;
    else if (p[0] == 0x03) pendingCmd = 0x03;
    else if (p[0] == 0x04) pendingCmd = 0x04;
  }
};

class OtaDataCb : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *c) override
  {
    if (bstate != BS_RUN) return;
    std::string v = c->getValue();
    if (!ringPush((const uint8_t *)v.data(), v.length())) overflow = true;
  }
};

void otaBleBegin()
{
  if (inited) return;
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (!srv) return;
  NimBLEService *svc = srv->createService(OTA_BLE_SVC);
  if (!svc) return;

  NimBLECharacteristic *ctrl = svc->createCharacteristic(
      OTA_BLE_CTRL, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  if (!ctrl) return;
  ctrl->setCallbacks(new OtaCtrlCb());

  NimBLECharacteristic *data = svc->createCharacteristic(OTA_BLE_DATA, NIMBLE_PROPERTY::WRITE_NR);
  if (!data) return;
  data->setCallbacks(new OtaDataCb());

  statChar = svc->createCharacteristic(OTA_BLE_STAT, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  if (!statChar) return;

  svc->start();

  NimBLEDevice::stopAdvertising();
  auto *adv = NimBLEDevice::getAdvertising();
  if (adv)
  {
    adv->addServiceUUID(OTA_BLE_SVC);
    adv->addServiceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
  }
  NimBLEDevice::startAdvertising();
  inited = true;
}

bool otaBleRunning()
{
  return bstate == BS_RUN;
}

void otaBleCancel()
{
  if (bstate == BS_RUN) doAbort("用户取消", BS_ABORT);
}

void otaBlePoll()
{
  // 命令统一在主循环处理，避免在 BLE 回调里碰 Update
  uint8_t cmd = pendingCmd;
  pendingCmd = 0;
  if (cmd == 0x01) beginSession();
  else if (cmd == 0x02) finishSession();
  else if (cmd == 0x03) doAbort("用户取消", BS_ABORT);
  else if (cmd == 0x04) notifyStatus();

  if (bstate != BS_RUN) return;

  if (!slBleConnected())
  {
    doAbort("蓝牙连接已断开", BS_ABORT);
    return;
  }
  if (overflow)
  {
    overflow = false;
    doAbort("接收缓冲溢出，请调小分片后重试", BS_ERR);
    return;
  }
  drainRing(false);
}

String otaBleStatus()
{
  switch (bstate)
  {
  case BS_RUN:
    return String("接收中 ") + String((unsigned)gotBytes) + " / " + String((unsigned)wantSize) + " B";
  case BS_DONE:
    return "升级完成，等待重启";
  case BS_ERR:
    return "失败: " + errTxt;
  case BS_ABORT:
    return "已中止: " + errTxt;
  case BS_READY:
    return "已就绪";
  default:
    return slBleConnected() ? String("已连接，等待网页开始") : String("等待网页连接");
  }
}
