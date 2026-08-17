# ESP32-S3 智能家居控制面板

基于 **Waveshare ESP32-S3-Touch-LCD-4B**（480×480 触摸圆屏）的桌面智能面板固件。
LVGL 图形界面，接入 Home Assistant 与自建服务器监控，支持 WiFi 配网、
Web 配置页与 SNTP 校时。

## 功能特性

| 页面 | 内容 |
| --- | --- |
| 仪表盘 | 室内温度 / 湿度 / PM2.5（HA 传感器），Open-Meteo 天气实况 |
| 设备控制 | 空气净化器（模式/电源）、空调、台灯、摄像机 —— 通过 HA 服务调用（REST），非只读展示 |
| 服务器监控 | Proxmox 集群 `/api/summary`：主机在线数、CPU/内存/磁盘占用、逐主机状态列表 |

其他能力：

- **WiFi 配网**：状态栏齿轮按钮 → 全屏配网浮层（扫描周边网络、LVGL 键盘输入密码、写入 NVS 后自动重启）
- **Web 配置页**：设备获得 IP 后内置 HTTP 服务（80 端口）提供单页表单，可配置 HA token / URL / 实体 ID 等，保存后自动重启
- **串口配置**：`fw> cfg set/list` 命令行备选入口（详见 [firmware/README.md](firmware/README.md)）
- **时钟**：SNTP（ntp.aliyun.com，CST-8 时区）校时，状态栏显示日期时间
- **中文支持**：内置 CJK 位图字体（16px 含 GB2312 全量 6763 汉字），缺字回退 Montserrat
- **健壮性**：WiFi 断线自动重连；HA 断线 3→30s 退避；三路轮询独立节奏（HA 3s / 服务器 15s、失败退避 60s / 天气 30min、失败 5min）

## 硬件

- Waveshare ESP32-S3-Touch-LCD-4B
  - ESP32-S3（240 MHz）/ 16MB Flash / 8MB Octal PSRAM（**qio_opi 必需**）
  - 480×480 RGB LCD（ST7701）+ GT911 触摸 + AXP2101 电源管理
- 详细引脚与外设说明见 `docs/Waveshare-ESP32-S3-Touch-LCD-4B.md`

## 仓库结构

```
firmware/    ESP-IDF v6.0.2 真机固件（主目标，见 firmware/README.md）
  main/business/   UI 与网络层（从已退役的 macOS 仿真器迁移而来）
  main/curl_shim/  libcurl 子集 shim（esp_http_client 后端）
  main/fonts/      CJK 位图字体生成管线（gen_fonts.sh）
docs/        开发板硬件资料
```

> `ESP32-S3-Touch-LCD-4B-chore-initial-resource-import/`（约 1GB 的厂商
> 参考仓库：官方示例、固件、原理图）不入 Git，按 `docs/` 说明自行下载。

## 快速上手（firmware/，ESP-IDF）

```sh
cd firmware
. ~/.espressif/v6.0.2/esp-idf/export.sh   # 本机 IDF v6.0.2 路径，按需调整
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

- 首次构建前需 `~/.espressif/v6.0.2/esp-idf/install.sh esp32s3` 生成 Python venv
- 依赖组件（lvgl 9.5、esp_lvgl_port、st7701、gt911 等）首次构建由组件管理器
  自动下载（`main/idf_component.yml`），需要网络
- 分区表：`ota_0` / `ota_1` 各 7.1MB + 2MB storage；app 当前约 3MB
- 首次启动无 WiFi 凭据时，点状态栏齿轮完成配网，其余配置经 Web 配置页写入

## CJK 字体管线（firmware/main/fonts/）

```sh
bash gen_fonts.sh
```

`extract_symbols.py` 生成两份符号集 → `lv_font_conv`（Arial Unicode.ttf，
4bpp）生成 `font_cjk_16.c` / `font_cjk_20.c`：

- **16px 正文**：GB2312 全量汉字（6763 字）+ UI 源码扫描字符，用于渲染
  服务器描述等运行时中文数据（约 5.7MB C 源码）
- **20px 大字**：仅 UI 源码扫描集（约 220 字，用于标题/大数字）。不要给
  20px 用全量集——8MB 级 C 源文件会拖垮 gcc 编译

## 工程要点

- `curl_shim`：业务层沿用仿真器的 libcurl 调用代码零改动；shim 已处理
  HTTP/1.0 无 `Content-Length` 的 `ESP_ERR_HTTP_INCOMPLETE_DATA` 兼容问题
  （串口仍会打印一条 `HTTP_CLIENT: Incomplete chunked data received` 的
  E 级日志，属 IDF 内部日志，无害）
- mbedTLS 缓冲区经 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` 走 PSRAM，规避开机
  时间窗内部 RAM 不足导致的 TLS 握手 OOM（-0x008D）
- 16KB 大结构体（`server_snapshot_t`）全部 static 化，避免栈溢出踩堆
- 各网络线程显式 12KB pthread 栈（IDF 默认 768B 过小）

## 相关文档

- [firmware/README.md](firmware/README.md) — IDF 固件构建、配置键表、目录说明
- [docs/Waveshare-ESP32-S3-Touch-LCD-4B.md](docs/Waveshare-ESP32-S3-Touch-LCD-4B.md) — 开发板资料
