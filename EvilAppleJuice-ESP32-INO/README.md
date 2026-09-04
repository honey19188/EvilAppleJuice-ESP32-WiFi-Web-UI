# EvilAppleJuice-ESP32（WiFi 控制版）

基于 EvilAppleJuice 的 ESP32-S3 固件，模拟 Apple 设备 BLE 广播以触发附近 iPhone/iPad 的"附近 AirPods / 连接"弹窗。
在原始版本基础上增加 **WiFi 控制后台（AP + Web UI / REST API）**、9 种预设模式、直接指定设备、参数实时调整、家庭 WiFi（STA）接入等功能。

> 适用于 Arduino-ESP32 **3.x 新版核心**（已适配新 API）。

---

## 1. 硬件与接线

| 硬件 | 说明 |
|------|------|
| 主控 | ESP32-S3（也兼容 C3/C2/H2/C6，发射功率宏已按芯片区分） |
| 右 LED | GPIO 12 |
| 左 LED | GPIO 13 |
| BOOT 按钮 | GPIO 9（内部上拉，短按切模式 / 长按 1s 复位） |

上电后 9 种 LED 亮灯模式对应 9 种广播模式，见 [第 5 节](#5-预设模式与-led-对照表)。

---

## 2. 编译与烧录（Arduino IDE）

1. 安装 **esp32 by Espressif Systems** 开发板包（3.x）。
2. 开发板选择：`ESP32S3 Dev Module`（烧录选项默认即可）。
3. 打开 `EvilAppleJuice-ESP32-INO.ino`，点击 **上传**。
4. 打开串口监视器（115200）可看到启动与广播日志。

> 若提示 `setDeviceAddress` / `BLE_ADDR_TYPE_RANDOM` 相关编译错误，请确认核心为 3.3.x 以上（本代码已尽量做版本/宏兼容）。

---

## 3. WiFi 控制后台

开机自动创建 WiFi 热点：

| 项目 | 值 |
|------|-----|
| AP SSID | `EAJ-Control` |
| 密码 | 无（开放式，可改第 31~34 行） |
| 是否隐藏 | 否（可直接扫描到） |
| 管理地址 | 手机连接后浏览器打开 `http://192.168.4.1` |

若要改密码，修改源码顶部：

```cpp
const char* WIFI_AP_PASSWORD = "";   // 留空 = 开放；设置需 >= 8 字符
```

### 手机连接步骤

- **iOS**：设置 → Wi-Fi → 搜索 `EAJ-Control` → 连接。
- **Android**：设置 → WLAN → 搜索 `EAJ-Control` → 连接。
- 连接后打开浏览器访问 `http://192.168.4.1`，即可看到控制页面。

---

## 4. REST API（供二次开发 / Android App）

所有接口均为 `GET`，返回 JSON。

| 方法 | 路径 | 参数 | 说明 |
|------|------|------|------|
| GET | `/` | — | 控制页面 HTML |
| GET | `/api/status` | — | 当前全部状态（IP/开关/模式/设备/间隔/功率/累计次数/STA 状态） |
| GET | `/api/devices` | — | 45 种设备完整列表（name/modelId/type） |
| GET | `/api/mode` | `m=0..8` | 切换预设模式（自动关闭"指定设备"） |
| GET | `/api/device` | `d=-1` 或 `d=0..44` | `-1` 恢复模式驱动；其它=直接指定设备 |
| GET | `/api/broadcast` | `on=0/1` | 停止 / 开启广播 |
| GET | `/api/settings` | `delay=X&pwr=Y` | 广播间隔 ms（20~5000）；功率 0~5 |
| GET | `/api/wifi/scan` | — | ESP32 软件扫描附近 WiFi（非手机系统扫描） |
| GET | `/api/wifi/connect` | `ssid=X&pass=Y` | 连接家庭 WiFi（AP+STA 共存，可从局域网访问） |
| GET | `/api/wifi/status` | — | STA 连接状态 |

状态字段示例（`/api/status`）：

```json
{
  "ip": "192.168.4.1",
  "staConnected": false,
  "broadcast": true,
  "mode": 0,
  "useManual": false,
  "manualIdx": 0,
  "lastDev": "Airpods",
  "delay": 100,
  "pwr": 0,
  "count": 12345
}
```

所有设置（模式 / 手动设备 / 开关 / 间隔 / 功率 / STA 凭据）均通过 `Preferences` **掉电保存**。

---

## 5. 预设模式与 LED 对照表

| 模式 | LED 状态 | 广播设备 | 类别 |
|------|----------|----------|------|
| 0 | 双灯灭 | AirPods | Audio |
| 1 | 右闪 | 随机设备（45 种中随机） | 随机 |
| 2 | 右亮 | Software Update | Audio |
| 3 | 左闪 | AirPods Gen 2 | Audio |
| 4 | 双闪 | Vision Pro | Setup |
| 5 | 左闪右亮 | AirPods Max | Audio |
| 6 | 左亮 | AppleTV Setup | Setup |
| 7 | 左亮右闪 | Transfer Number | Setup |
| 8 | 双灯亮 | AppleTV Pair | Setup |

- **ON**：常亮；**OFF**：常灭；**FLASH**：1 Hz 闪烁（由独立 500ms 定时器驱动，与广播循环解耦，肉眼清晰可辨）。
- 按键行为：短按 BOOT = 模式 +1（并关闭指定设备）；长按 ≥1s = 复位到模式 0。

---

## 6. 直接指定设备（45 种）

在 Web UI 或 `/api/device` 中可绕过模式、直接指定某一种设备广播（Audio 22 种 + Setup 23 种）。

`/api/devices` 中 `type`：`0 = Audio`（31 字节包，距离要求近）、`1 = Setup`（23 字节包，距离可更远，AppleTV Setup 效果通常最好）。

指定设备后 Web/串口状态会显示"指定设备模式"；点任一预设模式或 `d=-1` 即恢复模式驱动。

---

## 7. 参数设置

| 参数 | 范围 | 说明 |
|------|------|------|
| 广播间隔 `delay` | 20~2000 ms | 每轮广播持续时长 |
| 发射功率 `pwr` | 0~5 | `0`=动态随机（原版 70/15/10/4/1% 概率分配）；`1..5`=MAX / MAX-1 ~ MAX-4 固定档 |

`pwr=0` 动态策略：

| 概率 | 功率档 |
|------|--------|
| 70% | MAX |
| 15% | MAX-1 |
| 10% | MAX-2 |
| 4% | MAX-3 |
| 1% | MAX-4 |

---

## 8. BLE 广播实现要点（重要）

- **PDU 类型固定为 ADV_IND（可连接无向）= 0x00**。Apple 弹窗只对可连接无向广播响应。
  > 注意：`setAdvertisementType()` 接收的是 ESP32 `esp_ble_adv_type_t` 枚举值，**不是**蓝牙规范裸 PDU 值：
  > `0x00 = ADV_IND`、`0x01 = ADV_DIRECT_IND(定向，几乎不触发)`、`0x02 = ADV_SCAN_IND`、`0x03 = ADV_NONCONN_IND`。
  > 曾误用 0x01 导致完全无弹窗（定向广播只发给指定目标）。
- **每轮随机 MAC**：每次广播前生成新的随机地址，避免 Apple 端对固定 MAC 限速/记住后不再弹。
  - Bluedroid：`pAdvertising->setDeviceAddress(addr, 0x01 /*RANDOM*/)`
  - NimBLE（需 core ≥3.3.0）：`BLEDevice::setOwnAddrType(1)` + `BLEDevice::setOwnAddr(addr)`
- 广播数据由 `devices.cpp` 的 `generatePacket()` 生成（Audio 31 字节 / Setup 23 字节 Apple 厂商数据）。
- 广播循环中仍持续轮询 `server.handleClient()` 与刷新 LED，Web/按键操作不卡顿。

### 弹窗触发技巧（请留意）

- iPhone **亮屏**且处于主屏时更容易触发；锁屏状态下 iOS 不弹。
- 测试时建议**不要**让 iPhone 同时连接着 `EAJ-Control` 热点（BLE/WiFi 同频共存会影响成功率）。
- iOS 17 / 18+ 对仿冒广播拦截更强；若确认已是 ADV_IND 仍无效，优先尝试 **Setup 类设备**（如 AppleTV Setup）与较长间隔。

---

## 9. 故障排查

| 现象 | 排查 |
|------|------|
| 手机找不到 `EAJ-Control` | 确认第 34 行 `WIFI_AP_HIDDEN=false`；查看串口 AP 是否 OK、IP 是否为 192.168.4.1 |
| 网页打开报 404 | 路由采用双参数注册 + onNotFound 兜底（自动剥离 query string），串口会打印 `HTTP 404 -> uri=...` |
| 完全无弹窗 | ① 广播类型必须为 `ADV_IND (0x00)`，检查第 8 节；② iPhone 亮屏；③ 手机未连着该 ESP32 热点；④ 用 nRF Connect 确认类型与 MAC 变化 |
| 模式/设备不生效 | 指定设备后若点了预设模式，会退出指定设备（属预期）；确认 Preferences 已保存并重启生效 |
| STA 连不上 | `scan` 是否看到该网络；密码是否正确；AP+STA 时两个 IP 均可访问（`/api/status` 有 `staIp`） |

---

## 10. 文件结构

```
EvilAppleJuice-ESP32-INO/
├── EvilAppleJuice-ESP32-INO.ino   # 主程序（WiFi/Web/BLE/状态/LED 逻辑）
├── devices.hpp / devices.cpp       # 45 种 Apple 设备表 + 广播包生成
├── led.hpp                         # LED 模式枚举 / stateTable
└── EvilAppleJuice-ESP32-INO.md     # 本文档
```

> 仅供学习与个人设备测试使用；请遵守当地法律法规，勿用于骚扰他人。
