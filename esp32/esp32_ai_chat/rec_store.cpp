#include "rec_store.h"
#include <Preferences.h>

// NVS 键名 ≤15 字符；条目按“最近使用在末尾”维护，超出容量淘汰下标 0（最旧）
static const char *W_NS = "wifis";
static const char *A_NS = "aiv";
#define W_MAX 8
#define A_MAX 6

// ================= WiFi =================
static String wBlob(int i, Preferences &p)
{
  return p.getString(("w" + String(i)).c_str(), "");
}

bool wifiRecUpsert(const String &ssid, const String &pass)
{
  if (!ssid.length()) return false;
  Preferences p;
  p.begin(W_NS, false);
  int n = p.getInt("wn", 0);
  String blob = ssid + "\n" + pass;
  for (int i = 0; i < n; i++)
  {
    if (wBlob(i, p).startsWith(ssid + "\n"))
    {
      p.putString(("w" + String(i)).c_str(), blob.c_str());
      p.end();
      return true;
    }
  }
  if (n >= W_MAX)
  {
    for (int i = 1; i < n; i++)
      p.putString(("w" + String(i - 1)).c_str(), wBlob(i, p).c_str());
    n--;
  }
  p.putString(("w" + String(n)).c_str(), blob.c_str());
  p.putInt("wn", n + 1);
  p.end();
  return true;
}

int wifiRecCount()
{
  Preferences p;
  p.begin(W_NS, true);
  int n = p.getInt("wn", 0);
  p.end();
  return n;
}

bool wifiRecAt(int i, String &ssid, String &pass)
{
  if (i < 0 || i >= wifiRecCount()) return false;
  Preferences p;
  p.begin(W_NS, true);
  String b = wBlob(i, p);
  p.end();
  int sp = b.indexOf('\n');
  if (sp < 0) return false;
  ssid = b.substring(0, sp);
  pass = b.substring(sp + 1);
  return true;
}

bool wifiRecGet(const String &ssid, String &pass)
{
  int n = wifiRecCount();
  Preferences p;
  p.begin(W_NS, true);
  for (int i = 0; i < n; i++)
  {
    String b = wBlob(i, p);
    if (b.startsWith(ssid + "\n"))
    {
      int sp = b.indexOf('\n');
      pass = b.substring(sp + 1);
      p.end();
      return true;
    }
  }
  p.end();
  return false;
}

bool wifiRecForget(const String &ssid)
{
  int n = wifiRecCount();
  int hit = -1;
  Preferences p;
  p.begin(W_NS, true);
  for (int i = 0; i < n; i++)
    if (wBlob(i, p).startsWith(ssid + "\n")) { hit = i; break; }
  p.end();
  if (hit < 0) return false;
  p.begin(W_NS, false);
  for (int i = hit + 1; i < n; i++)
    p.putString(("w" + String(i - 1)).c_str(), wBlob(i, p).c_str());
  p.putInt("wn", n - 1);
  p.end();
  return true;
}

// ================= AI 厂商 =================
static const char SEP = '\x01'; // 字段分隔（模型列表内部用 '\n'，不冲突）

static String aJoin(const AiRec &r)
{
  return r.name + SEP + r.url + SEP + r.key + SEP + r.model + SEP + r.models;
}

static bool aSplit(const String &blob, AiRec &r)
{
  String f[5];
  int idx = 0, start = 0;
  for (int i = 0; i <= blob.length() && idx < 5; i++)
  {
    if (i == blob.length() || blob[i] == SEP)
    {
      f[idx++] = blob.substring(start, i);
      start = i + 1;
    }
  }
  if (idx < 5) return false;
  r.name = f[0]; r.url = f[1]; r.key = f[2]; r.model = f[3]; r.models = f[4];
  return true;
}

bool aiRecUpsert(const AiRec &r)
{
  if (!r.name.length() || r.name == "自定义") return false;
  Preferences p;
  p.begin(A_NS, false);
  int n = p.getInt("vn", 0);
  String blob = aJoin(r);
  for (int i = 0; i < n; i++)
  {
    AiRec old;
    if (aSplit(p.getString(("v" + String(i)).c_str(), ""), old) && old.name == r.name)
    {
      p.putString(("v" + String(i)).c_str(), blob.c_str());
      p.end();
      return true;
    }
  }
  if (n >= A_MAX)
  {
    for (int i = 1; i < n; i++)
      p.putString(("v" + String(i - 1)).c_str(), p.getString(("v" + String(i)).c_str(), "").c_str());
    n--;
  }
  p.putString(("v" + String(n)).c_str(), blob.c_str());
  p.putInt("vn", n + 1);
  p.end();
  return true;
}

int aiRecCount()
{
  Preferences p;
  p.begin(A_NS, true);
  int n = p.getInt("vn", 0);
  p.end();
  return n;
}

bool aiRecAt(int i, AiRec &r)
{
  if (i < 0 || i >= aiRecCount()) return false;
  Preferences p;
  p.begin(A_NS, true);
  bool ok = aSplit(p.getString(("v" + String(i)).c_str(), ""), r);
  p.end();
  return ok;
}

bool aiRecGet(const String &name, AiRec &r)
{
  int n = aiRecCount();
  Preferences p;
  p.begin(A_NS, true);
  for (int i = 0; i < n; i++)
  {
    AiRec t;
    if (aSplit(p.getString(("v" + String(i)).c_str(), ""), t) && t.name == name)
    {
      r = t;
      p.end();
      return true;
    }
  }
  p.end();
  return false;
}
