#ifndef APP_SYSTEM_TIME_H
#define APP_SYSTEM_TIME_H

#include <time.h>
#include <stdint.h>

void system_time_init(void);

/**
 * @brief Set the system time offset
 * 
 * @param h Hour
 * @param m Minute
 * @param s Second
 * @param D Day
 * @param M Month
 * @param Y Year
 */
void set_system_time(int h, int m, int s, int D, int M, int Y);

/**
 * @brief Get current system time
 * 
 * @param t Pointer to tm structure to fill
 */
void get_system_time(struct tm *t);

/**
 * @brief Set the system time from a unix timestamp (seconds since epoch).
 */
void set_system_time_unix(time_t ts);

/**
 * @brief Get the current system time as a unix timestamp (seconds since epoch).
 */
int64_t get_system_time_unix(void);

/**
 * @brief Get the firmware build time as a unix timestamp.
 *        Used to tell a freshly-flashed image from a restored clock.
 */
time_t system_time_get_build_unix(void);

#endif // APP_SYSTEM_TIME_H
