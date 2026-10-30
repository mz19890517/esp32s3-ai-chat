#include "ota_sd.h"
#include "ota_update.h"
#include "ui.h"
#include "board_config.h"
#include "touch_driver.h"

#include <FS.h>
#include <SD_MMC.h>
#include <Update.h>

/*
 * TF 卡升级（ESP32-S3-Touch-LCD-3.5B）
 *   引脚：SD_MMC 1-bit  CLK = GPIO11  CMD = GPIO10  D0 = GPIO9（微雪官方 demo）
 *   固件：卡根目录 firmware.bin（也兼容 update.bin / firmware/firmware.bin / 任意根目录 *.bin）
 *   开机流程：挂载 → 校验镜像头与 app 描述符 → 3 秒倒计时（可点屏取消）→ 写入备用分区 → 重启
 *   成功后把固件改名 *.bin.done，避免每次开都重复刷同一个文件
 */

#define SD_MOUNT "/sdcard"
#define SD_PIN_CLK 11
#define SD_PIN_CMD 10
#define SD_PIN_D0 9

static bool mounted = false;
static bool writing = false;
static String fwPath;
static String fwVer;
static uint32_t fwSize = 0;
static String stText = "未检测";
static File fwFile;
static uint32_t written = 0;
static uint32_t total = 0;
static uint32_t lastDrawMs = 0;

String otaSdFormatSize(uint32_t bytes)
{
  if (bytes >= 1048576u) return String(bytes / 1048576.0f, 1) + " MB";
  if (bytes >= 1024u) return String(bytes / 1024) + " KB";
  return String(bytes) + " B";
}

bool otaSdMounted() { return mounted; }

bool otaSdMount()
{
  if (mounted) return true;
  SD_MMC.end();
  SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0); // 失败则沿用默认引脚
  if (SD_MMC.begin(SD_MOUNT, true))
  {
    mounted = (SD_MMC.cardType() != CARD_NONE);
  }
  if (!mounted) SD_MMC.end();
  return mounted;
}

void otaSdUnmount()
{
  if (fwFile) fwFile.close();
  SD_MMC.end();
  mounted = false;
}

static bool checkHeader(File &f, String &ver)
{
  if ((uint32_t)f.size() < 0x60u) return false;
  uint8_t h[0x50];
  f.seek(0);
  if (f.read(h, sizeof(h)) != sizeof(h)) return false;
  if (h[0] != 0xE9) return false; // ESP 镜像 magic
  uint32_t m = (uint32_t)h[0x20] | ((uint32_t)h[0x21] << 8) | ((uint32_t)h[0x22] << 16) | ((uint32_t)h[0x23] << 24);
  if (m != 0xABCD5432u) return false; // esp_app_desc_t.magic_word
  char v[33];
  memcpy(v, h + 0x30, 32);
  v[32] = 0;
  for (int i = 0; i < 32; i++)
  {
    if (v[i] < 0x20 || v[i] > 0x7E)
    {
      v[i] = 0;
      break;
    }
  }
  ver = String(v);
  return true;
}

static String describeFw()
{
  String s = fwPath;
  if (fwVer.length()) s += "  v" + fwVer;
  s += "  ";
  s += otaSdFormatSize(fwSize);
  return s;
}

static bool tryPick(const char *path)
{
  if (!SD_MMC.exists(path)) return false;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  String ver;
  bool ok = checkHeader(f, ver);
  uint32_t sz = (uint32_t)f.size();
  f.close();
  if (!ok) return false;
  if (sz > (uint32_t)otaTargetSize())
  {
    stText = String(path) + " 超出分区容量";
    return false;
  }
  fwPath = path;
  fwSize = sz;
  fwVer = ver;
  return true;
}

bool otaSdScan()
{
  fwPath = "";
  fwVer = "";
  fwSize = 0;
  if (!otaSdMount())
  {
    stText = "未插入 TF 卡或挂载失败";
    return false;
  }

  static const char *cand[] = {"/firmware.bin", "/update.bin", "/firmware/firmware.bin"};
  for (uint8_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++)
  {
    if (tryPick(cand[i]))
    {
      stText = describeFw();
      return true;
    }
  }

  File dir = SD_MMC.open("/");
  if (dir && dir.isDirectory())
  {
    File e = dir.openNextFile();
    while (e)
    {
      bool isDir = e.isDirectory();
      String nm = e.name();
      e.close();
      if (!isDir)
      {
        if (nm.endsWith(".bin") || nm.endsWith(".BIN"))
        {
          String p = nm.startsWith("/") ? nm : ("/" + nm);
          if (tryPick(p.c_str()))
          {
            dir.close();
            stText = describeFw();
            return true;
          }
        }
      }
      e = dir.openNextFile();
    }
    dir.close();
  }
  stText = "TF 卡内未找到固件";
  return false;
}

String otaSdStatus() { return stText; }
String otaSdFileName() { return fwPath; }
uint32_t otaSdFileSize() { return fwSize; }
String otaSdFileVersion() { return fwVer; }

