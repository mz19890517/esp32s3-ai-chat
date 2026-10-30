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
