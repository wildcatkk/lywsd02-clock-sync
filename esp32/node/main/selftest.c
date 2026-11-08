#include "selftest.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_clock.h"
#include "cluster.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jobs.h"
#include "lywsd02.h"
#include "ntp.h"
#include "selftest_vectors.h"
#include "store.h"
#include "wifi.h"

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static int failures;

static void check(bool ok, const char *what, int index, const char *detail)
{
    if (!ok) {
        failures++;
        printf("FAIL %s #%d %s\n", what, index, detail);
    }
}

static bool same(double a, double b)
{
    return (isnan(a) && isnan(b)) || fabs(a - b) < 1e-9;
}

static void set_zone(const char *tz)
{
    wifi_apply_timezone(tz);
}

static void restore_zone(void)
{
    store_lock();
    char posix[72];
    snprintf(posix, sizeof(posix), "%s", json_str(store_config(), "timezone", ""));
    store_unlock();
    wifi_apply_timezone(posix);
}

static void run_selftest(void)
{
    failures = 0;
    char detail[96];
    int total = 0;

    for (size_t i = 0; i < COUNT(TIME_VECTORS); i++, total++) {
        set_zone(TIME_VECTORS[i].tz);
        int offset = lywsd02_utc_offset((time_t)TIME_VECTORS[i].now);
        snprintf(detail, sizeof(detail), "%s offset %d want %d", TIME_VECTORS[i].tz, offset,
                 TIME_VECTORS[i].offset);
        check(offset == TIME_VECTORS[i].offset, "offset", i, detail);
        uint8_t out[5];
        lywsd02_encode_time(TIME_VECTORS[i].now, out);
        snprintf(detail, sizeof(detail), "%s got %02x%02x%02x%02x%02x", TIME_VECTORS[i].tz,
                 out[0], out[1], out[2], out[3], out[4]);
        check(memcmp(out, TIME_VECTORS[i].encoded, 5) == 0, "encode", i, detail);
    }

    for (size_t i = 0; i < COUNT(CLOCK_VECTORS); i++, total++) {
        set_zone(CLOCK_VECTORS[i].tz);
        uint32_t stamp;
        int timezone;
        bool ok = lywsd02_decode_time(CLOCK_VECTORS[i].raw, CLOCK_VECTORS[i].length, &stamp,
                                      &timezone);
        double shown = ok ? lywsd02_shown_time(stamp, timezone, CLOCK_VECTORS[i].now) : NAN;
        double drift = shown - floor(CLOCK_VECTORS[i].now);
        snprintf(detail, sizeof(detail), "%s shown %.1f tz %d drift %.1f", CLOCK_VECTORS[i].tz,
                 shown, timezone, drift);
        check(ok && same(shown, CLOCK_VECTORS[i].shown) && timezone == CLOCK_VECTORS[i].timezone
              && same(drift, CLOCK_VECTORS[i].drift), "clock", i, detail);
    }
    restore_zone();

    for (size_t i = 0; i < COUNT(ENV_VECTORS); i++, total++) {
        double temperature = NAN, voltage = NAN;
        int humidity = -1;
        bool ok = lywsd02_decode_env(ENV_VECTORS[i].data, ENV_VECTORS[i].length, &temperature,
                                     &humidity, &voltage);
        snprintf(detail, sizeof(detail), "ok %d t %.3f h %d v %.3f", ok, temperature, humidity,
                 voltage);
        bool good = ok == ENV_VECTORS[i].ok;
        if (good && ok) {
            good = same(temperature, ENV_VECTORS[i].temperature)
                   && humidity == ENV_VECTORS[i].humidity && same(voltage, ENV_VECTORS[i].voltage);
        }
        check(good, "env", i, detail);
    }

    for (size_t i = 0; i < COUNT(ADV_VECTORS); i++, total++) {
        lywsd02_adv_t adv;
        lywsd02_adv_init(&adv);
        lywsd02_parse_adv(ADV_VECTORS[i].data, ADV_VECTORS[i].length, &adv);
        const char *want = ADV_VECTORS[i].name ? ADV_VECTORS[i].name : "";
        bool clock = lywsd02_looks_like_clock(&adv);
        snprintf(detail, sizeof(detail), "%s: name '%s' pid %d clock %d", ADV_VECTORS[i].what,
                 adv.name, adv.product_id, clock);
        check(strcmp(adv.name, want) == 0 && adv.product_id == ADV_VECTORS[i].product_id
              && clock == ADV_VECTORS[i].clock, "adv", i, detail);
    }

    for (size_t i = 0; i < COUNT(BUCKET_VECTORS); i++, total++) {
        int bucket = cluster_bucket(BUCKET_VECTORS[i].rssi);
        snprintf(detail, sizeof(detail), "rssi %d bucket %d", BUCKET_VECTORS[i].rssi, bucket);
        check(bucket == BUCKET_VECTORS[i].bucket, "bucket", i, detail);
    }

    char nested[34];
    for (int depth = 16; depth <= 17; depth++, total++) {
        memset(nested, '[', depth);
        memset(nested + depth, ']', depth);
        check(cluster_json_bounded(nested, depth * 2) == (depth == 16),
              "json depth", depth, "depth limit 16");
    }
    static const char escaped[] = "{\"text\":\"[]{}\\\"quoted\"}";
    total++;
    check(cluster_json_bounded(escaped, sizeof(escaped) - 1), "json string", 0, "escaped quote");
    static const char embedded[] = {'[', 0, ']'};
    total++;
    check(!cluster_json_bounded(embedded, sizeof(embedded)), "json nul", 0, "embedded NUL");
    static const char *const utf8[][2] = {
        {"\"\xe5\xae\xa2\xe5\x8e\x85\"", "1"}, {"\"\xf0\x9f\x98\x80\"", "1"},
        {"\"\xbf\xcd\xcc\xfc\"", ""}, {"\"\xe5\xae\"", ""}, {"\"\xc0\xaf\"", ""},
        {"\"\xed\xa0\x80\"", ""}, {"\"\xf4\x90\x80\x80\"", ""},
    };
    for (size_t i = 0; i < COUNT(utf8); i++, total++) {
        check(cluster_json_bounded(utf8[i][0], strlen(utf8[i][0])) == !!utf8[i][1][0],
              "json utf-8", i, "well-formed UTF-8 only");
    }
    // 30 ASCII bytes and then a 3-byte character the 31-byte buffer would cut.
    uint8_t long_name[2 + 33] = {33, 0x09};
    memset(long_name + 2, 'A', 30);
    memcpy(long_name + 32, "\xe5\xae\xa2", 3);
    lywsd02_adv_t cut;
    lywsd02_adv_init(&cut);
    lywsd02_parse_adv(long_name, sizeof(long_name), &cut);
    total++;
    check(strlen(cut.name) == 30, "adv utf-8", 0, "partial character dropped");

    static const uint8_t addr[6] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
    char key[13], address[18];
    uint8_t back[6];
    lywsd02_key(addr, key);
    lywsd02_address(addr, address);
    total++;
    check(strcmp(key, "AB8967452301") == 0 && strcmp(address, "AB:89:67:45:23:01") == 0
          && lywsd02_parse_key(address, back) && memcmp(back, addr, 6) == 0
          && lywsd02_parse_key(key, back) && memcmp(back, addr, 6) == 0
          && !lywsd02_parse_key("ab89674523", back), "address", 0, key);

    printf("selftest: %d checks, %d failed\n", total, failures);
}

