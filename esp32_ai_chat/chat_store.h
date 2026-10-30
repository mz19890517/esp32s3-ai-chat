#pragma once
#include <Arduino.h>
#include <vector>

#define CHAT_MAX_MSGS  80        // 最多保留的消息条数
#define CHAT_MAX_CHARS 120000    // 所有消息正文的总字符预算（约 120KB）
#define CHAT_MSG_MAX   6000      // 单条消息最大字符数

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
int chatTotalChars();
int chatContentHeight(bool busy);
void chatRender(int top, int h, int scroll, bool busy, int busyPhase);
