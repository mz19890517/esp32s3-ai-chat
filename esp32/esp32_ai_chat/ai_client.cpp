#include "ai_client.h"
#include "config_store.h"
#include "log_tee.h"
#include "serial_link.h"
#include "ota_update.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *SYSTEM_PROMPT = "你是一个运行在ESP32设备上的AI助手。请用简洁准确的中文回答。";
static const int MAX_HIST = 20;
static const size_t MAX_REPLY = 8000;

static SemaphoreHandle_t mtx;
static SemaphoreHandle_t sem;
static std::vector<AiMsg> reqHist;
static String reqUrl, reqKey, reqModel, reqSys;
static volatile bool busy = false;
static volatile bool done = false;
static String resultReply, resultErr;

bool aiBusy() { return busy; }

// 手动读响应体（fix13c）：替代 http.getString()，根治 rst=6。
// getString 的 chunked 内部循环（HTTPClient writeToStreamDataBlock）在「连接仍在、
// 服务器停发」时 readBytes 返 0 不退出 → 无限等待；AI 分析的长回复必现，聊天偶发。
// 本实现每步要么有进展要么立即报错：socket 20s 读超时 + 总限时 90s + 体上限 128KB，
// chunked 与 Content-Length 两种编码都支持。
static bool httpReadBody(WiFiClient &cli, bool chunked, int contentLen, String &out, String &oerr)
{
  out = "";
  const unsigned long deadline = millis() + 90000;   // 总限时
  const int CAP = 131072;                            // 体上限（与拒读阈值一致）

  // 读一行（\n 终止，不含 CRLF）。空串 = 超时/断流（chunk 头不可能为空）
  auto readLine = [&]() -> String
  {
    String line;
    while (millis() < deadline)
    {
      int c = cli.read();   // 底层 socket 已设 20s 读超时
      if (c < 0) return String("");
      if (c == '\n') return line;
      if (c != '\r') line += (char)c;
    }
    return String("");
  };

  // 精确读 n 字节：每轮必须读到数据，否则立即失败（绝不无限重试）
  auto readExact = [&](char *buf, int n) -> bool
  {
    int got = 0;
    while (got < n)
    {
      if (millis() >= deadline) return false;
      int r = cli.read((uint8_t *)buf + got, n - got);
      if (r <= 0) return false;
      got += r;
    }
    return true;
  };

  // 读 n 字节追加进 out：512B 分段 + 先 reserve，避免 String 扩容双倍峰值
  auto readInto = [&](int n) -> bool
  {
    if (out.length() + n > CAP) { oerr = "响应过大（超128KB，已拒读）"; return false; }
    if (!out.reserve(out.length() + n + 1)) { oerr = "内存不足"; return false; }
    char buf[512];
    while (n > 0)
    {
      int want = n < (int)sizeof(buf) ? n : (int)sizeof(buf);
      if (!readExact(buf, want)) { oerr = "读响应超时（网络不稳）"; return false; }
      out.concat(buf, want);
      n -= want;
    }
    return true;
  };

  if (chunked)
  {
    for (;;)   // 所有分支均 return
    {
      String hdr = readLine();
      if (!hdr.length()) { oerr = "读响应超时（chunk头）"; return false; }
      int clen = (int)strtol(hdr.c_str(), nullptr, 16);
      if (clen < 0) { oerr = "chunk头异常: " + hdr; return false; }
      if (clen == 0) return true;   // 终结块，收工
      if (!readInto(clen)) return false;
      char crlf[2];
      if (!readExact(crlf, 2)) { oerr = "读响应超时（chunk尾）"; return false; }
    }
  }
  else
  {
    if (contentLen <= 0) { oerr = "响应无长度信息"; return false; }
    return readInto(contentLen);
  }
}