bool otaSdStartUpdate()
{
  if (otaBusy())
  {
    stText = "已有升级任务正在进行";
    return false;
  }
  if (!fwPath.length())
  {
    if (!otaSdScan()) return false;
  }
  fwFile = SD_MMC.open(fwPath, FILE_READ);
  if (!fwFile)
  {
    stText = "无法打开 " + fwPath;
    return false;
  }
  total = (uint32_t)fwFile.size();
  written = 0;
  if (total < 16384u || !Update.begin(total, U_FLASH))
  {
    fwFile.close();
    stText = String("无法开始写入，错误 ") + String(Update.getError());
    return false;
  }
  writing = true;
  otaSetChannel(OCH_SD);
  otaReport(OST_RUNNING, "TF 卡升级 " + fwVer, 0);
  return true;
}

static bool sdStep()
{
  if (!writing) return false;

  uint8_t buf[4096];
  int n = (int)fwFile.read(buf, sizeof(buf));
  if (n > 0)
  {
    if (Update.write(buf, (size_t)n) != (size_t)n)
    {
      writing = false;
      fwFile.close();
      Update.abort();
      stText = "写入闪存失败";
      otaReport(OST_FAIL, "TF 卡升级失败: 写入闪存出错", 0);
      return false;
    }
    written += (uint32_t)n;
    int pct = (int)((uint64_t)written * 100 / (uint64_t)(total ? total : 1));
    otaReport(OST_RUNNING, "TF 卡升级 " + fwVer, pct);
  }

  if (n > 0 && written < total) return true;

  writing = false;
  fwFile.close();
  if (written != total || !Update.end(true) || !Update.isFinished())
  {
    Update.abort();
    stText = "固件校验失败";
    otaReport(OST_FAIL, "TF 卡升级失败: 固件校验不通过", 0);
    return false;
  }

  String done = fwPath + ".done";
  if (SD_MMC.exists(done)) SD_MMC.remove(done);
  SD_MMC.rename(fwPath, done);
  stText = "升级完成，固件已改名为 " + done;
  otaReport(OST_DONE, "TF 卡升级完成，即将重启", 100);
  otaRequestRestart(1500);
  return false;
}

void otaSdPoll() { sdStep(); }

void otaSdAbort()
{
  if (!writing) return;
  writing = false;
  Update.abort();
  if (fwFile) fwFile.close();
  stText = "已取消";
  otaReport(OST_FAIL, "TF 卡升级已取消", 0);
}

/* ---------------- 开机检测界面 ---------------- */

static void bootScreen(const char *title, const String &l1, const String &l2, int pct, bool bar)
{
  gfx->fillScreen(COL_BG);
  gfx->fillRect(0, 0, SCREEN_W, TITLE_H, COL_PANEL);
  gfx->drawFastHLine(0, TITLE_H, SCREEN_W, COL_LINE);
  int tw = textWidth(title);
  drawTextAt((SCREEN_W - tw) / 2, centerBaseY(0, TITLE_H), title, COL_TEXT, COL_PANEL);
  drawTextAt(16, 100, fitTail(l1, 288), COL_TEXT, COL_BG);
  drawTextAt(16, 128, fitTail(l2, 288), COL_SUB, COL_BG);
  if (bar)
  {
    gfx->drawRoundRect(16, 160, 288, 18, 9, COL_LINE);
    int w = (int)((long)(288 - 4) * pct / 100);
    if (w > 0) gfx->fillRoundRect(18, 162, w, 14, 7, COL_ACCENT);
    drawTextAt(16, 206, String(pct) + " %", COL_SUB, COL_BG);
  }
  gfx->flush();
}

bool otaSdBootCheck()
{
  if (!otaSdMount()) return false;
  if (!otaSdScan()) return false;
  if (!fwSize) return false;

  bool same = fwVer.length() && fwVer == String(FW_VERSION);
  for (int i = 3; i > 0; i--)
  {
    String hint = String("版本 ") + fwVer + (same ? "（与当前相同）" : "");
    hint += "  ";
    hint += String(i);
    hint += " 秒后自动升级，点击屏幕取消";
    bootScreen("检测到 TF 卡固件", describeFw(), hint, 0, false);
    uint32_t t0 = millis();
    while (millis() - t0 < 1000)
    {
      TouchPoint tp;
      if (touchRead(tp) && tp.pressed)
      {
        bootScreen("已跳过 TF 卡升级", fwPath, "继续正常启动", 0, false);
        delay(600);
        return false;
      }
      delay(20);
    }
  }

  if (!otaSdStartUpdate())
  {
    bootScreen("TF 卡升级未开始", stText, "继续正常启动", 0, false);
    delay(1600);
    return false;
  }

  lastDrawMs = 0;
  while (sdStep())
  {
    if (millis() - lastDrawMs > 150)
    {
      lastDrawMs = millis();
      int pct = (int)((uint64_t)written * 100 / (uint64_t)(total ? total : 1));
      bootScreen("TF 卡升级中",
                 "写入 " + otaSdFormatSize(written) + " / " + otaSdFormatSize(total),
                 "请勿断电或拔卡", pct, true);
    }
    delay(1);
  }

  if (otaState() == OST_DONE)
  {
    bootScreen("升级完成", "正在重启进入新固件", "", 100, true);
    delay(1500);
    ESP.restart();
    return true;
  }

  bootScreen("TF 卡升级失败", otaMessage(), "继续正常启动", 0, false);
  delay(1800);
  return false;
}
