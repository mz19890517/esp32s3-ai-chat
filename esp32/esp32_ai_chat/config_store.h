#pragma once
#include <Arduino.h>

struct Config
{
  String ssid;
  String wpass;
  String url;
  String key;
  String model;
  String otaUrl;
};

extern Config cfg;

void cfgLoad();
void cfgSaveWiFi();
void cfgSaveApi();
void cfgSaveOtaUrl();

String apiEndpoint(const String &base);
String apiNormalizeBase(const String &base);   // 剥掉误填的 /chat/completions 后缀（拉模型等基础地址用途）