// 有界 SSE 流式读体（fix13e）：替代 200 响应的整包读取。
// 实测非流式长回复（AI 分析生成 83-94s）时，headers 到达后服务器/中间层在静默窗口
// 掐断连接，body 永不到达（旧固件 getString 的死循环在此 rst=6，fix13c 在此读 0B）。
// SSE 从第 1 秒起持续吐数据，无静默窗口。本实现边解 chunked 边按行解析 delta，
// 每步要么有进展要么立即报错：单读 90s（容忍思考静默）+ 总限时 5min + 单行 8KB 上限。
static bool httpReadSse(WiFiClient &cli, String &reply, String &oerr)
{
  const unsigned long deadline = millis() + 300000;
  long chunkLeft = 0;      // 当前 chunk 剩余字节
  bool needCrlf = false;   // 当前 chunk 数据后待吞的 \r\n
  String line;             // SSE 行组装缓冲（按行清空，峰值只有单行）

  // 处理一行 SSE；返回 false = 协议/服务端错误（终止）
  auto feedLine = [&](String &l, bool &done) -> bool
  {
    l.trim();
    if (!l.length() || l[0] == ':') return true;   // 空行 / keep-alive 注释
    if (l.startsWith("data:"))
    {
      String p = l.substring(5);
      p.trim();
      if (p == "[DONE]") { done = true; return true; }
      JsonDocument rd;
      if (deserializeJson(rd, p)) return true;   // 单行坏 JSON：跳过
      const char *c = rd["choices"][0]["delta"]["content"] | "";
      if (c[0] && reply.length() < MAX_REPLY) reply += c;
      const char *em = rd["error"]["message"] | "";
      if (em[0]) { oerr = String("AI 流式错误: ") + em; return false; }
    }
    return true;
  };

  for (;;)
  {
    if (millis() >= deadline) { oerr = "读响应超时（总限时）"; return false; }
    if (needCrlf)
    {
      for (int k = 0; k < 2; k++)
      {
        int c = cli.read();
        if (c < 0) { oerr = "读响应超时（chunk尾）"; return false; }
      }
      needCrlf = false;
      continue;
    }
    if (chunkLeft == 0)
    {
      String hdr;
      bool okline = false;
      while (millis() < deadline)
      {
        int c = cli.read();
        if (c < 0) break;
        if (c == '\n') { okline = true; break; }
        if (c != '\r') hdr += (char)c;
      }
      if (!okline || !hdr.length()) { oerr = "读响应超时（chunk头）"; return false; }
      long clen = strtol(hdr.c_str(), nullptr, 16);
      if (clen < 0) { oerr = "chunk头异常: " + hdr; return false; }
      if (clen == 0) return true;   // 终结块（流自然结束，含未发 [DONE] 的情形）
      chunkLeft = clen;
      continue;
    }
    int c = cli.read();   // chunk 数据字节（底层 socket 超时 90s）
    if (c < 0) { oerr = "读响应超时（数据）"; return false; }
    chunkLeft--;
    if (chunkLeft == 0) needCrlf = true;
    if (c == '\n')
    {
      bool done = false;
      if (!feedLine(line, done)) return false;
      line = "";
      if (done) return true;   // 收到 [DONE]
    }
    else if (c != '\r')
    {
      if (line.length() > 8192) { oerr = "SSE 行超长"; return false; }
      line += (char)c;
    }
  }
}

