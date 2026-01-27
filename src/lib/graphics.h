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

// Functions
void graphics_init(void);
void graphics_clear(uint8_t color); // 0=Black, 1=White (Standard logic)
void graphics_draw_pixel(int x, int y, int color);
void graphics_draw_char(int x, int y, char c);
void graphics_draw_string(int x, int y, const char *str);
const uint8_t* graphics_get_buffer(void);

#endif // GRAPHICS_H
