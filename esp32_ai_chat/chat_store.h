#pragma once
#include <Arduino.h>
#include <vector>

struct ChatMsg
{
  bool user;
  bool err;
  String text;
  std::vector<String> lines;
  int bw = 0, bh = 0;
};

extern std::vector<ChatMsg> msgs;

void chatAdd(bool user, const String &text, bool err = false);
void chatClear();
int chatContentHeight(bool busy);
void chatRender(int top, int h, int scroll, bool busy, int busyPhase);
