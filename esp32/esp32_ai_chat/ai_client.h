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

// 拉取厂商模型列表（OpenAI 兼容 GET {base}/models），models 以 '\n' 连接；阻塞调用（约 15s 超时）
bool aiFetchModels(const String &baseUrl, const String &apiKey, String &models, String &err);
