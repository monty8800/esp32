# 米家空气净化器接入仿真器：温湿度/PM2.5 显示 + 档位/开关控制

## 背景与约束

- HA 地址：`http://192.168.9.207:8123`（纯 HTTP，无 TLS）
- 设备：米家空气净化器（型号 zhimi.airpurifier.m1）。实体 ID **全部已确认**（经 `/api/states` 实测）：
  - 温度：`sensor.zhimi_cn_56424062_m1_temperature_p_3_3`（state 形如 `"25.8"`，单位 °C）
  - 湿度：`sensor.zhimi_cn_56424062_m1_relative_humidity_p_3_1`（state 形如 `"51"`，单位 %）
  - PM2.5：`sensor.zhimi_cn_56424062_m1_pm2_5_density_p_3_2`（state 形如 `"106"`，单位 µg/m³）
  - 档位/模式：`select.zhimi_cn_56424062_m1_mode_p_2_2`（state 为**中文字符串**，options：`自动` / `睡眠` / `最爱`，当前 `自动`）——即用户所说的档位控制
  - 开关：`switch.zhimi_cn_56424062_m1_on_p_2_1`（state 为 `on`/`off`，当前 `on`）
- 认证：Long-Lived Access Token，**用户已提供**；**严禁写入源码**，实施时写入本地文件 `~/.ha_esp32_token`（权限 600，不入代码库），代码从环境变量 `HA_TOKEN`（优先）或该文件（回退）读取
- 项目分层约定（`src/main.c` 头部注释、`src/ui/ui_demo.h`）：`src/ui/` 只含纯 LVGL 代码，网络等平台代码放 `src/ui/` 之外
- 线程模型：`lv_conf.h` 中 `LV_USE_OS = LV_OS_NONE`，单线程；采用 lv_timer 内同步拉取 + 严格超时封顶，**不引入线程**
- 依赖实测结论：macOS SDK 自带 libcurl 8.7.1，`#include <curl/curl.h>` 编译、`-lcurl` 链接均通过；CMake 用 `find_package(CURL REQUIRED)` 探测
- JSON 解析：`GET /api/states/<entity_id>` 返回单对象，顶层 `"state"` 即数值字符串，用手写提取（strstr+边界扫描），零新依赖
- 中文档位名处理：内建 Montserrat 字体（U+0020–U+00FF）无法渲染中文，故 UI 侧把 `自动/睡眠/最爱` 映射为英文标签 `AUTO/SLEEP/FAV` 显示；控制时反向映射，POST 给 HA 的仍是 UTF-8 中文字符串（C 源码为 UTF-8，直接写字面量即可）；不动 lv_conf.h 字体配置
- 控制方式：HA REST `POST /api/states/<entity_id>`，body `{"state": "..."}`，select 传中文选项、switch 传 `on`/`off`；事件回调在主循环内同步执行（局域网下毫秒级，超时封顶 2s）

## 令牌落地（用户已提供令牌）

- 实施第一步：把用户提供的令牌写入 `~/.ha_esp32_token`（`chmod 600`），此后以文件回退方式运行 sim，无需每次带环境变量
- 令牌严禁出现在任何源码、CMake、日志中；日志只打印令牌长度

## 代码变更

### 新建 `src/ha_client.h` / `src/ha_client.c`（平台层，libcurl + 手写 JSON 提取）

- 配置读取：
  - 令牌：`getenv("HA_TOKEN")` → 回退读 `~/.ha_esp32_token`（去尾部换行）；缺失时返回错误，UI 显示 NO TOKEN，日志只打印令牌长度不打印内容
  - Base URL：`getenv("HA_BASE_URL")`，默认 `http://192.168.9.207:8123`
  - 实体 ID：集中 `#define` 五个实体 ID（见「背景与约束」已确认值），允许 `HA_ENTITY_TEMP`/`HA_ENTITY_HUM`/`HA_ENTITY_PM25`/`HA_ENTITY_MODE`/`HA_ENTITY_POWER` 环境变量覆盖
- `ha_client_init(void)`：`curl_global_init`、读取配置，返回成功与否
- `bool ha_client_fetch_state(const char * entity_id, char * out, size_t out_len)`：
  - `curl_easy` GET `<base>/api/states/<entity_id>`，头 `Authorization: Bearer <token>`
  - `CURLOPT_TIMEOUT_MS 2000`、`CURLOPT_CONNECTTIMEOUT_MS 800`、`CURLOPT_NOSIGNAL 1`（UI 阻塞封顶 ~2s，30s 才一次）
  - write 回调累积到固定 4KB 缓冲；HTTP 200 后从响应中定位 `"state"` 键并复制其字符串值到 `out`（处理引号边界；state 值不含转义引号，风险低）
  - 401/403 → 日志提示令牌无效；其他错误返回 false
- `bool ha_client_set_state(const char * entity_id, const char * state_value)`：
  - `curl_easy` POST `<base>/api/states/<entity_id>`，头增加 `Content-Type: application/json`，body 固定格式 `{"state":"<value>"}`（state 值不含双引号，手工拼接即可）
  - 同样的超时设置；HTTP 200 视为成功，日志打印 entity 与结果

### 新建 `src/ui/env_panel.h` / `src/ui/env_panel.c`（纯 LVGL，可移植，不含任何网络/平台代码）

