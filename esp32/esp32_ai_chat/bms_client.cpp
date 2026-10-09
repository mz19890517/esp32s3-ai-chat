#include "bms_client.h"
#include <NimBLEClient.h>
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEUtils.h>

namespace bms {

static const char *SVC_UUID = "0000ff00-0000-1000-8000-00805f9b34fb";
static const char *NOTIFY_UUID = "0000ff01-0000-1000-8000-00805f9b34fb";
static const char *WRITE_UUID = "0000ff02-0000-1000-8000-00805f9b34fb";

static const uint8_t FRAME_START = 0xDD;
static const uint8_t FRAME_END = 0x77;
static const uint8_t MODE_READ = 0xA5;
static const uint8_t CMD_BASE_INFO = 0x03;
static const uint8_t CMD_CELL_VOLTAGE = 0x04;

static const uint32_t POLL_INTERVAL_MS = 2000;
static const uint32_t SCAN_DURATION_MS = 5000;

static NimBLEClient *client = nullptr;
static BLERemoteCharacteristic *notifyChar = nullptr;
static BLERemoteCharacteristic *writeChar = nullptr;

static bool connectedFlag = false;
static bool hasDataFlag = false;
static BaseInfo lastInfo;
static uint32_t lastPoll = 0;
static uint8_t pendingCmd = 0;
static bool writeInFlight = false;

static uint8_t streamBuf[2048];
static int streamLen = 0;

static void onNotify(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify);
static void connectToServer(NimBLEAdvertisedDevice *dev);
static void buildRequest(uint8_t cmd, uint8_t *out, int *outLen);
static bool verifyChecksum(uint8_t *frame, int total);
static void handleFrame(uint8_t *frame, int total);
static void parseBaseInfo(uint8_t *c, int len);
static void pump();

class ClientCallbacks : public NimBLEClientCallbacks
{
  void onConnect(NimBLEClient *c) override
  {
    connectedFlag = true;
  }
  void onDisconnect(NimBLEClient *c) override
  {
    connectedFlag = false;
    hasDataFlag = false;
    notifyChar = nullptr;
    writeChar = nullptr;
  }
};

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
  void onResult(NimBLEAdvertisedDevice *dev) override
  {
    if (dev->haveServiceUUID() && dev->isAdvertisingService(NimBLEUUID(SVC_UUID)))
    {
      connectToServer(dev);
    }
  }
};

static ClientCallbacks clientCb;
static ScanCallbacks scanCb;
static volatile bool scanRequested = false;

static void onScanComplete(NimBLEScanResults results)
{
  scanRequested = false;
}

bool begin()
{
  NimBLEDevice::init("ESP32-BMS");
  NimBLEDevice::setMTU(247);
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCb);
  return true;
}

void poll()
{
  if (!client) return;

  if (!connectedFlag)
  {
    if (NimBLEDevice::getScan()->isScanning()) return;
    NimBLEDevice::getScan()->setAdvertisedDeviceCallbacks(&scanCb);
    NimBLEDevice::getScan()->start(SCAN_DURATION_MS, onScanComplete, false);
    return;
  }

  if (millis() - lastPoll < POLL_INTERVAL_MS) return;
  lastPoll = millis();

  if (!writeChar) return;
  if (writeInFlight) return;

  pendingCmd = CMD_BASE_INFO;
  pump();
}

static void pump()
{
  if (!writeChar || writeInFlight) return;
  uint8_t frame[16];
  int len = 0;
  buildRequest(pendingCmd, frame, &len);
  writeChar->writeValue(frame, len, true);
  writeInFlight = true;
}

static void buildRequest(uint8_t cmd, uint8_t *out, int *outLen)
{
  out[0] = FRAME_START;
  out[1] = MODE_READ;
  out[2] = cmd;
  out[3] = 0;
  int sum = cmd;
  int chk = (~sum + 1) & 0xFFFF;
  out[4] = (chk >> 8) & 0xFF;
  out[5] = chk & 0xFF;
  out[6] = FRAME_END;
  *outLen = 7;
}

