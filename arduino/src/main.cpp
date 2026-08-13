/**
 * @file main.cpp
 * ESP32-S3-Touch-LCD-4B PlatformIO/Arduino 移植 — 任务 #1 环境 Spike
 *
 * 验证两个关键技术前提（编译级）：
 *   1. POSIX API：pthread / pthread_mutex / pthread_cond_timedwait /
 *      clock_gettime / setenv+getenv（决定 curl shim / net_worker 可否沿用）
 *   2. esp_http_client：头文件与符号是否可用（决定 curl shim 是否需要
 *      用 HTTPClient / WiFiClientSecure 重写）
 *
 * 本文件仅用于 spike，后续任务将替换为正式应用入口。
 */

#include <Arduino.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

/* --- esp_http_client spike: include + 符号引用（不发起任何网络调用） --- */
#include <esp_http_client.h>

/* ========================================================================
 * Spike 1: POSIX API
 * ======================================================================== */

static pthread_mutex_t s_spike_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_spike_cond = PTHREAD_COND_INITIALIZER;

static void *spike_thread_entry(void *arg)
{
    (void)arg;
    Serial.printf("[spike][posix] thread started, tid=%lu\n",
                  (unsigned long)pthread_self());

    /* clock_gettime(CLOCK_REALTIME) */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        Serial.printf("[spike][posix] clock_gettime OK  sec=%lld nsec=%ld\n",
                      (long long)ts.tv_sec, (long)ts.tv_nsec);
    } else {
        Serial.printf("[spike][posix] clock_gettime FAILED errno=%d\n", errno);
    }

    /* setenv / getenv */
    setenv("SPIKE_TEST", "1", 1);
    const char *v = getenv("SPIKE_TEST");
    Serial.printf("[spike][posix] setenv+getenv: SPIKE_TEST=%s\n",
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
    Serial.printf("[spike][posix] cond_timedwait rc=%d (%s)\n",
                  rc, rc == ETIMEDOUT ? "ETIMEDOUT as expected" : "unexpected");

    return NULL;
}

/** POSIX spike：编译通过 + 运行打印即可确认 API 可用性 */
void spike_posix_check(void)
{
    Serial.println("[spike][posix] ---- POSIX API check start ----");

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    int rc = pthread_create(&th, &attr, spike_thread_entry, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        Serial.printf("[spike][posix] pthread_create FAILED rc=%d\n", rc);
        return;
    }
    pthread_join(th, NULL);
    Serial.println("[spike][posix] pthread_join OK — POSIX check done");
}

/* ========================================================================
 * Spike 2: esp_http_client
 * ======================================================================== */

/** 只引用符号验证链接可用，绝不调用、不发起网络请求 */
void spike_http_client_check(void)
{
    Serial.println("[spike][http] ---- esp_http_client check start ----");
    /* 取函数地址：若 esp_http_client 组件未编入，链接阶段即报错 */
    void *sym = (void *)&esp_http_client_init;
    Serial.printf("[spike][http] esp_http_client.h included, esp_http_client_init symbol @ %p => AVAILABLE\n", sym);
    Serial.println("[spike][http] check done (no network call made)");
}

/* ========================================================================
 * Arduino 入口
 * ======================================================================== */

void setup(void)
{
    Serial.begin(115200);
    delay(1500); /* 等待 USB CDC 就绪 */

    Serial.println("==============================================");
    Serial.println(" ESP32-S3-Touch-LCD-4B | PlatformIO Arduino   ");
    Serial.println(" Port Spike — Task #1 Environment Validation  ");
    Serial.println("==============================================");
    Serial.printf("Chip: %s rev%d, %d core(s) @ %d MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores(), ESP.getCpuFreqMHz());

    /* PSRAM / Flash 探测（Octal PSRAM qio_opi 前提验证） */
    Serial.printf("PSRAM  : total=%u bytes, free=%u bytes\n",
                  ESP.getPsramSize(), ESP.getFreePsram());
    Serial.printf("Flash  : chip size=%u bytes (%.1f MB)\n",
                  ESP.getFlashChipSize(),
                  ESP.getFlashChipSize() / 1024.0 / 1024.0);

    if (ESP.getPsramSize() == 0) {
        Serial.println("[WARN] PSRAM not detected! check qio_opi / psram=enabled");
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
