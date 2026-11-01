#include "config_store.h"
#include "secrets.h"
#include "log_tee.h"
#include <Preferences.h>

Config cfg;
static Preferences prefs;

void cfgLoad()
{
  prefs.begin("aichat", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.wpass = prefs.getString("wpass", "");
  cfg.url = prefs.getString("url", "");
  cfg.key = prefs.getString("key", "");
  cfg.model = prefs.getString("model", "");
  cfg.otaUrl = prefs.getString("otaurl", "");
  prefs.end();
  if (!cfg.ssid.length()) cfg.ssid = DEFAULT_SSID;
  if (!cfg.wpass.length()) cfg.wpass = DEFAULT_WPASS;
  if (!cfg.key.length()) cfg.key = DEFAULT_API_KEY;
  if (!cfg.url.length()) cfg.url = DEFAULT_URL;
  if (!cfg.model.length()) cfg.model = DEFAULT_MODEL;
}

void cfgSaveWiFi()
{
  prefs.begin("aichat", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("wpass", cfg.wpass);
  prefs.end();
}

void cfgSaveApi()
{
  logTeePrintf("[cfg] saveApi url=%s model=%s\n", cfg.url.c_str(), cfg.model.c_str());   // 崩溃锚点：保存动作
  prefs.begin("aichat", false);
  prefs.putString("url", cfg.url);
  prefs.putString("key", cfg.key);
  prefs.putString("model", cfg.model);
  prefs.end();
}

void cfgSaveOtaUrl()
{
  prefs.begin("aichat", false);
  prefs.putString("otaurl", cfg.otaUrl);
  prefs.end();
}

String apiEndpoint(const String &base)
{
  String u = base;
  u.trim();
  while (u.length() && u.endsWith("/"))
    u.remove(u.length() - 1);
  String low = u;
  low.toLowerCase();
  if (low.endsWith("/chat/completions")) return u;
  return u + "/chat/completions";
}

// 基础地址归一化：用户/记录里误填完整聊天端点时剥掉后缀。
// 聊天走 apiEndpoint 自动补全，拉模型要拼 /models —— 完整端点会让拉模型 404
//（曾复现：两次拉模型之间 cfg.url 变成完整端点，第二次 GET .../chat/completions -> 404）。
// 所以所有写 cfg.url 的入口都先过这里，运行期任何残留也能被拉模型入口兜底。
String apiNormalizeBase(const String &base)
{
  String u = base;
  u.trim();
  while (u.length() && u.endsWith("/"))
    u.remove(u.length() - 1);
  String low = u;
  low.toLowerCase();
  const char *SUF = "/chat/completions";
  if (low.endsWith(SUF))
  {
    logTeePrintf("[cfg] strip chat suffix: %s\n", u.c_str());
    logTeePersistSave();
    u.remove(u.length() - strlen(SUF));
  }
  return u;
}