static void connectToServer(NimBLEAdvertisedDevice *dev)
{
  NimBLEDevice::getScan()->stop();
  if (!client->connect(dev))
  {
    NimBLEDevice::getScan()->start(SCAN_DURATION_MS, onScanComplete, false);
    return;
  }
  BLERemoteService *svc = client->getService(SVC_UUID);
  if (!svc)
  {
    client->disconnect();
    return;
  }
  notifyChar = svc->getCharacteristic(NOTIFY_UUID);
  writeChar = svc->getCharacteristic(WRITE_UUID);
  if (!notifyChar || !writeChar)
  {
    client->disconnect();
    return;
  }
  notifyChar->subscribe(true, onNotify);
}

static void onNotify(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify)
{
  writeInFlight = false;
  if (len <= 0) return;
  if (streamLen + len > (int)sizeof(streamBuf)) streamLen = 0;
  memcpy(streamBuf + streamLen, data, len);
  streamLen += len;

  int i = 0;
  while (i < streamLen)
  {
    if (streamBuf[i] != FRAME_START) { i++; continue; }
    if (streamLen - i < 4) break;
    int total = (streamBuf[i + 3] & 0xFF) + 7;
    if (streamLen - i < total) break;
    if (streamBuf[i + total - 1] == FRAME_END && verifyChecksum(streamBuf + i, total))
    {
      handleFrame(streamBuf + i, total);
      i += total;
    }
    else i++;
  }
  if (i > 0)
  {
    memmove(streamBuf, streamBuf + i, streamLen - i);
    streamLen -= i;
  }
}

static bool verifyChecksum(uint8_t *frame, int total)
{
  int len = frame[3] & 0xFF;
  int sum = len + (frame[2] & 0xFF);
  for (int i = 0; i < len; i++) sum += frame[4 + i] & 0xFF;
  int expected = (~sum + 1) & 0xFFFF;
  return frame[total - 3] == (expected >> 8) && frame[total - 2] == (expected & 0xFF);
}

static void handleFrame(uint8_t *frame, int total)
{
  int len = frame[3] & 0xFF;
  uint8_t *content = frame + 4;
  int status = frame[2] & 0xFF;
  if (status != 0) return;

  int echoed = frame[1] & 0xFF;
  if (echoed == CMD_BASE_INFO) parseBaseInfo(content, len);
  else if (echoed == CMD_CELL_VOLTAGE) { /* cell voltages parsed on demand */ }
}

static void parseBaseInfo(uint8_t *c, int len)
{
  if (len < 23) return;
  BaseInfo &I = lastInfo;
  I.totalVoltage = ((c[0] << 8) | c[1]) / 100.0f;
  I.current = (int16_t)((c[2] << 8) | c[3]) / 100.0f;
  I.remainPower = ((c[4] << 8) | c[5]) / 100.0f;
  I.nominalPower = ((c[6] << 8) | c[7]) / 100.0f;
  I.cycles = (c[8] << 8) | c[9];
  I.soc = c[19] & 0xFF;
  I.cellCount = c[21] & 0xFF;
  I.ntcCount = c[22] & 0xFF;
  for (int i = 0; i < I.ntcCount && i < 4; i++)
  {
    int idx = 23 + i * 2;
    if (idx + 1 < len)
      I.temperatures[i] = (((c[idx] << 8) | c[idx + 1]) - 2731) / 10.0f;
  }
  int tail = I.ntcCount * 2 + 22;
  if (len > tail + 5)
  {
    I.extended = true;
    I.balanceCurrent = ((c[tail + 4] << 8) | c[tail + 5]) / 100.0f;
  }
  hasDataFlag = true;
}

bool connected() { return connectedFlag; }
bool hasData() { return hasDataFlag; }
BaseInfo info() { return lastInfo; }

}