static void heap(const char *when)
{
    printf("heap %s: free %u, lowest %u, largest %u\n", when,
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void ble_cycle(int rounds)
{
    // Take the lifecycle lock before checking pending: a new worker can queue but
    // cannot start/stop BLE until the entire diagnostic cycle has released it.
    if (!jobs_ble_try_lock()) {
        printf("blecycle: radio lifecycle unavailable or in use\n");
        return;
    }
    store_lock();
    bool busy = jobs_any_pending();
    store_unlock();
    if (busy) {
        jobs_ble_unlock();
        printf("blecycle: a job is running or queued\n");
        return;
    }
    heap("before");
    for (int i = 1; i <= rounds; i++) {
        esp_err_t err = ble_clock_start();
        if (err != ESP_OK) {
            printf("blecycle %d: start failed %s\n", i, esp_err_to_name(err));
            ble_clock_stop();
            break;
        }
        char when[24];
        snprintf(when, sizeof(when), "running %d", i);
        heap(when);
        // stop() reports teardown errors itself and blocks reuse on failure.
        ble_clock_stop();
        // The host task deletes itself; the idle task frees it a moment later.
        vTaskDelay(pdMS_TO_TICKS(500));
        snprintf(when, sizeof(when), "after %d", i);
        heap(when);
    }
    jobs_ble_unlock();
}

bool selftest_command(const char *line)
{
    if (strcmp(line, "selftest") == 0) {
        run_selftest();
    } else if (strcmp(line, "heap") == 0) {
        heap("now");
    } else if (strncmp(line, "blecycle", 8) == 0 && (line[8] == 0 || line[8] == ' ')) {
        int rounds = atoi(line + 8);
        ble_cycle(rounds > 0 ? rounds : 5);
    } else if (strncmp(line, "ntp", 3) == 0 && (line[3] == 0 || line[3] == ' ')) {
        char host[65];
        const char *given = line + 3;
        while (*given == ' ') {
            given++;
        }
        store_lock();
        snprintf(host, sizeof(host), "%s",
                 *given ? given : json_str(store_config(), "time_server", ""));
        store_unlock();
        if (!host[0]) {
            snprintf(host, sizeof(host), "ntp.aliyun.com");
        }
        double offset;
        char error[160];
        if (ntp_query(host, &offset, error, sizeof(error))) {
            printf("ntp %s: offset %+.3f s\n", host, offset);
        } else {
            printf("ntp %s: %s\n", host, error);
        }
    } else {
        return false;
    }
    return true;
}
