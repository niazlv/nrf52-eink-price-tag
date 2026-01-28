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

#endif // APP_SYSTEM_TIME_H
