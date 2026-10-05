#pragma once
#include <Arduino.h>

enum LinkMode : uint8_t { LINK_BLE, LINK_TCP };

#define SL_TCP_PORT 8888

void slBegin(const char *devName);
void slPoll();
bool slActive();
LinkMode slMode();
void slSetMode(LinkMode m);
void slToggleMode();
String slStatus();
bool slSend(const String &s);
String slLocalIp();