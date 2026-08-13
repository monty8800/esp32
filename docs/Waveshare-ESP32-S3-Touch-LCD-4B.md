# ESP32-S3-Touch-LCD-4B 开发板资料总结（基于 Waveshare 官方中文 Wiki）

> **资料来源**：<https://www.waveshare.net/wiki/ESP32-S3-Touch-LCD-4B>
> **总结日期**：2026-08-13
>
> 说明：本文是对上述 Wiki 页面的结构化调研总结。Wiki 未提及的内容（如具体 GPIO 引脚编号、板子尺寸数值、FAQ 详情等）均明确标注"**Wiki 未提及**"，不作推测或编造。文末附本项目自行核对的引脚参考小节。
>
> **页面结构前提**：该中文 Wiki 页面的实际目录为「1 产品介绍（简介/特性/硬件说明/资源简介/产品尺寸）、2 使用说明、3 Arduino 开发、4 ESP-IDF 开发、5 小智AI应用、6 Flash 固件的烧录与擦除、7 资料、8 FAQ、9 技术支持」。页面**不含**独立的"引脚定义"章节、**不含**版本对比表、**不含**硬件结构图章节，FAQ 章节为空占位。

---

## 1. 产品概述

- ESP32-S3-Touch-LCD-4B 是微雪电子（Waveshare）推出的开发板，搭载 ESP32-S3 2.4GHz Wi-Fi + BLE 5 模组，集成 **16MB Flash** 和 **8MB PSRAM**。
- 板载 **4 英寸 480×480 分辨率 RGB 接口 LCD 屏**，可流畅运行 LVGL 等 GUI 程序。
- 定位场景：智能中控面板、家庭网关、智能交互面板、工业控制、智能灯控等 HMI 应用。
- 与其他型号的版本区别：**Wiki 未提及**。

---

## 2. 硬件规格表（来自页面原文）

| 项目 | 参数 |
|---|---|
| 主控 | ESP32-S3R8，Xtensa 32 位 LX7 双核，主频高达 240MHz（模组为 ESP32-S3-WROOM-1-N16R8） |
| 无线 | 2.4GHz Wi-Fi (802.11 b/g/n) + Bluetooth 5 (LE)，板载天线 |
| 内存 | 内置 512KB SRAM + 384KB ROM；叠封 8MB PSRAM；外接 16MB Flash |
| 屏幕 | 4 英寸电容触摸屏，480×480，65K 色，RGB 接口 |
| 显示驱动 | ST7701，使用 RGB 接口通信 |
| 触摸 | GT911 电容触控芯片，使用 I2C 接口通信 |
| IMU | QMI8658 六轴（3 轴加速度 + 3 轴陀螺仪），支持运动姿态、计步 |
| RTC | PCF85063，通过 AXP2101 接入电池实现不间断供电 |
| 电源管理 | AXP2101（高效电源管理、多输出电压、充电与电池管理、电池寿命优化） |
| 音频 | ES8311 低功耗音频编解码芯片；ES7210 回声消除算法芯片；贴片麦克风（含回声消除） |
| 扬声器 | MX1.25 2P 连接器，支持 8Ω 2W 喇叭 |
| IO 扩展 | TCA9554PWR IO 扩展芯片 |
| 电池 | 3.7V PH2.0 2P 锂电池充放电接口；CHG LED 充电指示灯 |
| 按键 | PWRKEY 电源按键（可控制电源通断，支持自定义功能）；BOOT 按键（设备启动和功能调试） |
| USB | 双 Type-C：① USB TO UART（供电/烧录/调试）；② ESP32-S3 原生 USB（供电、烧录、日志打印） |
| 扩展接口 | 2.0mm 间距扩展接口，可外接 IO |
| 尺寸 | 页面有尺寸图，但 Wiki 文本**未给出具体尺寸数值**（Wiki 未提及） |

---

## 3. 板上资源（"资源简介"列出 17 项）

1. ESP32-S3-WROOM-1-N16R8 模组
2. AXP2101（电源管理）
3. ES7210（回声消除/拾音）
4. ES8311（音频编解码）
5. PCF85063（RTC）
6. QMI8658（六轴 IMU）
7. TCA9554PWR（IO 扩展）
8. LCD 接口（4 寸 RGB 屏）
9. Type-C USB TO UART
10. ESP32-S3 原生 USB Type-C
11. 贴片麦克风
12. 扬声器（MX1.25 2P，8Ω 2W）
13. PWRKEY 电源按键
14. BOOT 按键
15. CHG LED 充电指示灯
16. PH2.0 锂电池接口
17. 2.0mm 间距扩展接口

