#pragma once
#include <Arduino.h>

#define KB_CHANGED 1
#define KB_SEND    2

void kbAttach(String *buf);
void kbSetMask(bool m);
void kbSetSendLabel(const char *label);
void kbReset();
String kbTake();
String kbPeek();
void kbDraw();
void kbDrawInputLine();
uint8_t kbHandle(bool down, int x, int y);
