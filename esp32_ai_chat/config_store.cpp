#include "config_store.h"
#include "secrets.h"
#include <Preferences.h>

Config cfg;
static Preferences prefs;

void cfgLoad()
{
  prefs.begin("aichat", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.wpass = prefs.getString("wpass", "");
  cfg.url = prefs.getString("url", "https://api.deepseek.com");
  cfg.key = prefs.getString("key", "");
  cfg.model = prefs.getString("model", "deepseek-chat");
  prefs.end();
  if (!cfg.ssid.length()) cfg.ssid = DEFAULT_SSID;
  if (!cfg.wpass.length()) cfg.wpass = DEFAULT_WPASS;
  if (!cfg.key.length()) cfg.key = DEFAULT_API_KEY;
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
  prefs.begin("aichat", false);
  prefs.putString("url", cfg.url);
  prefs.putString("key", cfg.key);
  prefs.putString("model", cfg.model);
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
