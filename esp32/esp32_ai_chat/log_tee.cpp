#include "log_tee.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 环形缓冲：放 PSRAM（内部 RAM 紧张，BLE/WiFi/任务栈都在内部；静态 32KB 会挤掉
// 开机阶段 http/mdns 等任务的创建）。PSRAM 不可用时退回 16KB 内部堆。
#define LOG_CAP 32768
static char *rbuf = nullptr;
static size_t wpos = 0;      // 下一个写入位置；wrapped 后也是“最老日志”的起点
static bool wrapped = false;
static SemaphoreHandle_t mtx = nullptr;

void logTeeBegin()
{
  if (!mtx) mtx = xSemaphoreCreateMutex();
  if (!rbuf)
  {
    rbuf = (char *)heap_caps_malloc(LOG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rbuf) rbuf = (char *)heap_caps_malloc(16384, MALLOC_CAP_8BIT);
    wpos = 0;
    wrapped = false;
  }
  logTeePrintf("\n==== 网页日志捕获开启（运行 %lu ms，缓冲 %u KB）====\n",
               (unsigned long)millis(), rbuf ? (unsigned)(heap_caps_get_allocated_size(rbuf) / 1024) : 0);
}

void logTeeWrite(const char *s, size_t n)
{
  if (!s || !n || !rbuf) return;
  if (!mtx) mtx = xSemaphoreCreateMutex();
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (size_t i = 0; i < n; i++)
  {
    rbuf[wpos] = s[i];
    wpos++;
    if (wpos >= LOG_CAP) { wpos = 0; wrapped = true; }
  }
  xSemaphoreGive(mtx);
}

void logTeePrintf(const char *fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  size_t len = (n > (int)sizeof(tmp) - 1) ? sizeof(tmp) - 1 : (size_t)n;
  logTeeWrite(tmp, len);
  Serial.write((const uint8_t *)tmp, len);   // 原样转发真实串口
}

void logTeePrint(const String &s)
{
  logTeeWrite(s.c_str(), s.length());
  Serial.print(s);
}

void logTeeClear()
{
  xSemaphoreTake(mtx, portMAX_DELAY);
  wpos = 0;
  wrapped = false;
  xSemaphoreGive(mtx);
}

String logTeeDump(size_t maxBytes)
{
  if (!mtx) return String();
  xSemaphoreTake(mtx, portMAX_DELAY);
  size_t avail = wrapped ? LOG_CAP : wpos;
  size_t skip = (avail > maxBytes) ? (avail - maxBytes) : 0;
  size_t len = avail - skip;
  size_t idx = wrapped ? (wpos + skip) % LOG_CAP : skip;
  String out;
  out.reserve(len + 8);
  for (size_t i = 0; i < len; i++)
  {
    out += rbuf[idx];
    idx = (idx + 1) % LOG_CAP;
  }
  xSemaphoreGive(mtx);
  return out;
}
