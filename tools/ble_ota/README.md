# 固件升级 + 网页控制台

本页（`index.html`）现在有三个 tab：**推送升级** / **设备设置** / **设备状态**。
后两个走设备 8080 端口的 HTTP 接口（`/api/caps` `/api/settings` `/api/status`），
设置项由固件里的配置表（`esp32/esp32_ai_chat/cfg_table.cpp`）自动生成——固件加一项设置，网页自动出现对应控件，屏幕「设置 → 配置项」页也会同步出现。

## 0. 网页控制台（新）

1. 设备连上 WiFi（屏幕：设置 → 设备状态 → 本机 IP）。
2. 网页填设备 IP（推送升级页连蓝牙可自动获取；或填 `esp32-ai-s3.local`）。
3. **设备设置**：点「加载设置」，按分组渲染表单；开关项点击即切换，其余改完点「保存」，逐项写入 NVS。
4. **设备状态**：进入 tab 自动开始 1 秒轮询（固件版本/槽位/IP/WiFi/内存/蓝牙/BMS）。

**PIN 保护**：设备上 设置 → 配置项 → 「网页需要 PIN」打开后，所有 `/api/*`
（除 `/api/caps` 外）都要求请求头 `X-Pin`，否则 401。PIN 是开机随机的 4 位数字，
显示在设备屏幕 设置 → 设备状态 的「网页 PIN」一行，**每次重启会变**。
网页侧在「设备设置」页顶部输入 PIN 即可，之后自动携带。

> CORS 已在固件端处理（`Access-Control-Allow-Origin: *` + `Access-Control-Allow-Private-Network: true`，
> 预检放行 `Content-Type, X-Pin`）。页面仍需 `http://localhost` 或 https 才能用 Web Bluetooth。

## 1. SD 卡升级（开机自动）

1. TF 卡格式化为 **FAT32**。
2. 在卡根目录建 `update` 文件夹，把 PlatformIO 生成的
   `esp32s3/.pio/build/esp32s3/firmware.bin` 复制进去并命名为 `fw.bin`：

   ```
   TF 卡/
     update/
       fw.bin
   ```

3. 插卡 → 重启设备。开机会显示「检测到 SD 卡固件」，进度到 100% 后自动重启。

安全性：

- 只接受 ESP32-S3 镜像（magic `0xE9`、chip id `0x0009`、段数合法、大小 ≤ 3 MB）
- 写入后由 ESP-IDF 的 `esp_ota_end` 校验整包 SHA256，失败不会切换启动分区
- 成功后文件改名为 `update/fw.bin.ok`，失败改名为 `update/fw.bin.bad`，不会反复刷同一文件

## 2. 蓝牙 OTA（Web Bluetooth）

1. 双击 `serve.bat`，浏览器会自动打开 `http://localhost:8080/`（Chrome / Edge / 安卓 Chrome）。
2. 点「连接设备」，选择 `ESP32-S3-AI`。
3. 选择 `firmware.bin` → 「开始升级」。
4. 完成后设备自动重启。

> Web Bluetooth 需要安全上下文：`localhost` 或 HTTPS。iPhone 不支持 Web Bluetooth。

自定义 GATT：

| 用途 | UUID |
|---|---|
| Service | `5f9b0001-7a2c-4d3e-9c1e-9b6f5c1a2d01` |
| RX（写入） | `5f9b0002-7a2c-4d3e-9c1e-9b6f5c1a2d01` |
| TX（通知） | `5f9b0003-7a2c-4d3e-9c1e-9b6f5c1a2d01` |

协议（小端）：

```
主机 -> 设备
  0x01 + uint32 size    开始，size 为固件字节数
  0x02 + data           固件分片（≤ 244 字节）
  0x03                  结束
  0x04                  取消

设备 -> 主机
  0x81                  准备就绪（擦除完成）
  0x82 + uint32 bytes   已写入字节数（每 8 KB 一次）
  0x83                  校验通过，即将重启
  0x84 + uint32 code    错误码
```

错误码：1 无法开始写、2 写入失败、3 校验失败、4 数据溢出、5 大小/数据不完整、6 超时、7 已取消。

## 3. WiFi 升级（原有）

- 屏幕：设置 → 固件升级 → 填写 `http://电脑IP:8000/firmware.bin` → 开始 URL 升级
- 电脑推送：ArduinoOTA（`espota.py` / Arduino IDE 网络端口），主机名 `ESP32-AI-S3`，端口 3232

电脑上可直接双击 `../ota/serve_ota.bat`，它会自动复制最新 `firmware.bin` 并开好 8000 端口。
