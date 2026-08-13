# ESP32-S3-Touch-LCD-4B 固件（M4：网络集成）

Waveshare ESP32-S3-Touch-LCD-4B（480×480）的 ESP-IDF 固件。UI 与网络层
位于 `main/business/`（从已退役的 macOS 仿真器迁移而来，保持零改动），
libcurl 由 `main/curl_shim/`（esp_http_client 后端子集实现）替代。

## 构建 / 烧录

统一口径：本固件基于 **ESP-IDF v6.0.2** 构建（与 `sdkconfig.defaults`
头注一致）。

```sh
cd firmware
. ~/.espressif/v6.0.2/esp-idf/export.sh   # 本机 IDF v6.0.2 安装路径
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

> 首次使用需先跑一次 IDF 安装脚本生成 Python venv：
> `~/.espressif/v6.0.2/esp-idf/install.sh esp32s3`。
> 依赖组件（lvgl 9.x、esp_lvgl_port、st7701 等）首次构建时由组件管理器
> 自动下载（`idf_component.yml`），需要网络。

## 版本与配置口径

- **LVGL**：组件声明 `^9.3`，组件管理器实际解析到 **9.5.0**（见
  `dependencies.lock`）；仿真器已退役，UI 源码统一维护在
  `main/business/ui/`（arduino/ 移植版另持一份同步副本）。
- **lv_conf**：`firmware/lv_conf.h`（RGB565 + libc malloc），经
  `-DLV_CONF_INCLUDE_SIMPLE` + `LV_CONF_PATH` 接入，细节见其头部注释。
- **mbedtls 内存**：`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`（sdkconfig.defaults），
  TLS 缓冲区走 PSRAM，规避开机时间窗内部 RAM 不足导致的握手 OOM。
- **CONNECTTIMEOUT_MS 行为差异**：IDF v6 的 esp_http_client 只暴露单一
  `timeout_ms`，curl_shim 接受 `CURLOPT_CONNECTTIMEOUT_MS` 但不单独
  生效，连接耗时并入总超时（见 `curl_shim.c` perform_once 注释）。

## 配置入口（三选一）

1. **触摸屏配网浮层**：状态栏齿轮按钮 → 扫描周边网络 → LVGL 键盘输入
   密码 → 写入 NVS 后自动重启（`main/business/ui/wifi_setup_overlay.c`）
2. **Web 配置页**：设备获得 IP 后访问 `http://<设备IP>/`，单页表单配置
   HA token / URL / 实体 ID，保存后自动重启（`main/web_config.c`）
3. **串口 cfg 命令**：`idf.py monitor` 串口输入（提示符 `fw> `）

所有配置保存在 NVS 命名空间 `fwcfg`，**写入后需重启生效**。

| 键 | 用途 | 对应环境变量 |
| --- | --- | --- |
| `wifi_ssid` | WiFi SSID | — |
| `wifi_psk` | WiFi 密码（空=开放网络） | — |
| `ha_token` | Home Assistant 长期令牌 | `HA_TOKEN` |
| `ha_base_url` | HA REST 基址 | `HA_BASE_URL` |
| `server_summary_url` | 服务器监控 /api/summary | `SERVER_SUMMARY_URL` |
| `weather_url` | Open-Meteo 天气 URL | `WEATHER_URL` |
| `ha_entity_temp` | 室内温度传感器实体 ID | `HA_ENTITY_TEMP` |
| `ha_entity_hum` | 室内湿度传感器实体 ID | `HA_ENTITY_HUM` |
| `ha_entity_pm25` | PM2.5 传感器实体 ID | `HA_ENTITY_PM25` |
| `ha_entity_mode` | 空气净化器模式 select 实体 ID | `HA_ENTITY_MODE` |
| `ha_entity_power` | 空气净化器电源 switch 实体 ID | `HA_ENTITY_POWER` |
| `ha_entity_ac` | 空调 climate 实体 ID | `HA_ENTITY_AC` |
| `ha_entity_lamp` | 台灯 switch 实体 ID | `HA_ENTITY_LAMP` |
| `ha_entity_cam1` | 摄像机 1 电源 switch 实体 ID | `HA_ENTITY_CAM1` |
| `ha_entity_cam2` | 摄像机 2 电源 switch 实体 ID | `HA_ENTITY_CAM2` |

```
fw> cfg set wifi_ssid MyHome
fw> cfg set wifi_psk my-password
fw> cfg set ha_token eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.xxxx
fw> cfg set ha_base_url http://192.168.9.207:8123
fw> cfg list          # ha_token / wifi_psk 只显示长度，不回显明文
fw> restart
```

未设置的键使用源码内编译期默认值（HA/服务器/天气 URL 及全部
`ha_entity_*` 实体 ID，见 `main/business/ha_client.h`）；没有
`wifi_ssid` 时 WiFi 不启动，UI 以 `--` 降级态运行，console 仍可用。

## M4 验收清单

- [x] `idf.py build` 通过，链接的 `main/business/net_worker.c`、
      `ha_client.c`、`server_client.c`、`weather_client.c`、cJSON 均零改动
- [x] 串口 `cfg set/list/restart` 可写读凭据，token 不回显明文
- [x] 开机 5s 内连上 WiFi（got ip 日志）；超时不阻塞启动
- [x] 有 token：状态栏 `LIVE`，仪表盘温/湿度、设备页与服务器页数据刷新
- [x] 无 token：状态栏 `NO TOKEN`，服务器/天气轮询仍工作
- [x] WiFi 断开后自动重连；HA 断线走 3→30s 退避
- [x] 天气走 HTTPS（证书包验证）正常返回深圳实况（mbedtls 缓冲区走 PSRAM 后）

## 目录要点

- `main/business/` — UI 页面与三路网络客户端（ha/server/weather）+
  `net_worker.c` 轮询编排；`third_party/cjson/` 内嵌 cJSON
- `main/curl_shim/` — libcurl 子集 shim（`curl/curl.h` + `curl_shim.c`），
  未支持的 option 返回 `CURLE_UNKNOWN_OPTION` 并打日志，绝不静默忽略；
  已兼容 HTTP/1.0 无 Content-Length 的 `ESP_ERR_HTTP_INCOMPLETE_DATA`
- `main/fonts/` — CJK 位图字体管线：`gen_fonts.sh` →
  `extract_symbols.py`（16px=GB2312 全量，20px=UI 扫描集）→
  `lv_font_conv` 生成 `font_cjk_16.c` / `font_cjk_20.c`
- `main/config_store.c` — NVS 读写 + esp_console `cfg`/`restart` 命令
- `main/env_shim.c` — 启动时把 NVS 配置 `setenv()` 为环境变量
- `main/wifi_sta.c` — STA 连接/自动重连，`wifi_sta_wait_connected()`
- `main/web_config.c` — 内置 HTTP 配置页（80 端口，got IP 后启动）
- `main/app_main.c` — 装配顺序：NVS → env → 时区 + SNTP → WiFi →
  显示/触摸 → UI → 等待 WiFi(5s) → curl_global_init → ha_client_init →
  net_worker → ui_drain