---

## 4. 引脚说明

**中文 Wiki 和英文 Wiki 均未提供任何 GPIO 引脚编号表**（Wiki 未提及）。页面中与引脚相关的信息仅有以下几点：

- Arduino 例程 `02_GFX_AsciiTable` 代码片段中使用宏 `LCD_CS`、`LCD_SCLK`、`LCD_SDIO0`~`LCD_SDIO3`，通过 `Arduino_ESP32QSPI` 总线驱动 ST7701（官方例程代码原样内容）。
- 例程 `06_LVGL_Arduino_v9` 使用 `Wire.begin(IIC_SDA, IIC_SCL)` 初始化 I2C，并通过 TCA9554 expander 扩展芯片控制引脚。
- 上述宏的真实 GPIO 编号定义在示例程序包内的 **Mylibrary**（"开发板宏定义"库，仅离线安装）中，页面未展开。
- 逐项而言：LCD RGB 信号线、背光、GT911 I2C、TCA9554、USB/UART、按键、喇叭 I2S、电池 ADC 的 GPIO 编号，**Wiki 均未提及**。
- **引脚号权威来源**：原理图 PDF <https://www.waveshare.net/w/upload/8/82/ESP32-S3-Touch-LCD-4B.pdf> 及示例程序包中的 Mylibrary。