- 严格模仿 `src/ui/ui_demo.c` 现有模式：自带 `style_panel`/`make_kicker`/`add_corner_tick` 静态副本（不改 ui_demo.c）、复用调色板宏值、`lv_font_montserrat_14/16/20`
- `env_panel_create(lv_obj_t * scr)`：单面板「AIR PURIFIER · M1」，内部两区：
  - **数据区**：温度、湿度、PM2.5 三个数值 label（初始 `--`，如 `25.8 °C` / `51 %` / `106 µg/m³`，Montserrat 覆盖 ° 与 µ），横向三列布局
  - **控制区**：档位三按钮行（`AUTO`/`SLEEP`/`FAV`，选中态用 COL_ACCENT 高亮、未选 COL_TEXT_DIM）+ 电源 `lv_switch`（仿 ui_demo.c 现有 switch 面板样式）
  - 底部一行小字状态：`LIVE` / `OFFLINE` / `NO TOKEN`
- 控制回调注入（保持 UI 层纯净）：`env_panel_set_control_cb(void (*cb)(int action, void * user_data), void * user_data)`，action 枚举：`ENV_ACT_MODE_AUTO/SLEEP/FAV`、`ENV_ACT_POWER_ON/OFF`；用户操作时面板先**乐观更新** UI（按钮高亮/switch 状态立即变），再调回调
- `env_panel_update(...)`：接收三个传感器值 + 档位索引 + 电源状态 + 状态文本；仅在数值变化时 `lv_label_set_text_fmt`；每 30s 轮询回写以同步 HA 侧外部变更（外部改了档位/开关时 UI 自动跟上）
- 面板高度预计 ~150-180px，追加在现有 flex 列末尾 → 屏幕内容超出 480px，依赖 LVGL 默认滚动，属预期行为

### 修改 `src/main.c`（~15 行）

- 头部 `#include "ha_client.h"`、`#include "ui/env_panel.h"`
- `ui_demo_create();`（L81）之后：`env_panel_create(lv_screen_active());`
- `ha_client_init()` + `env_panel_set_control_cb(env_control_cb, NULL)` + `lv_timer_create(ha_refresh_cb, 30000, NULL)`
- `ha_refresh_cb`：依次拉取温度/湿度/PM2.5/档位/开关五个实体，档位中文字符串映射为索引（`自动`→0、`睡眠`→1、`最爱`→2，匹配失败保持 UI 现状），汇总调 `env_panel_update`；创建 timer 后先手动执行一次（启动即显示）
- `env_control_cb`：action → 实体/取值映射（AUTO→`自动`、SLEEP→`睡眠`、FAV→`最爱`；POWER_ON/OFF→`on`/`off`），调 `ha_client_set_state`；失败时日志提示并依赖下一轮轮询把 UI 纠正回真实状态

### 修改 `CMakeLists.txt`（~8 行）

- `find_package(CURL REQUIRED)`（放在 SDL2 解析段之后）
- `sim` 源列表追加 `src/ha_client.c`、`src/ui/env_panel.c`
- `target_link_libraries(sim PRIVATE CURL::libcurl)`——注意：libcurl 只被 sim 侧代码使用、不进 lvgl 库，故**不**按 L42 的 PUBLIC 模式链到 lvgl

### 不改动

`src/ui/ui_demo.c`、`lv_conf.h`、`lvgl/` 全部保持原样。

## 实施步骤与依赖

1. **令牌落地**：写入 `~/.ha_esp32_token`（chmod 600）；实体 ID 已确认，无需再枚举
2. 新建 `src/ha_client.*` 与 `src/ui/env_panel.*`（可并行）
3. 修改 `src/main.c`、`CMakeLists.txt`（依赖 2 的接口签名）
4. 增量构建：`cmake --build build_dbg`
5. 运行验证：`./build_dbg/sim`（令牌自动从 `~/.ha_esp32_token` 读取）

## 测试计划

- **构建**：`build_dbg` 增量编译零错误；现有 sim 行为回归检查（tap/slider/switch 面板不受影响）
- **正常路径**：运行 sim，stderr 打印拉取结果，UI 面板显示真实温湿度/PM2.5 数值与当前档位/开关状态
- **控制路径**：点击档位按钮 → 面板高亮切换且 HA 侧档位实体状态实际变更（用 curl 复核）；拨动电源 switch → HA 开关实体 on/off 翻转且净化器实际响应；在 HA 界面外部改档位/开关 → ≤30s 内 UI 自动同步
- **异常路径**：错误令牌 → UI 显示 NO TOKEN/OFFLINE 且不崩溃；HA 不可达（临时改 `HA_BASE_URL` 指向无效地址）→ 阻塞 ≤2s、显示 OFFLINE、30s 后自动重试恢复；控制请求失败 → 乐观 UI 在下一轮轮询被纠正，不崩溃

## 已否决的备选方案

- **后台 pthread + 信箱架构**：彻底避免 UI 阻塞且可扩展性最好，但对"小案例"过重；2s 超时封顶 + 30s 周期的同步方案代价可接受，留作未来升级路径
- **直接修改 `ui_demo.c` 内嵌面板**：会破坏"ui_demo 零改动"的低回归目标，故面板独立成 `env_panel`
- **开启 `LV_USE_OS = LV_OS_PTHREAD`** 以获得跨线程 `lv_async_call`：改变全局 LVGL 行为、风险大，否决
- **引入 cJSON / LVGL 内建 JSON**：单实体顶层 `state` 提取用手写足够；LVGL v9.3 已无 lv_json，cJSON 仅作嵌套解析被迫时的后手
- **自造中文字体显示档位名**：需改 lv_conf.h 字体配置并增大字体体积，收益小；英文标签映射更简单