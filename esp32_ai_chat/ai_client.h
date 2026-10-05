#pragma once
#include <Arduino.h>
#include <vector>

struct AiMsg
{
  String role;
  String content;
};

void aiBegin();
bool aiBusy();
void aiStart(const std::vector<AiMsg> &history, const String &url, const String &key, const String &model);
void aiStartEx(const std::vector<AiMsg> &history, const String &url, const String &key, const String &model, const String &sysPrompt);
bool aiPoll(String &reply, String &err);
