#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define GRAPHICS_DEFAULT_WIDTH  128
#define GRAPHICS_DEFAULT_HEIGHT 296
#define GRAPHICS_BUFFER_SIZE(width, height) (((width) * (height)) / 8)
#define DISPLAY_WIDTH  GRAPHICS_DEFAULT_WIDTH
#define DISPLAY_HEIGHT GRAPHICS_DEFAULT_HEIGHT
#define BUFFER_SIZE    GRAPHICS_BUFFER_SIZE(DISPLAY_WIDTH, DISPLAY_HEIGHT)

// Colors
#define GFX_BLACK 0
#define GFX_WHITE 1
#define GFX_RED   2
#define GFX_GRAY  3 // Simulated (Checkerboard Black/White)
#define GFX_PINK  4 // Simulated (Checkerboard Red/White)

typedef struct {
    int width;
    int height;
    int rotation;
    uint8_t *bw_buffer;
    uint8_t *red_buffer;
    size_t buffer_size;
} graphics_canvas_t;

void graphics_canvas_init(graphics_canvas_t *canvas,
                          int width,
                          int height,
                          uint8_t *bw_buffer,
                          uint8_t *red_buffer,
                          size_t buffer_size);
void graphics_set_canvas(graphics_canvas_t *canvas);
graphics_canvas_t *graphics_get_canvas(void);

void graphics_init(void);
void graphics_clear(uint8_t color); // 0=Black, 1=White (Standard logic)
void graphics_draw_pixel(int x, int y, int color);
void graphics_fill_rect(int x, int y, int width, int height, int color);
void graphics_draw_rect(int x, int y, int width, int height, int color);
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