static void doRequest(const std::vector<AiMsg> &hist, const String &url, const String &key, const String &model, const String &sysPrompt, String &reply, String &err)
{
  String body;
  {
    // JSON 池用完即释：内部堆是 TLS 握手的刚需，能省一点是一点
    JsonDocument doc;
    doc["model"] = model;
    doc["stream"] = false;   // fix13g：流式实测 headers 后 body 死寂（errno=11 EAGAIN，got=0），回到验证过的非流式整包读
  doc["max_tokens"] = 700;   // fix13g：限制输出长度。生成 >75s 的长回复会被掐（聊天短回复 61-64s 必成功），700 token ≈ 49-54s 在安全区
  JsonArray arr = doc["messages"].to<JsonArray>();
    JsonObject sys = arr.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = sysPrompt.length() ? sysPrompt : SYSTEM_PROMPT;
    for (auto &m : hist)
    {
      JsonObject o = arr.add<JsonObject>();
      o["role"] = m.role;
      o["content"] = m.content;
    }
    serializeJson(doc, body);
  }   // doc 的内存池在此释放
  String uhead = url.substring(0, 60);
  logTeePrintf("[ai] chat POST %s body=%u tls=%d hist=%u\n", uhead.c_str(),
               (unsigned)body.length(), (int)url.startsWith("https"), (unsigned)hist.size());
  logTeePrintf("[ai] req head: %.120s\n", body.c_str());   // 实锤请求体：stream 字段在不在
  logTeePersistSave();   // 存档点①：JSON 已构建

  // fix13：聊天不再无条件挂 BLE。fix8 后 mbedtls 全在 PSRAM，TLS 的内部堆需求只剩几 KB，
  // BLE 常驻（largest ~16KB）大概率够握手。而「挂起→重建」每轮漏 ~1.4KB 内部堆（实测
  // resumed heap 42092→40648→39836 递减），频繁聊天加速累积；且「响应读完→重建完成」
  // 的盲区正是两次 rst=6 挂死的发生地。改为：BLE 常驻直接握手，仅当失败且是内存类错误
  // （tlserr 含 Memory/alloc）时挂起重试一次；退出路径由 BleGuard 兜底恢复（含异常残留）。
  struct BleGuard
  {
    ~BleGuard()
    {
      if (!slBleDown() || otaBleActive()) return;
      slBleResume();   // loop 任务内完整重建（含蓝牙 OTA 服务 + 广播）
      logTeePrintf("[ai] ble resumed (chat)\n");
    }
  } bg;

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
  const char *hkeys[] = {"Transfer-Encoding", "Connection", "Content-Type"};
  char eb[96] = {0};
  int code = 0;
  for (int att = 0; att < 2; att++)
  {
    if (!http.begin(cli, url))
    {
      err = "无法连接到服务器";
      return;
    }
    http.setTimeout(90000);
    http.collectHeaders(hkeys, 3);   // 读体前可打印 TE/Connection/CT：chunked 与流式是否生效一眼可见
    http.addHeader("Content-Type", "application/json");
    if (key.length()) http.addHeader("Authorization", "Bearer " + key);
    logTeePrintf("[ai] chat pre-POST heap=%u largest=%u att=%d bleDown=%d\n",
                 (unsigned)ESP.getFreeHeap(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 att, (int)slBleDown());
    logTeePersistSave();   // 存档点③：即将 http.POST（TLS 握手窗口）
    // 请求体挪 PSRAM：不占内部堆
    char *pbody = (char *)heap_caps_malloc(body.length() + 1, MALLOC_CAP_SPIRAM);
    if (pbody)
    {
      memcpy(pbody, body.c_str(), body.length() + 1);
      code = http.POST((uint8_t *)pbody, body.length());
      heap_caps_free(pbody);
    }
    else
      code = http.POST(body);
    logTeePrintf("[ai] chat POST -> %d (%lums) att=%d\n", code, (unsigned long)millis() % 100000, att);
    logTeePersistSave();   // 存档点④：POST 已返回（之后再崩 = 读体/解析/BLE 重建盲区）
    if (code > 0) break;
    // 打印 mbedtls 具体错误码：区分内存不足（ALLOC_FAILED）与连接/TLS 协议错误
    if (tls) scl.lastError(eb, sizeof(eb));
    logTeePrintf("[ai] chat POST fail tlserr=%s largest=%u\n", eb,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    logTeePersistSave();
    bool memFail = strstr(eb, "Memory") || strstr(eb, "alloc");
    if (att == 1 || !memFail || otaBleActive())
    {
      err = "网络错误: " + http.errorToString(code);
      http.end();
      return;
    }
    // 内存类失败才挂 BLE 重试一次（largest 从 ~16KB 升到 ~30KB+）
    logTeePrintf("[ai] mem fail -> suspend ble, retry\n");
    logTeePersistSave();   // 高危窗口（BLE 即将释放）存档
    slBleSuspend();
    http.end();
  }
  if (code <= 0) return;   // 失败路径已在循环内 return，防御性兜底

  // —— 读响应体：fix13e 改流式（SSE）。长回复（AI 分析）非流式时服务器/中间层
  // ~90s 静默后掐断连接，body 永不到达（POST 83-94s 后 body 0B 实锤；旧固件在此 rst=6）——
  int respLen = http.getSize();
  logTeePrintf("[ai] pre-read size=%d te=%s conn=%s\n", respLen,
               http.header("Transfer-Encoding").c_str(), http.header("Connection").c_str());
  logTeePersistSave();   // 存档点⑤a：即将读体（再崩 = 读体阶段）
  if (code == 200)
  {
    bool sse = http.header("Content-Type").indexOf("event-stream") >= 0;
    logTeePrintf("[ai] resp ct=%s sse=%d\n", http.header("Content-Type").c_str(), (int)sse);
    if (sse)
    {
      cli.setTimeout(45000);   // 单读超时 45s 快失败：实测服务器 headers 后 body 死寂时不值得干等 90s
      if (!httpReadSse(cli, reply, err))
        logTeePrintf("[ai] sse fail got=%u errno=%d\n", (unsigned)reply.length(), errno);
    }
    else
    {
      // fix13i：手动读优先（有界+超时防崩，getString 无界在读体阶段随机 rst），0B 时 getString 兜底
      // （fix13g2 时代手动读恒 0B / fix13h getString 能读，事后判定为服务器时段波动而非读法差异）
      cli.setTimeout(20000);
      String resp;
      {
        bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
        String berr;
        if (!httpReadBody(cli, chunked, respLen, resp, berr))
          err = berr;
        if (resp.length() == 0)
        {
          logTeePrintf("[ai] body 0B avail=%d conn=%d errno=%d -> getString 兜底\n",
                       cli.available(), (int)cli.connected(), errno);
          errno = 0;
          resp = http.getString();
          err = "";
        }
      }
      int jlen = (int)resp.length();
      logTeePrintf("[ai] body %dB head=%.80s\n", jlen, resp.c_str());
      if (!err.length())
      {
        JsonDocument rd;
        if (!deserializeJson(rd, resp.c_str(), jlen))
        {
          const char *c = rd["choices"][0]["message"]["content"] | "";
          reply = String(c);
          reply.trim();
          if (!reply.length())
          {
            // fix13i：思考模型（deepseek-flash）在 max_tokens 被思考耗尽时 content 为空，
            // 思考文本在 reasoning_content——兜底展示，用户至少能看到分析过程
            const char *r = rd["choices"][0]["message"]["reasoning_content"] | "";
            reply = String(r);
            reply.trim();
            if (reply.length())
              logTeePrintf("[ai] content empty, use reasoning_content\n");
          }
          if (reply.length() > MAX_REPLY) reply = reply.substring(0, MAX_REPLY);
        }
        else
          err = "解析失败";
      }
    }
    if (!err.length() && !reply.length()) err = "AI 返回了空回复";
    logTeePrintf("[ai] read done reply=%u err=%s\n", (unsigned)reply.length(), err.length() ? err.c_str() : "-");
    logTeePersistSave();   // 存档点⑤b
  }
  else
  {
    // 非 200：错误体是小 JSON，整包读（httpReadBody）后提取 error.message
    cli.setTimeout(20000);
    String resp;
    {
      bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
      String berr;
      if (!httpReadBody(cli, chunked, respLen, resp, berr))
        err = berr;
    }
    int jlen = (int)resp.length();
    logTeePrintf("[ai] body %dB heap\n", jlen);
    logTeePersistSave();   // 存档点⑤b
    if (!err.length())
    {
      JsonDocument rd;
      if (!deserializeJson(rd, resp.c_str(), jlen))
      {
        const char *emsg = rd["error"]["message"] | "";
        String msg(emsg);
        if (msg.length()) err = "HTTP " + String(code) + ": " + msg;
        else err = "HTTP " + String(code);
        if (err.length() > 300) err = err.substring(0, 300);
      }
      else
        err = "HTTP " + String(code);
    }
  }
  http.end();
  logTeePrintf("[ai] chat done reply=%u err=%s\n", (unsigned)reply.length(),
               err.length() ? err.c_str() : "-");
  logTeePersistSave();   // 存档点⑥：即将返回并触发 BLE 重建（再崩 = 重建阶段）
}

static bool aiFetchModelsImpl(const String &baseUrl, const String &apiKey, String &models, String &err);

// 请求种类：0=聊天，1=拉取模型列表（共用同一个 16KB 栈任务，不额外占内存）
static int reqKind = 0;
static String resultModels;
static bool resultOk = false;

static void taskFn(void *)
{
  for (;;)
  {
    xSemaphoreTake(sem, portMAX_DELAY);
    std::vector<AiMsg> hist;
    String url, key, model, sysp;
    int kind;
    xSemaphoreTake(mtx, portMAX_DELAY);
    hist = reqHist;
    url = reqUrl;
    key = reqKey;
    model = reqModel;
    sysp = reqSys;
    kind = reqKind;
    xSemaphoreGive(mtx);

    String reply, err, models;
    bool ok = true;
    logTeePrintf("[ai] task kind=%d begin\n", kind);   // 崩溃锚点：本行进快照 = 崩在 AI 流程内，否则在 UI/loop
    logTeePersistSave();   // 飞行记录仪：请求开始，崩溃现场从这拍起
    if (kind == 1)
      ok = aiFetchModelsImpl(url, key, models, err);
    else
      doRequest(hist, url, key, model, sysp, reply, err);
    logTeePersistClear();  // 请求完整走完（无论成败），现场作废

    xSemaphoreTake(mtx, portMAX_DELAY);
    resultReply = reply;
    resultErr = err;
    resultModels = models;
    resultOk = ok;
    done = true;
    busy = false;
    xSemaphoreGive(mtx);
  }
}

void aiBegin()
{
  // mbedtls 全局分配器指到 PSRAM：TLS 握手要 ~33KB 连续内部内存（in/out 缓冲 16KB×2 + 证书），
  // BLE 挂起后内部最大块只剩 ~31-34KB，多轮「挂起→TLS→重建」循环碎片化后必然
  // "SSL - Memory allocation failed"（已用 lastError 实锤）。挪 PSRAM 后内部堆需求降到几 KB，根治。
  // free 用系统 free：IDF 的 free 即 heap_caps_free，对旧的内部分配块同样有效（mixed state 安全）。
  mbedtls_platform_set_calloc_free(
      [](size_t n, size_t sz) -> void *
      {
        void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_calloc(n, sz, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
        return p;
      },
      free);
  mtx = xSemaphoreCreateMutex();
  sem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(taskFn, "ai", 16384, nullptr, 1, nullptr, 0);
}

void aiStart(const std::vector<AiMsg> &history, const String &url, const String &key, const String &model)
{
  aiStartEx(history, url, key, model, "");
}

void aiStartEx(const std::vector<AiMsg> &history, const String &url, const String &key, const String &model, const String &sysPrompt)
{
  xSemaphoreTake(mtx, portMAX_DELAY);
  reqHist = history;
  reqUrl = url;
  reqKey = key;
  reqModel = model;
  reqSys = sysPrompt;
  reqKind = 0;   // 关键！reqKind 只在拉模型路径被置 1，聊天复用本函数时若不归零，
                 // 会残留上一次的 1，聊天被当成拉模型执行 → 结果 reply/err 恒空 → 空气泡
  done = false;
  busy = true;
  xSemaphoreGive(mtx);
  xSemaphoreGive(sem);
}

bool aiPoll(String &reply, String &err)
{
  if (!done) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!done)
  {
    xSemaphoreGive(mtx);
    return false;
  }
  reply = resultReply;
  err = resultErr;
  done = false;
  xSemaphoreGive(mtx);
  return true;
}

// 实际拉取逻辑（必须在足够大的任务栈里跑：TLS 握手吃栈，loop 栈 8KB 会握不上手）
static bool aiFetchModelsImpl(const String &baseUrl, const String &apiKey, String &models, String &err)
{
  models = "";
  String u = apiNormalizeBase(baseUrl);   // 兼容误填完整端点：剥掉 /chat/completions 再拼 /models
  if (!u.length()) { err = "地址为空"; return false; }

  // 先单独诊断 DNS：-1 连接失败时先分清是“解析不到域名”还是“TLS 握手失败”
  String host = u;
  host.remove(0, host.indexOf("//") + 2);
  int sp = host.indexOf('/');
  if (sp >= 0) host.remove(sp);
  int cp = host.indexOf(':');
  if (cp >= 0) host.remove(cp);
  IPAddress rip;
  unsigned long t0 = millis();
  int dnsr = WiFi.hostByName(host.c_str(), rip);
  logTeePrintf("[ai] dns %s -> %s (r=%d, %lums)\n", host.c_str(),
               rip.toString().c_str(), dnsr, (unsigned long)(millis() - t0));
  logTeePersistSave();   // 存档点①：DNS 已完成（崩溃在 slBleSuspend 内则停在这）
  if (dnsr != 1)
  {
    err = "域名解析失败（检查设备能否上网）";
    return false;
  }

  // TLS 握手峰值要 ~40KB 内部内存，BLE 常驻时最大空闲块只有 ~16KB，握手必然 -1。
  // 所以先临时释放 BLE（网页蓝牙会断开，几秒后自动恢复），所有退出路径由 BleGuard 恢复。
  struct BleGuard
  {
    bool down = false;
    ~BleGuard()
    {
      if (!down) return;
      slBleResume();   // loop 任务内完整重建（含蓝牙 OTA 服务 + 广播）
      logTeePrintf("[ai] ble resumed\n");
    }
  } bg;

  if (u.startsWith("https"))
  {
    if (otaBleActive()) { err = "蓝牙升级进行中，请稍后再试"; return false; }
    slBleSuspend();
    bg.down = true;
    logTeePrintf("[ai] ble dropped, largest=%u\n",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    logTeePersistSave();   // 高危窗口（BLE 已释放、即将 TLS）存档
  }

  WiFiClientSecure scl;
  WiFiClient pcl;
  bool tls = u.startsWith("https");
  WiFiClient &cli = tls ? static_cast<WiFiClient &>(scl) : static_cast<WiFiClient &>(pcl);
  if (tls)
  {
    scl.setInsecure();
    scl.setHandshakeTimeout(15);
  }
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(cli, u + "/models"))
  {
    err = "地址无效";
    return false;
  }
  if (apiKey.length()) http.addHeader("Authorization", "Bearer " + apiKey);
  const char *mk[] = {"Transfer-Encoding"};   // 必须在 GET 前 collect，读体时才能判断 chunked
  http.collectHeaders(mk, 1);
  logTeePrintf("[ai] pre-GET heap=%u largest=%u\n",
               (unsigned)ESP.getFreeHeap(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  logTeePersistSave();   // 存档点③：即将 http.GET（TLS 握手窗口）
  int code = http.GET();
  logTeePrintf("[ai] models GET %s -> %d\n", u.c_str(), code);
  logTeePersistSave();   // 存档点④：GET 已返回（之后再崩 = 解析阶段）
  if (code <= 0)
  {
    char eb[96] = {0};
    if (tls) scl.lastError(eb, sizeof(eb));
    logTeePrintf("[ai] models GET fail tlserr=%s largest=%u\n", eb,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    logTeePersistSave();
    err = "网络错误: " + http.errorToString(code);
    http.end();
    return false;
  }
  if (code != 200)
  {
    err = "HTTP " + String(code) + (code == 401 ? "（Key 不对？）" : "");
    http.end();
    return false;
  }
  String resp;
  const char *jtxt;
  int jlen;
  {
    cli.setTimeout(20000);   // fix13c：拉模型同样改手动读体（getString 的 chunked 循环有 rst=6 风险）
    String berr;
    bool chunked = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
    if (!httpReadBody(cli, chunked, http.getSize(), resp, berr))
    {
      err = berr;
      http.end();
      return false;
    }
    jtxt = resp.c_str();
    jlen = (int)resp.length();
  }
  http.end();
  if (jlen > 120) logTeePrintf("[ai] models resp head: %.120s\n", jtxt);

  // 解析段包作用域：/models 响应可达几十 KB，JSON 池用完即释，不占内部堆到函数尾
  int cnt = 0;
  {
    JsonDocument rd;
    DeserializationError e = deserializeJson(rd, jtxt, jlen);
    if (e)
    {
      err = "响应解析失败";
      return false;
    }
    const char *paths[] = {"data", "models", "model"};
    for (const char *path : paths)
    {
      JsonArray arr = rd[path].as<JsonArray>();
      if (arr.isNull()) continue;
      for (JsonVariant m : arr)
      {
        String id;
        if (m.is<JsonObject>()) id = m["id"] | "";
        else if (m.is<const char *>()) id = m.as<const char *>();
        if (!id.length()) continue;
        if (models.length()) models += "\n";
        models += id;
        if (++cnt >= 24 || models.length() > 1500) break;   // 控制体积（NVS blob 有限）
      }
      if (cnt) break;
    }
  }
  if (!cnt)
  {
    err = "未找到模型列表";
    return false;
  }
  return true;
}

// —— 模型拉取：复用 AI 任务（16KB 栈），loop 投递请求并等待结果 ——
bool aiFetchModels(const String &baseUrl, const String &apiKey, String &models, String &err)
{
  if (!sem || !mtx)
  {
    err = "内部错误：未初始化";
    return false;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (busy)
  {
    xSemaphoreGive(mtx);
    err = "AI 正忙，稍后再试";
    return false;
  }
  reqHist.clear();
  reqSys = "";
  reqUrl = baseUrl;
  reqKey = apiKey;
  reqModel = "";
  reqKind = 1;
  done = false;
  busy = true;
  xSemaphoreGive(mtx);
  xSemaphoreGive(sem);

  uint32_t t0 = millis();
  bool d = false;
  while (millis() - t0 < 40000)
  {
    vTaskDelay(pdMS_TO_TICKS(60));
    // 本函数跑在 loop 任务里等 AI 任务结果；期间 loop 主循环停摆，
    // AI 任务的 BLE 挂起/恢复请求（req/ack）会没人处理而超时。
    // 这里代泵一轮 slPoll，保证挂起能及时执行（同任务顺序调用，无并发问题）。
    slPoll();
    xSemaphoreTake(mtx, portMAX_DELAY);
    d = done;
    xSemaphoreGive(mtx);
    if (d) break;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool ok = d && resultOk;
  models = resultModels;
  err = d ? resultErr : String("等待超时");
  done = false;   // 结果已消费
  xSemaphoreGive(mtx);
  return ok;
}
