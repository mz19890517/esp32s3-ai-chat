#pragma once
#include <Arduino.h>

// 记录存储（NVS）：像手机一样的“记住”能力
//  - WiFi 密码：连过即存，最多 8 条，旧的自动淘汰；切回来免输入
//  - AI 厂商：url / key / model / 已拉取的模型列表一起记，最多 6 条，切厂商即恢复

// ---- WiFi 记录 ----
bool wifiRecUpsert(const String &ssid, const String &pass); // 连接成功后调用
int  wifiRecCount();
bool wifiRecAt(int i, String &ssid, String &pass);
bool wifiRecGet(const String &ssid, String &pass);          // 是否记住过
bool wifiRecForget(const String &ssid);

// ---- AI 厂商记录 ----
struct AiRec
{
  String name;   // 厂商名（与 cfg_table 预置一致；"自定义" 不记录）
  String url;    // 接口基础地址（如 https://api.deepseek.com）
  String key;    // API Key
  String model;  // 当前选中的模型
  String models; // 已拉取的模型 id 列表，'\n' 分隔
};
bool aiRecUpsert(const AiRec &r);
int  aiRecCount();
bool aiRecAt(int i, AiRec &r);
bool aiRecGet(const String &name, AiRec &r);
