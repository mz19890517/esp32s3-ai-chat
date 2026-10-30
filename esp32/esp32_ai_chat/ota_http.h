#pragma once
#include <Arduino.h>

// WiFi 推送升级：设备开一个极简 HTTP 服务，网页直接把 firmware.bin POST 上来。
//   OPTIONS /ota   预检（CORS）
//   GET     /info  返回版本号
//   POST    /ota   body 为固件二进制，Content-Length 必须是文件大小

#define OTA_HTTP_PORT 8080

void otaHttpBegin();          // 启动后台任务
void otaHttpPoll();           // 主循环调用：把当前 IP 同步给蓝牙"信息"特征
bool otaHttpWriting();        // 正在接收
String otaHttpUrl();          // http://<ip>:8080/ota，未联网时为空
