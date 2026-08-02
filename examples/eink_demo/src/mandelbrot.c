#include "mandelbrot.h"
#include <gfx/graphics.h>

#define MANDELBROT_MAX_ITER 32

static int iteration_to_color(int iter)
{
    if (iter >= MANDELBROT_MAX_ITER) {
        return GFX_BLACK;
    }

    if (iter > 9) {
        return GFX_RED;
    }

    return GFX_WHITE;
}

void mandelbrot_draw(void)
{
    int width = graphics_get_width();
    int height = graphics_get_height();

    graphics_clear(GFX_WHITE);

    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            float x0 = -2.20f + ((float)px / (float)(width - 1)) * 3.20f;
            float y0 = -1.20f + ((float)py / (float)(height - 1)) * 2.40f;
            float x = 0.0f;
            float y = 0.0f;
            int iter = 0;

            while ((x * x + y * y <= 4.0f) && (iter < MANDELBROT_MAX_ITER)) {
                float xt = x * x - y * y + x0;
                y = 2.0f * x * y + y0;
                x = xt;
                iter++;
            }

            graphics_draw_pixel(px, py, iteration_to_color(iter));
        }
    }
}
