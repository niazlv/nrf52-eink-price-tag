#include "system_time.h"
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

static int64_t time_offset_sec = 0;

static int get_month_index(const char *m) {
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i=0; i<12; i++) {
        if (strncmp(m, months[i], 3) == 0) return i+1;
    }
    return 1; // Default
}

void system_time_init(void) {
#if defined(APP_BUILD_YEAR)
    /* Values injected at build time via Makefile → CMake → zephyr_compile_definitions. */
    set_system_time(APP_BUILD_HOUR, APP_BUILD_MIN, APP_BUILD_SEC,
                    APP_BUILD_DAY, APP_BUILD_MONTH, APP_BUILD_YEAR);
#else
    /* Fallback: compiler-provided macros. Only accurate for pristine builds
     * because __DATE__/__TIME__ are set per translation unit at compile time. */
    char m_str[4];
    int D, Y, h, m, s;
    sscanf(__DATE__, "%s %d %d", m_str, &D, &Y);
    sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
    int M = get_month_index(m_str);
    set_system_time(h, m, s, D, M, Y);
#endif
}

void set_system_time(int h, int m, int s, int D, int M, int Y) {
    struct tm t = {0};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = s;
    t.tm_isdst = -1;
    
    time_t target_ts = mktime(&t);
    int64_t uptime_sec = k_uptime_get() / 1000;
    
    time_offset_sec = (int64_t)target_ts - uptime_sec;
}

void get_system_time(struct tm *t) {
    if (!t) return;
    int64_t uptime_sec = k_uptime_get() / 1000;
    time_t now = (time_t)(uptime_sec + time_offset_sec);
    
    struct tm *tmp = gmtime(&now);
    if (tmp) *t = *tmp;
    else memset(t, 0, sizeof(struct tm));
}
