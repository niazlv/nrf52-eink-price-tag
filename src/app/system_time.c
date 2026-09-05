#include "system_time.h"
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

static int64_t time_offset_sec = 0;

#if !defined(APP_BUILD_YEAR)
/* Only the __DATE__ fallback needs this; CMakeLists.txt always defines the
 * APP_BUILD_* stamp, so in the normal build it would be an unused function. */
static int get_month_index(const char *m) {
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i=0; i<12; i++) {
        if (strncmp(m, months[i], 3) == 0) return i+1;
    }
    return 1; // Default
}
#endif

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

void set_system_time_unix(time_t ts) {
    int64_t uptime_sec = k_uptime_get() / 1000;
    time_offset_sec = (int64_t)ts - uptime_sec;
}

int64_t get_system_time_unix(void) {
    return k_uptime_get() / 1000 + time_offset_sec;
}

time_t system_time_get_build_unix(void) {
    struct tm t = {0};
#if defined(APP_BUILD_YEAR)
    t.tm_year = APP_BUILD_YEAR - 1900;
    t.tm_mon  = APP_BUILD_MONTH - 1;
    t.tm_mday = APP_BUILD_DAY;
    t.tm_hour = APP_BUILD_HOUR;
    t.tm_min  = APP_BUILD_MIN;
    t.tm_sec  = APP_BUILD_SEC;
#else
    char m_str[4];
    int D, Y, h, m, s;
    sscanf(__DATE__, "%s %d %d", m_str, &D, &Y);
    sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
    t.tm_year = Y - 1900;
    t.tm_mon  = get_month_index(m_str) - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;
#endif
    t.tm_isdst = -1;
    return mktime(&t);
}
