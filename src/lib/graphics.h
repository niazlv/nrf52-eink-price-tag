#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include <string.h>

#define DISPLAY_WIDTH  128
#define DISPLAY_HEIGHT 296
#define BUFFER_SIZE    (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

// Colors
#define GFX_BLACK 0
#define GFX_WHITE 1
#define GFX_RED   2
#define GFX_GRAY  3 // Simulated (Checkerboard Black/White)
#define GFX_PINK  4 // Simulated (Checkerboard Red/White)

// Functions
void graphics_init(void);
void graphics_clear(uint8_t color); // 0=Black, 1=White (Standard logic)
void graphics_draw_pixel(int x, int y, int color);
void graphics_draw_char(int x, int y, uint16_t c);
void graphics_draw_string(int x, int y, const char *str);
void graphics_draw_string_color(int x, int y, const char *str, int color);
void graphics_draw_string_scaled(int x, int y, const char *str, int scale);
void graphics_draw_string_color_scaled(int x, int y, const char *str, int color, int scale);
void graphics_draw_battery(int x, int y, int percent);
const uint8_t* graphics_get_buffer(void);
const uint8_t* graphics_get_red_buffer(void);
void graphics_set_rotation(int rotation); // 0, 1, 2, 3 (90 degree steps)
int graphics_get_width(void);
int graphics_get_height(void);

#endif // GRAPHICS_H
