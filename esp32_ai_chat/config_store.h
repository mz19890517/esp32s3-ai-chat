#pragma once
#include <Arduino.h>

struct Config
{
  String ssid;
  String wpass;
  String url;
  String key;
  String model;
};

extern Config cfg;

void cfgLoad();
void cfgSaveWiFi();
void cfgSaveApi();

String apiEndpoint(const String &base);
