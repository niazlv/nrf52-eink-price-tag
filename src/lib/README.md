# Display/graphics library layout

Portable files:

- `graphics.h` / `graphics.c` - framebuffer canvas, rotation, pixels, rectangles, text and small UI primitives.
- `life.h` / `life.c` - display-independent Game of Life state/update logic.

Project-specific files:

- `../drivers/ssd1675a.*` - SSD1675A e-paper controller and board GPIO/SPI details.
- `../app/display_manager.*` - Zephyr thread, power/update policy and display driver calls.
- `../app/display_screens.*` - application screens built on top of `graphics`.

To reuse the drawing layer in another project, copy `graphics.*`, allocate one black/white buffer and optionally one red buffer, then call:

```c
graphics_canvas_t canvas;
uint8_t bw[GRAPHICS_BUFFER_SIZE(width, height)];
uint8_t red[GRAPHICS_BUFFER_SIZE(width, height)];

graphics_canvas_init(&canvas, width, height, bw, red, sizeof(bw));
graphics_set_canvas(&canvas);
graphics_clear(GFX_WHITE);
```

If the target display has no red plane, pass `NULL` as `red_buffer`.
