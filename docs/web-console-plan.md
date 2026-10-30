# 网页控制台（Web Console）需求与设计

> 状态：**P1 已实现并追加体验增强，待设备实测**（2026-10-30 完成编码并编译通过）
> 追加：AI 厂商预置+自动拉取模型列表（S_API 重排 + 新页 S_MODELS）、WiFi 密码记忆（rec_store，像手机）、
> 修复触摸 release 落进新页面列表导致的“配置项进页即弹编辑/取消后循环弹编辑”
> 目标版本：1.3.x　｜　前置版本：1.2.8（WiFi 推送 OTA 已完成并可用）

---

## 1. 目标

把现在的 `ble_ota/index.html` 从"只能推送升级"的单一页面，扩展成设备的**网页控制台**：

1. 分页：`推送升级`（现有内容改名）/ `设备设置` / `设备状态`（后续再加 `串口` / `聊天`）
2. **核心诉求：设置项动态同步** —— 固件里新加/修改一个设置项，网页自动出现对应控件，
   不需要再手改 HTML。屏幕上的设置页也由同一张表驱动（"一处定义，两端同步"）。

## 2. 已拍板的决策

| 议题 | 结论 |
|---|---|
| 首期范围 | **P1：动态设置 + 状态页**（串口/聊天后置） |
| 传输通道 | **HTTP 轮询**（8080 端口），不用 WebSocket |
| 访问保护 | 做成**一个设置项** `web.pin`（是否要求 PIN），开机随机 4 位显示在设备状态页 |
| 屏幕端 | **也改成表驱动**（固件/网页两边一起改） |

## 3. 核心设计：配置表即唯一数据源

固件里一张表定义所有设置项，新增一项 = 加一行：

```cpp
struct CfgItem {
  const char *key;     // NVS 键，≤15 字符
  const char *group;   // 分组（表内必须连续排列）
  const char *label;
  uint8_t type;        // CT_STR / CT_INT / CT_BOOL / CT_ENUM
  int32_t minv, maxv;  // CT_INT 范围
  const char *options; // CT_ENUM："a|b|c"
  uint8_t flags;       // CFG_SECRET（打码）/ CFG_RESTART
  const char *defval;
};
```

- 旧字段（ssid/wpass/url/key/model/otaUrl）映射到内存里的 `Config cfg`，仍走原来的 `cfgSave*()`
- 新字段统一存 NVS 命名空间 `cfg`（键即 `CfgItem.key`）
- 已建好文件：`esp32_ai_chat/cfg_table.h` / `cfg_table.cpp`（**当前是半成品，未被任何源文件 include，不影响编译**）

## 4. 接口草案（8080）

```
GET  /api/caps
     -> {"api":1,"fw":"1.2.9","caps":["settings","status"],"pinreq":false}

GET  /api/settings               # 按 group 分组，网页据此渲染
     -> {"api":1,"groups":[
          {"g":"网络","items":[
            {"k":"net.ssid","l":"WiFi 名称","t":"str","v":"MyWiFi"},
            {"k":"net.pass","l":"WiFi 密码","t":"str","v":"abc****de","secret":true}]},
          {"g":"安全","items":[
            {"k":"web.pin","l":"网页需要 PIN","t":"bool","v":"0"}]}]}

POST /api/settings               # Content-Type: application/x-www-form-urlencoded
     k=net.ssid&v=MyWiFi
     -> {"ok":true,"v":"MyWiFi"} / {"err":"超出范围"}

GET  /api/status                 # 实时状态，网页 1 秒轮询
     -> {"fw":"1.2.9","slot":"ota_0 -> ota_1","ip":"192.168.1.23",
         "ssid":"xxx","rssi":-52,"heap":138000,"maxblk":110000,
         "ble":{"conn":true,"adv":false,"name":"ESP32-S3-AI"},
         "bms":{"ok":true,"soc":87,"v":52.3,"a":-1.2}}
```

