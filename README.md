# ESP32-S3 触摸屏 AI 聊天固件

微雪 **ESP32-S3-Touch-LCD-3.5B**（3.5 寸 320×480 触摸屏）独立运行的 AI 聊天应用：上电即用，无需手机或电脑配合。

## 功能

- 屏幕软键盘输入（中英文界面、符号页、退格长按连删）
- 调用 OpenAI 兼容 API（默认 DeepSeek），流式气泡聊天界面
- WiFi 扫描/选择/密码输入，断线自动重连
- API 地址、模型、Key 均可在屏幕上自定义
- WiFi 与 API 配置存 NVS，断电不丢

## 硬件

- 主控：ESP32-S3R8（16MB Flash / 8MB PSRAM）
- 屏幕：AXS15231B QSPI 320×480（触摸一体）
- 驱动库：[GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) ≥ 1.6.0

## 方式一：Arduino IDE 编译

1. 安装 ESP32 板支持包（Boards Manager 搜索 `esp32`）
2. 库管理器安装：
   - **GFX Library for Arduino**（作者 Moon On Our Nation，版本 ≥ 1.6.0）
   - **ArduinoJson**（≥ 7.0）
3. 开发板选 **ESP32S3 Dev Module**，关键设置：
   | 选项 | 值 |
   |---|---|
   | PSRAM | OPI PSRAM |
   | Flash Size | 16MB |
   | Flash Mode | QIO 80MHz |
   | Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) 或 Huge App (3MB No OTA) |
   | USB CDC On Boot | Enabled |
4. 打开 `esp32_ai_chat/esp32_ai_chat.ino` 编译上传

## 方式二：PlatformIO

仓库根目录已带 `platformio.ini`，直接 `pio run`；或推送到 GitHub 由 Actions 自动编译，在 Actions 页面下载 `firmware-bin` 构件得到 `firmware.bin`。

## 烧录 .bin（免 IDE）

- 电脑：[esptool](https://docs.espressif.com/projects/esptool/)：
  ```
  esptool --chip esp32s3 --port COMx --baud 921600 write_flash 0x0 firmware.bin
  ```
  （使用 Huge App 分区时地址为 0x0；若用默认 4MB 分区方案则为 0x10000，且需同时烧录 bootloader/partition table）
- 手机/电脑浏览器：打开 https://espressif.github.io/esptool-js/ ，Chrome/Edge 用数据线连接即可网页刷入

## 首次使用

1. 上电进入主界面，点右上角 ⚙ 进入设置
2. WiFi 设置 → 选择热点 → 输密码 → 连接
3. API 设置 → 填 Key（默认 `https://api.deepseek.com` + `deepseek-chat`，兼容任何 OpenAI 格式接口）
4. 返回聊天界面，输入消息点「发送」

## 说明

- HTTPS 使用 `setInsecure()` 不校验证书（嵌入式常见做法，Key 只发往你自己填写的服务器）
- 聊天记录仅保存在内存，重启清空
