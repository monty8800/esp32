/**
 * spike_ide.ino
 * ESP32-S3-Touch-LCD-4B Arduino 移植 — 任务 #1 环境 Spike（Arduino IDE 版）
 *
 * 本文件由 arduino/src/main.cpp（PlatformIO 工程）整理而来，内容逻辑完全一致，
 * 仅调整了串口打印前缀，便于对照分析。原 main.cpp 未做任何修改。
 *
 * 验证三项技术前提：
 *   1. PSRAM：探测到 ~8MB OPI PSRAM（对应 platformio.ini 的 qio_opi + psram=enabled）
 *   2. POSIX API：pthread / pthread_mutex / pthread_cond_timedwait /
 *      clock_gettime / setenv+getenv（决定 curl shim / net_worker 可否沿用）
 *   3. esp_http_client：头文件与符号是否可链接（决定 curl shim 是否需要
 *      用 HTTPClient / WiFiClientSecure 重写）
 *
 * ============================================================================
 * Arduino IDE 手动设置（Tools 菜单，缺一不可）
 * ============================================================================
 *   Board:              ESP32S3 Dev Module
 *   PSRAM:              OPI PSRAM            ← 对应 qio_opi，不设则探测不到 8MB PSRAM
 *   Flash Size:         16MB (128Mb)
 *   Partition Scheme:   16M Flash (3MB APP/9.9MB FATFS)
 *                       说明：PlatformIO 工程的 partitions.csv 为 OTA 双分区布局
 *                       （2×6.9MB app + 2MB SPIFFS），Arduino IDE 内置方案没有
 *                       完全一致项，上面这个最接近。spike 本身不写分区、不用 OTA，
 *                       任选任意 16MB 方案均可。
 *   USB CDC On Boot:    Enabled              ← console 走原生 USB（对应
 *                                              -DARDUINO_USB_CDC_ON_BOOT=1）
 *   其余选项按默认即可。
 *
 * 关于 build_flags：platformio.ini 中的 -DLV_CONF_INCLUDE_SIMPLE / -DBOARD_HAS_PSRAM
 * 等宏本 spike 代码均未依赖（未使用 LVGL，未用条件编译引用这些宏），
 * 故 Arduino IDE 下无需任何额外宏定义。
 *
 * 串口输出回传：烧录后打开串口监视器（115200 baud），把以 [SPIKE] 开头的所有行
 * （连同 Chip/PSRAM/Flash 信息）原样回传即可。
 * ============================================================================
 */

#include <pthread.h>
#include <time.h>
#include <stdlib.h>

/* --- esp_http_client spike: include + 符号引用（不发起任何网络调用） --- */
#include <esp_http_client.h>

/* ========================================================================
 * Spike 2: POSIX API
 * ======================================================================== */

static pthread_mutex_t s_spike_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_spike_cond = PTHREAD_COND_INITIALIZER;

static void *spike_thread_entry(void *arg)
{
    (void)arg;
    Serial.printf("[SPIKE] POSIX: thread started, tid=%lu\n",
                  (unsigned long)pthread_self());

    /* clock_gettime(CLOCK_REALTIME) */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        Serial.printf("[SPIKE] POSIX: clock_gettime OK  sec=%lld nsec=%ld\n",
                      (long long)ts.tv_sec, (long)ts.tv_nsec);
    } else {
        Serial.printf("[SPIKE] POSIX: clock_gettime FAILED errno=%d\n", errno);
    }

    /* setenv / getenv */
    setenv("SPIKE_TEST", "1", 1);
    const char *v = getenv("SPIKE_TEST");
    Serial.printf("[SPIKE] POSIX: setenv+getenv: SPIKE_TEST=%s\n",
                  v ? v : "(null)");

    /* pthread_mutex + pthread_cond_timedwait（等待 50ms 超时） */
    pthread_mutex_lock(&s_spike_mtx);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 50L * 1000L * 1000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec  += 1;
    }
    int rc = pthread_cond_timedwait(&s_spike_cond, &s_spike_mtx, &deadline);
    pthread_mutex_unlock(&s_spike_mtx);
    Serial.printf("[SPIKE] POSIX: cond_timedwait rc=%d (%s)\n",
                  rc, rc == ETIMEDOUT ? "ETIMEDOUT as expected" : "unexpected");

    return NULL;
}

/** POSIX spike：编译通过 + 运行打印即可确认 API 可用性 */
static void spike_posix_check(void)
{
    Serial.println("[SPIKE] POSIX: ---- POSIX API check start ----");

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    int rc = pthread_create(&th, &attr, spike_thread_entry, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        Serial.printf("[SPIKE] POSIX: pthread_create FAILED rc=%d\n", rc);
        return;
    }
    pthread_join(th, NULL);
    Serial.println("[SPIKE] POSIX: pthread_join OK — POSIX check done");
}

/* ========================================================================
 * Spike 3: esp_http_client
 * ======================================================================== */

/** 只引用符号验证链接可用，绝不调用、不发起网络请求 */
static void spike_http_client_check(void)
{
    Serial.println("[SPIKE] HTTP: ---- esp_http_client check start ----");
    /* 取函数地址：若 esp_http_client 组件未编入，链接阶段即报错 */
    void *sym = (void *)&esp_http_client_init;
    Serial.printf("[SPIKE] HTTP: esp_http_client_init symbol @ %p => AVAILABLE\n", sym);
    Serial.println("[SPIKE] HTTP: check done (no network call made)");
}

/* ========================================================================
 * Arduino 入口
 * ======================================================================== */

void setup(void)
{
    Serial.begin(115200);
    delay(1500); /* 等待 USB CDC 就绪 */

    Serial.println("==============================================");
    Serial.println(" ESP32-S3-Touch-LCD-4B | Arduino IDE          ");
    Serial.println(" Port Spike — Task #1 Environment Validation  ");
    Serial.println("==============================================");
    Serial.printf("Chip: %s rev%d, %d core(s) @ %d MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores(), ESP.getCpuFreqMHz());

    /* --- Spike 1: PSRAM / Flash 探测（qio_opi 前提验证） --- */
    Serial.printf("[SPIKE] PSRAM: total=%u bytes (%.1f MB), free=%u bytes\n",
                  ESP.getPsramSize(), ESP.getPsramSize() / 1024.0 / 1024.0,
                  ESP.getFreePsram());
    Serial.printf("Flash  : chip size=%u bytes (%.1f MB)\n",
                  ESP.getFlashChipSize(),
                  ESP.getFlashChipSize() / 1024.0 / 1024.0);

    if (ESP.getPsramSize() == 0) {
        Serial.println("[SPIKE] PSRAM: NOT DETECTED! 检查 Tools > PSRAM 是否设为 OPI PSRAM");
    } else if (ESP.getPsramSize() >= 7000000) {
        Serial.println("[SPIKE] PSRAM: ~8MB OPI PSRAM OK");
    } else {
        Serial.println("[SPIKE] PSRAM: detected but size abnormal, check memory_type");
    }

    spike_posix_check();
    spike_http_client_check();

    Serial.println("==============================================");
    Serial.println(" Spike finished. Idling in loop().");
    Serial.println("==============================================");
}

void loop(void)
{
    delay(1000);
}