- PIN 开启时，所有 `/api/*` 需带 `X-Pin: 1234`，否则 401
- 所有响应必须带 CORS 头，并含 `Access-Control-Allow-Private-Network: true`（Chrome 私有网络访问）

## 5. 任务清单

### P1（下一步就做这个）
- [x] `cfg_table.cpp` 落地：表 + `cfgGet/cfgSet` + 类型校验 + 旧字段映射
- [x] `cfgBuildSettingsJson()` / `cfgBuildStatusJson()`
- [x] `ota_http.cpp` 增加 `/api/caps` `/api/settings`(GET/POST) `/api/status`，PIN 校验
      （caps 不挡 PIN：网页要先从它拿到 pinreq）
- [x] 屏幕：设置页加一行「配置项」→ 新页 `S_CONF`，列表由表生成；点按编辑
      （BOOL 直接翻转 / ENUM 循环 / STR 与 INT 走键盘编辑页）
- [x] 状态页显示 PIN（仅 `web.pin` 开启时）
- [x] 网页：加 tab 栏，`推送升级` 原样迁移；`设备设置` 按 schema 渲染表单；`设备状态` 轮询
- [x] 版本号 → 1.2.9
- [ ] 设备实测（烧录 → 网页三个 tab 全流程 + PIN 开关）

### P2
- [ ] 串口控制台：`GET /api/serial/rx`（环形缓冲游标）+ `POST /api/serial/tx`，150 ms 轮询
- [ ] 网页 `串口` tab：HEX/文本切换、清空、自动滚动

### P3
- [ ] 聊天：`GET /api/chat?since=` + `POST /api/chat`（轮询；可选 SSE）
- [ ] 网页 `聊天` tab：气泡列表、发送、思考中动画
- [ ] （可选）同一套 JSON 走 BLE 兜底（`5f9b0005` 请求 / `5f9b0003` 响应，分片重装）

## 6. 已知坑（别重踩）

1. **GATT 服务必须在广播前注册**：`ble_gatts_add_svcs()` 在广播/扫描/连接期间返回 `EBUSY`，
   表现为"服务找得到、特征找不到"。顺序必须是 `slBegin → otaBleBegin → otaBegin → slStartAdv`。
2. **Chrome 缓存 GATT**：改了固件服务后要清 `chrome://bluetooth-internals` 和系统蓝牙里的设备记录。
3. **页面必须 `http://localhost` 或 https**：Web Bluetooth 需要安全上下文；`file://` 直接不可用。
4. **CORS + 私有网络访问**：localhost 页面访问内网 IP，预检要回 `Access-Control-Allow-Private-Network: true`。
5. **mDNS 不可全指望**：安卓 Chrome 基本解析不了 `.local`，所以查找顺序是 mDNS → 蓝牙读 IP → 网段扫描。
6. **WiFi 推送的 /info 要带 CORS**，网段扫描靠它确认"真的是我们的设备"。
7. `Preferences` 键长 **≤15 字符**。

## 7. 当前固件基线（1.2.8 已实现）

- WiFi 推送 OTA（8080 `/ota`，XHR 上传带进度、CORS）
- 蓝牙 OTA（`5f9b0001` 服务 + 只读信息特征 `5f9b0004` 返回 `http://ip:8080/ota`）
- TF 卡 OTA（开机检测 `/update/fw.bin`，成功改名 `.ok`、失败 `.bad`）
- 设备状态页（设置第 5 项）：版本/槽位/升级进度/BLE 连接与广播/本机地址/WiFi/IP/内存
- 聊天内存上限、历史按字数裁剪、BLE 发送不阻塞、TCP 整包读取、列表滚动条

## 8. 恢复工作时第一步

```powershell
cd F:\ESP32\esp32s3
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -t upload   # 先确认 1.2.8 基线正常
```

然后从 P1 任务清单第一项开始：把 `cfg_table.cpp` 写完，再改 `ota_http.cpp` 加路由。