> 本项目自行核对的引脚参考见文末附录：[本项目已核对的引脚参考](#9-附录本项目已核对的引脚参考)。

---

## 5. 开发环境搭建

官方支持 **Arduino IDE** 与 **ESP-IDF** 两种方式（MicroPython 在页面中被注释掉、未正式提供，但保留了 MicroPython 文档链接）。

### 5.1 Arduino

- 需安装 `esp32 by Espressif Systems` 开发板包，**版本 ≥ 3.2.0**。
- 板卡选择 **"ESP32S3 Dev Module"**；仅有 USB 口时需开启 **USB CDC**。

Arduino 依赖库表：

| 库 | 用途 | 版本 | 安装要求 |
|---|---|---|---|
| GFX_Library_for_Arduino | 适配 ST7701 的 GFX 图形库 | v1.6.0 | 在线/离线均可 |
| lvgl | LVGL 图形库 | v9.3.0 | 在线安装后需复制 demos 文件夹至 src，建议离线 |
| SensorLib | PCF85063、QMI8658、GT911 驱动 | v0.3.1 | 在线/离线均可 |
| XPowersLib | AXP2101 电源管理驱动 | v0.2.6 | 在线/离线均可 |
| Mylibrary | 开发板宏定义（含引脚定义） | — | 仅离线 |
| lv_conf.h | LVGL 配置文件 | — | 仅离线 |

### 5.2 ESP-IDF

- 官方推荐 **VS Code + Espressif IDF 插件**。
- 参考文档使用的 IDF 版本为 **v5.1.4**；页面**未明确给出**例程所需 ESP-IDF 版本号（Wiki 未提及）。
- 目标芯片 ESP32-S3，UART 下载，板载自动下载电路；下载失败时按复位键 1 秒以上。

---

## 6. 官方例程列表

### 6.1 Arduino 例程（8 个）

| 例程 | 说明 | 依赖库 |
|---|---|---|
| 01_HelloWorld | 基本图形库功能，测试显示屏基础性能/随机文本 | GFX_Library_for_Arduino |
| 02_GFX_AsciiTable | 按行列打印 ASCII 字符表 | GFX_Library_for_Arduino |
| 03_LVGL_PCF85063_simpleTime | LVGL 显示 RTC 当前时间 | LVGL、SensorLib |
| 04_LVGL_QMI8658_ui | LVGL 绘制加速度/陀螺仪折线图 | LVGL、SensorLib |
| 05_LVGL_AXP2101_ADC_Data | LVGL 显示 PMIC 数据（温度/充放电/Vbus/电池电压/百分比），PWR 键控制亮/熄屏渐变 | LVGL、XPowersLib |
| 06_LVGL_Arduino_v9 | LVGL Widgets 演示，动态帧率 20~30fps，含 TCA9554+GT911 初始化 | LVGL、Arduino_DriveBus |
| 07_ES8311 | I2S 驱动 ES8311 播放音频（16 位、canon_pcm） | — |
| 08_ES7210 | I2S 驱动 ES7210 拾音、人声过滤 | — |

### 6.2 ESP-IDF 例程（5 个 + 工厂固件）

| 例程 | 说明 |
|---|---|
| 01_AXP2101 | ESP-IDF 移植 XPowersLib 读取电源数据（不点屏，仅串口输出） |
| 02_lvgl_demo_v9 | 运行 LVGL V9 demo |
| 03_esp-brookesia | esp-brookesia 框架 UI 示例，依赖 v0.4.2 |
| 04_Immersive_block | QMI8658 重力感应，块状体沉浸式倾倒 |
| 05_Spec_Analyzer | 麦克风拾音 + FFT 频谱显示 |
| FactoryFirmWare | 基于 esp-brookesia master 的综合应用（触摸/显示/拾音/播放/时钟/陀螺仪/PMU/小智AI），持续更新中 |

### 6.3 相关下载与教程入口

- 例程统一下载入口（zip）：<https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4B/ESP32-S3-Touch-LCD-4B-Demos.zip>
- 固件烧录/擦除教程：<https://www.waveshare.net/wiki/Flash固件的烧录与擦除>
- 小智AI应用教程：<https://www.waveshare.net/wiki/小智AI应用参考教程>（声音问题可通过对话"将声音调至最大"解决）

---

## 7. 注意事项

> 注意：Wiki 的 FAQ 章节为**空占位**（Wiki 未提及具体 FAQ 内容），以下为页面中给出的提示事项。

- **下载失败时**：按复位按键 1 秒以上或手动进下载模式，待 PC 重新识别后重试。
- ESP-IDF 编译占用大量 CPU，首次编译时间长。
- 正确选择工程目录/COM 口。
- **锂电池安全**：防潮防高温防摔，避免过充过放，长期不用拆电池存放，选带保护电路的合规电池；循环寿命到期或用满两年应更换；勿混用新旧电池。
- Arduino 安装 lvgl 库后需将 demos 文件夹复制进 src。

---

## 8. 资料链接汇总

### 8.1 硬件资料

| 资料 | 链接 |
|---|---|
| 原理图 | <https://www.waveshare.net/w/upload/8/82/ESP32-S3-Touch-LCD-4B.pdf> |
| ESP32-S3 数据手册（中） | <https://www.waveshare.net/w/upload/5/58/Esp32-s3_datasheet_cn.pdf> |
| ESP32-S3 数据手册（英） | <https://www.waveshare.net/w/upload/b/bd/Esp32-s3_datasheet_en.pdf> |
| ESP32-S3 技术参考手册（中） | <https://www.waveshare.net/w/upload/8/88/Esp32-s3_technical_reference_manual_cn.pdf> |
| ESP32-S3 技术参考手册（英） | <https://www.waveshare.net/w/upload/1/11/Esp32-s3_technical_reference_manual_en.pdf> |

### 8.2 外设 Datasheet

| 芯片 | 链接 |
|---|---|
| QMI8658 | <https://www.waveshare.net/w/upload/5/5f/QMI8658C.pdf> |
| PCF85063 | <https://www.waveshare.net/w/upload/9/97/PCF85063A.pdf> |
| AXP2101 | <https://www.waveshare.net/w/upload/e/ed/X-power-AXP2101_SWcharge_V1.0.pdf> |
| ES8311 Datasheet | <https://www.waveshare.net/w/upload/6/65/ES8311.DS.pdf> |
| ES8311 UserGuide | <https://www.waveshare.net/w/upload/5/56/ES8311.user.Guide.pdf> |
| GT911（中） | <https://www.waveshare.net/w/upload/e/eb/GT911.pdf> |
| GT911（英） | <https://www.waveshare.net/w/upload/d/d9/GT911_EN_Datasheet.pdf> |
| ES7210 | <https://www.waveshare.net/w/upload/5/54/ES7210-datasheet.pdf> |

### 8.3 软件与示例

| 资料 | 链接 |
|---|---|
| 示例程序（zip） | <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4B/ESP32-S3-Touch-LCD-4B-Demos.zip> |
| 汉字取模工具 | <https://www.waveshare.net/w/upload/c/c6/Zimo221.7z> |
| Image2Lcd | <https://www.waveshare.net/w/upload/b/bd/Image2Lcd2.9.zip> |
| Flash_download_tool | <https://dl.espressif.com/public/flash_download_tool.zip> |

### 8.4 教程与参考文档

| 资料 | 链接 |
|---|---|
| 图片取模教程 | <https://www.waveshare.net/wiki/Image_extraction> |
| 字库取模教程 | <https://www.waveshare.net/wiki/E-Paper_Font_Tutorial> |
| MicroPython 文档 | <https://docs.micropython.org/en/latest/> |
| Arduino-ESP32 文档 | <https://docs.espressif.com/projects/arduino-esp32/en/latest/index.html> |
| arduino-esp32 仓库 | <https://github.com/espressif/arduino-esp32> |
| ESP-IDF 仓库 | <https://github.com/espressif/esp-idf> |
| Arduino 板管理教程 | <https://www.waveshare.net/wiki/Arduino_板管理教程> |
| Arduino 库管理教程 | <https://www.waveshare.net/wiki/Arduino_库管理教程> |
| 安装 Espressif IDF 插件教程 | <https://www.waveshare.net/wiki/安装Espressif_IDF插件教程> |

---

## 9. 附录：本项目已核对的引脚参考

> ⚠️ 本节内容**不来自 Wiki**（Wiki 无 GPIO 引脚表），而是本项目从 `firmware/main/bsp_pins.h` 提取的引脚定义。该定义已与官方 BSP 组件 **esp32_s3_touch_lcd_4b v2.0.0**（ESP Component Registry，源码仓库 `waveshareteam/Waveshare-ESP32-components`）核对一致（核对日期 2026-08-12）。**最终请以官方原理图 PDF 为准**：<https://www.waveshare.net/w/upload/8/82/ESP32-S3-Touch-LCD-4B.pdf>

### 9.1 屏幕规格

| 项目 | 值 |
|---|---|
| 分辨率 | 480×480 |
| 像素格式 | RGB565（RGB 总线，16 位数据线） |
| 像素时钟 | 12 MHz（保守取值，官方 ST7701 宏默认约 16 MHz） |

### 9.2 I2C 总线（TCA9554、GT911 等共享）

| 信号 | GPIO | 备注 |
|---|---|---|
| I2C SDA | GPIO47 | I2C1，400 kHz |
| I2C SCL | GPIO48 | — |
| TCA9554 I2C 地址 | 0x20 | A0=A1=A2=0（ADDRESS_000） |
| GT911 I2C 地址 | 0x5D | 官方 BSP 使用地址；0x14 为复位时 INT 电平选择的备选地址，未使用 |
| GT911 INT / RST | 未接线（NC） | 触摸驱动采用轮询模式；复位通过 TCA9554 时序完成 |

### 9.3 ST7701 三线 SPI 控制线（全部位于 TCA9554 扩展 IO 上）

| 信号 | TCA9554 引脚 |
|---|---|
| SPI CS | P0 |
| SPI SDA（MOSI） | P1 |
| SPI SCK | P2 |
| LCD 复位时序引脚 | P5（面板复位，手动时序） |
| 触摸复位时序引脚 | P6（触摸复位，手动时序） |

说明：官方 BSP **没有**独立的 ST7701 RST GPIO（panel reset 与 GT911 rst 均为 NC），而是在创建 panel IO 之前手动操作扩展器完成复位时序：P6 拉低 → P5 拉低（200 ms）→ P5 释放为高（200 ms）→ P6 恢复输入（200 ms）。

### 9.4 ST7701 RGB565 并行接口（直连 GPIO）

| 信号 | GPIO | 信号 | GPIO | 信号 | GPIO |
|---|---|---|---|---|---|
| PCLK | GPIO9 | R0 | GPIO40 | G0 | GPIO21 |
| DE | GPIO17 | R1 | GPIO41 | G1 | GPIO8 |
| HSYNC | GPIO46 | R2 | GPIO42 | G2 | GPIO18 |
| VSYNC | GPIO3 | R3 | GPIO2 | G3 | GPIO45 |
| DISP | NC | R4 | GPIO1 | G4 | GPIO38 |
|  |  |  |  | G5 | GPIO39 |
| B0 | GPIO10 | B1 | GPIO11 | B2 | GPIO12 |
| B3 | GPIO13 | B4 | GPIO14 |  |  |

### 9.5 背光

| 项目 | 值 |
|---|---|
| 背光 GPIO | GPIO4 |
| 驱动方式 | LEDC PWM（Timer 1 / Channel 1，5 kHz，10 位分辨率 0~1023） |
| 极性 | 低电平有效（duty = 1023 × (100 − 亮度百分比) / 100） |

### 9.6 与本项目相关的 Wiki 缺口汇总

- 无 GPIO 引脚表（中英文 Wiki 均无）
- 未给出例程所需 ESP-IDF 版本号
- 未给出板子具体尺寸数值
- FAQ 章节为空占位
