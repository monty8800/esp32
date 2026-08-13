/**
 * ha_panel —— LVGL 温湿度面板（Arduino IDE 路线）
 *
 * 【当前阶段】步骤 2：业务层搬运 验收 sketch
 *
 * 本 sketch 是"步骤 2 业务层搬运"阶段的验收载体：
 *   - src/ 目录下的全部共享业务代码（HA 客户端、网络层、服务器客户端、
 *     天气客户端、cJSON、UI 页面、ui_drain、字体、curl_shim）
 *     会被 Arduino 构建系统编译并链接；
 *   - 验收标准 = 全量编译链接通过（build OK）；
 *   - 本阶段不接显示 / 触摸 / UI / 网络，setup()/loop() 保持最小。
 *
 * 【Tools 菜单设置】（arduino-esp32 3.3.11，目标板 Waveshare ESP32-S3-Touch-LCD-4B）
 *   - Board:              ESP32S3 Dev Module
 *   - PSRAM:              OPI PSRAM
 *   - Flash Size:         16MB (128Mb)
 *   - Partition Scheme:   16M Flash (3MB APP/9.9MB FATFS)
 *   - USB CDC On Boot:    Enabled
 */

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("[ha_panel] step2: business layer sources present, build OK");
}

void loop() {
    delay(1000);
}
