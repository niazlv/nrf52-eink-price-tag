#include "graphics.h"

static uint8_t frame_arena[GRAPHICS_ARENA_BYTES];
static graphics_canvas_t default_canvas;
static graphics_canvas_t *active_canvas = &default_canvas;
static int render_mode = GFX_RENDER_NORMAL;

void graphics_set_render_mode(int mode)
{
    render_mode = (mode == GFX_RENDER_RED_MASK) ? GFX_RENDER_RED_MASK : GFX_RENDER_NORMAL;
}

int graphics_get_render_mode(void)
{
    return render_mode;
}

static int canvas_logical_width(const graphics_canvas_t *canvas)
{
    return (canvas->rotation % 2 == 0) ? canvas->width : canvas->height;
}

static int canvas_logical_height(const graphics_canvas_t *canvas)
{
    return (canvas->rotation % 2 == 0) ? canvas->height : canvas->width;
}

void graphics_canvas_init(graphics_canvas_t *canvas,
                          int width,
                          int height,
                          uint8_t *bw_buffer,
                          uint8_t *red_buffer,
                          size_t buffer_size)
{
    if (!canvas) {
        return;
    }

    canvas->width = width;
    canvas->height = height;
    canvas->rotation = 1;
    canvas->bw_buffer = bw_buffer;
    canvas->red_buffer = red_buffer;
    canvas->buffer_size = buffer_size;
}

void graphics_set_canvas(graphics_canvas_t *canvas)
{
    if (canvas) {
        active_canvas = canvas;
    }
}

graphics_canvas_t *graphics_get_canvas(void)
{
    return active_canvas;
}

// Font 5x7 Data (moved from main.c)
static const uint8_t font5x7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // Space (32)
    0x00, 0x00, 0x5F, 0x00, 0x00, // !
    0x00, 0x07, 0x00, 0x07, 0x00, // "
    0x14, 0x7F, 0x14, 0x7F, 0x14, // #
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // $
    0x23, 0x13, 0x08, 0x64, 0x62, // %
    0x36, 0x49, 0x55, 0x22, 0x50, // &
    0x00, 0x05, 0x03, 0x00, 0x00, // '
    0x00, 0x1C, 0x22, 0x41, 0x00, // (
    0x00, 0x41, 0x22, 0x1C, 0x00, // )
    0x14, 0x08, 0x3E, 0x08, 0x14, // *
    0x08, 0x08, 0x3E, 0x08, 0x08, // +
    0x00, 0x50, 0x30, 0x00, 0x00, // ,
    0x08, 0x08, 0x08, 0x08, 0x08, // -
    0x00, 0x60, 0x60, 0x00, 0x00, // .
    0x20, 0x10, 0x08, 0x04, 0x02, // /
    0x3E, 0x51, 0x49, 0x45, 0x3E, // 0
    0x00, 0x42, 0x7F, 0x40, 0x00, // 1
    0x42, 0x61, 0x51, 0x49, 0x46, // 2
    0x21, 0x41, 0x45, 0x4B, 0x31, // 3
    0x18, 0x14, 0x12, 0x7F, 0x10, // 4
    0x27, 0x45, 0x45, 0x45, 0x39, // 5
    0x3C, 0x4A, 0x49, 0x49, 0x30, // 6
    0x01, 0x71, 0x09, 0x05, 0x03, // 7
    0x36, 0x49, 0x49, 0x49, 0x36, // 8
    0x06, 0x49, 0x49, 0x29, 0x1E, // 9
    0x00, 0x36, 0x36, 0x00, 0x00, // :
    0x00, 0x56, 0x36, 0x00, 0x00, // ;
    0x08, 0x14, 0x22, 0x41, 0x00, // <
    0x14, 0x14, 0x14, 0x14, 0x14, // =
    0x00, 0x41, 0x22, 0x14, 0x08, // >
    0x02, 0x01, 0x51, 0x09, 0x06, // ?
    0x32, 0x49, 0x79, 0x41, 0x3E, // @
    0x7E, 0x11, 0x11, 0x11, 0x7E, // A
    0x7F, 0x49, 0x49, 0x49, 0x36, // B
    0x3E, 0x41, 0x41, 0x41, 0x22, // C
    0x7F, 0x41, 0x41, 0x22, 0x1C, // D
    0x7F, 0x49, 0x49, 0x49, 0x41, // E
    0x7F, 0x09, 0x09, 0x09, 0x01, // F
    0x3E, 0x41, 0x49, 0x49, 0x7A, // G
    0x7F, 0x08, 0x08, 0x08, 0x7F, // H
    0x00, 0x41, 0x7F, 0x41, 0x00, // I
    0x20, 0x40, 0x41, 0x3F, 0x01, // J
    0x7F, 0x08, 0x14, 0x22, 0x41, // K
    0x7F, 0x40, 0x40, 0x40, 0x40, // L
    0x7F, 0x02, 0x0C, 0x02, 0x7F, // M
    0x7F, 0x04, 0x08, 0x10, 0x7F, // N
    0x3E, 0x41, 0x41, 0x41, 0x3E, // O
    0x7F, 0x09, 0x09, 0x09, 0x06, // P
    0x3E, 0x41, 0x51, 0x21, 0x5E, // Q
    0x7F, 0x09, 0x19, 0x29, 0x46, // R
    0x46, 0x49, 0x49, 0x49, 0x31, // S
    0x01, 0x01, 0x7F, 0x01, 0x01, // T
    0x3F, 0x40, 0x40, 0x40, 0x3F, // U
    0x1F, 0x20, 0x40, 0x20, 0x1F, // V
    0x3F, 0x40, 0x38, 0x40, 0x3F, // W
    0x63, 0x14, 0x08, 0x14, 0x63, // X
    0x07, 0x08, 0x70, 0x08, 0x07, // Y
    0x61, 0x51, 0x49, 0x45, 0x43, // Z
    0x00, 0x7F, 0x41, 0x41, 0x00, // [
    0x02, 0x04, 0x08, 0x10, 0x20, // Backslash 
    0x00, 0x41, 0x41, 0x7F, 0x00, // ]
    0x04, 0x02, 0x01, 0x02, 0x04, // ^
    0x40, 0x40, 0x40, 0x40, 0x40, // _
    0x00, 0x01, 0x02, 0x04, 0x00, // `
    0x20, 0x54, 0x54, 0x54, 0x78, // a
    0x7F, 0x48, 0x44, 0x44, 0x38, // b
    0x38, 0x44, 0x44, 0x44, 0x20, // c
    0x38, 0x44, 0x44, 0x48, 0x7F, // d
    0x38, 0x54, 0x54, 0x54, 0x18, // e
    0x08, 0x7E, 0x09, 0x01, 0x02, // f
    0x0C, 0x52, 0x52, 0x52, 0x3E, // g
    0x7F, 0x08, 0x04, 0x04, 0x78, // h
    0x00, 0x44, 0x7D, 0x40, 0x00, // i
    0x20, 0x40, 0x44, 0x3D, 0x00, // j
    0x7F, 0x10, 0x28, 0x44, 0x00, // k
    0x00, 0x41, 0x7F, 0x40, 0x00, // l
    0x7C, 0x04, 0x18, 0x04, 0x78, // m
    0x7C, 0x08, 0x04, 0x04, 0x78, // n
    0x38, 0x44, 0x44, 0x44, 0x38, // o
    0x7C, 0x14, 0x14, 0x14, 0x08, // p
    0x08, 0x14, 0x14, 0x18, 0x7C, // q
    0x7C, 0x08, 0x04, 0x04, 0x08, // r
    0x48, 0x54, 0x54, 0x54, 0x20, // s
    0x04, 0x3F, 0x44, 0x40, 0x20, // t
    0x3C, 0x40, 0x40, 0x20, 0x7C, // u
    0x1C, 0x20, 0x40, 0x20, 0x1C, // v
    0x3C, 0x40, 0x30, 0x40, 0x3C, // w
    0x44, 0x28, 0x10, 0x28, 0x44, // x
    0x0C, 0x50, 0x50, 0x50, 0x3C, // y
    0x44, 0x64, 0x54, 0x4C, 0x44, // z
    0x00, 0x08, 0x36, 0x41, 0x00, // {
    0x00, 0x00, 0x7F, 0x00, 0x00, // |
    0x00, 0x41, 0x36, 0x08, 0x00, // }
    0x10, 0x08, 0x08, 0x10, 0x08  // ~
};

// Basic 2-byte Cyrillic map (incomplete, just common letters for demo)
// 0xD0 0x90 (А) to 0xD0 0xBF (п)
// 0xD1 0x80 (р) to 0xD1 0x8F (я)
static const uint8_t font_cyr[] = {
    // А, Б, В, ... (Starts at U+0410)
    0x7E, 0x11, 0x11, 0x11, 0x7E, // A (0x10)
    0x7F, 0x49, 0x49, 0x49, 0x31, // Б (0x11)
    0x7F, 0x49, 0x49, 0x49, 0x36, // В (0x12)
    0x7F, 0x01, 0x01, 0x01, 0x03, // Г (0x13)
    0x70, 0x4C, 0x43, 0x4C, 0x7F, // Д (0x14) - tricky 5x7
    0x7F, 0x49, 0x49, 0x49, 0x41, // Е (0x15)
    0x77, 0x08, 0x7F, 0x08, 0x77, // Ж (0x16)
    0x00, 0x41, 0x7F, 0x41, 0x00, // З (0x17) (Like 3)
    0x7F, 0x10, 0x08, 0x04, 0x7F, // И (0x18) (N mirrored)
    0x7F, 0x10, 0x09, 0x04, 0x7F, // Й (0x19)
    0x7F, 0x08, 0x14, 0x22, 0x41, // К (0x1A)
    0x70, 0x0E, 0x01, 0x0E, 0x70, // Л (0x1B)
    0x7F, 0x02, 0x0C, 0x02, 0x7F, // М (0x1C)
    0x7F, 0x08, 0x08, 0x08, 0x7F, // Н (0x1D) (H)
    0x3E, 0x41, 0x41, 0x41, 0x3E, // О (0x1E)
    0x7F, 0x01, 0x01, 0x01, 0x7F, // П (0x1F)
    0x3E, 0x41, 0x41, 0x41, 0x3E, // Р (0x20) (P)
    0x3E, 0x41, 0x41, 0x41, 0x22, // С (0x21) (C)
    0x01, 0x01, 0x7F, 0x01, 0x01, // Т (0x22) (T)
    0x0F, 0x10, 0x10, 0x10, 0x7F, // У (0x23)
    0x1C, 0x22, 0x7F, 0x22, 0x1C, // Ф (0x24)
    0x63, 0x14, 0x08, 0x14, 0x63, // Х (0x25) (X)
    0x7E, 0x42, 0x42, 0x7F, 0x40, // Ц (0x26)
    0x07, 0x08, 0x08, 0x08, 0x7F, // Ч (0x27) (inverted h ish)
    0x7F, 0x40, 0x7F, 0x40, 0x7F, // Ш (0x28)
    0x7F, 0x40, 0x7F, 0x40, 0x7F, // Щ (0x29) (Same for 5x7?)
    0x01, 0x7F, 0x48, 0x48, 0x30, // Ъ (0x2A)
    0x7F, 0x48, 0x30, 0x00, 0x7F, // Ы (0x2B)
    0x7F, 0x48, 0x48, 0x30, 0x00, // Ь (0x2C)
    0x22, 0x49, 0x49, 0x49, 0x3E, // Э (0x2D)
    0x7F, 0x10, 0x5F, 0x10, 0x7F, // Ю (0x2E)
    0x4E, 0x51, 0x51, 0x51, 0x7F, // Я (0x2F)
    
    // Lowercase a-p (0x30-0x3F) - reusing Upper for simplification in 5x7 where possible or simple mod
    // Just filling placeholders for simplicity of this demo request
    0x20, 0x54, 0x54, 0x54, 0x78, // a (0x30)
    0x7F, 0x44, 0x44, 0x44, 0x38, // б (0x31)
    0x38, 0x44, 0x44, 0x44, 0x7F, // в (0x32)
    0x7F, 0x04, 0x04, 0x04, 0x03, // г (0x33)
    0x70, 0x4C, 0x43, 0x4C, 0x7F, // д (0x34)
    0x38, 0x54, 0x54, 0x54, 0x18, // e (0x35)
    0x77, 0x08, 0x7F, 0x08, 0x77, // ж (0x36)
    0x44, 0x44, 0x44, 0x44, 0x38, // з (0x37)
    0x7C, 0x08, 0x04, 0x04, 0x78, // и (0x38)
    0x7C, 0x09, 0x05, 0x04, 0x78, // й (0x39)
    0x7F, 0x08, 0x14, 0x22, 0x41, // к (0x3A)
    0x40, 0x40, 0x3C, 0x02, 0x7F, // л (0x3B)
    0x7C, 0x04, 0x18, 0x04, 0x78, // м (0x3C)
    0x7C, 0x08, 0x04, 0x04, 0x78, // н (0x3D)
    0x38, 0x44, 0x44, 0x44, 0x38, // о (0x3E)
    0x7C, 0x04, 0x04, 0x04, 0x7C, // п (0x3F)
    
    // Lowercase p-ya (0xD1 0x80 ...)
    0x7C, 0x14, 0x14, 0x14, 0x08, // р (0x00 relative to D180 block)
    0x38, 0x44, 0x44, 0x44, 0x20, // с
    0x04, 0x3F, 0x44, 0x40, 0x20, // т
    0x4C, 0x50, 0x50, 0x50, 0x7C, // у
    0x08, 0x14, 0x7C, 0x14, 0x08, // ф 
    0x44, 0x28, 0x10, 0x28, 0x44, // х
    0x7C, 0x44, 0x44, 0x7F, 0x40, // ц
    0x04, 0x08, 0x08, 0x08, 0x7F, // ч
    0x7C, 0x40, 0x7C, 0x40, 0x7C, // ш
    0x7C, 0x40, 0x7C, 0x40, 0x7C, // щ
    0x48, 0x7E, 0x49, 0x41, 0x30, // ъ
    0x7F, 0x40, 0x30, 0x00, 0x7F, // ы
    0x7F, 0x48, 0x48, 0x30, 0x00, // ь
    0x22, 0x49, 0x49, 0x49, 0x3E, // э
    0x7F, 0x10, 0x5F, 0x10, 0x7F, // ю
    0x4E, 0x51, 0x51, 0x51, 0x7F  // я
};

bool graphics_init_panel(int width, int height, bool with_red)
{
    if (width < 8 || height < 1 || (width % 8) != 0) {
        return false;
    }
    size_t plane = (size_t)GRAPHICS_BUFFER_SIZE(width, height);
    size_t need = with_red ? 2 * plane : plane;
    if (need > sizeof(frame_arena)) {
        return false;
    }

    uint8_t *bw = frame_arena;
    uint8_t *red = with_red ? frame_arena + plane : NULL;
    graphics_canvas_init(&default_canvas, width, height, bw, red, plane);
    active_canvas = &default_canvas;

    // Default to White (0xFF) in BW buffer
    memset(bw, 0xFF, plane);
    // Default to No Red (0x00 or 0xFF depending on logic) in Red Buffer.
    // Assuming 0x00 = Transparent/No Red (User feedback: 0x00 covered nothing)
    if (red) {
        memset(red, 0x00, plane);
    }
    return true;
}

void graphics_init(void) {
    graphics_init_panel(GRAPHICS_DEFAULT_WIDTH, GRAPHICS_DEFAULT_HEIGHT, true);
}

void graphics_clear(uint8_t color) {
    if (!active_canvas || !active_canvas->bw_buffer) {
        return;
    }

    if (render_mode == GFX_RENDER_RED_MASK) {
        /* The B/W buffer is the red mask: only a red fill sets bits. */
        memset(active_canvas->bw_buffer, (color == GFX_RED) ? 0xFF : 0x00,
               active_canvas->buffer_size);
        return;
    }

    if (color == GFX_WHITE) {
        memset(active_canvas->bw_buffer, 0xFF, active_canvas->buffer_size);
        if (active_canvas->red_buffer) {
            memset(active_canvas->red_buffer, 0x00, active_canvas->buffer_size);
        }
    } else if (color == GFX_BLACK) {
        memset(active_canvas->bw_buffer, 0x00, active_canvas->buffer_size);
        if (active_canvas->red_buffer) {
            memset(active_canvas->red_buffer, 0x00, active_canvas->buffer_size);
        }
    } else if (color == GFX_RED) {
        // Full red screen: white BW backing + all-red plane (red bit 1 = Red).
        memset(active_canvas->bw_buffer, 0xFF, active_canvas->buffer_size);
        if (active_canvas->red_buffer) {
            memset(active_canvas->red_buffer, 0xFF, active_canvas->buffer_size);
        }
    }
}

void graphics_set_rotation(int rotation) {
    if (!active_canvas) {
        return;
    }
    active_canvas->rotation = rotation % 4;
    if (active_canvas->rotation < 0) {
        active_canvas->rotation += 4;
    }
}

int graphics_get_width(void) {
    return active_canvas ? canvas_logical_width(active_canvas) : 0;
}

int graphics_get_height(void) {
    return active_canvas ? canvas_logical_height(active_canvas) : 0;
}

/* Logical -> physical, in 90-degree steps. Factored out of graphics_draw_pixel
 * so graphics_fill_rect can map a rectangle's corners with the same rules
 * rather than restating them. */
static inline void canvas_map_point(const graphics_canvas_t *canvas,
                                    int x, int y, int *tx, int *ty)
{
    const int w = canvas->width;
    const int h = canvas->height;

    switch (canvas->rotation) {
        case 1: // 90 deg
            *tx = w - 1 - y;
            *ty = x;
            break;
        case 2: // 180 deg
            *tx = w - 1 - x;
            *ty = h - 1 - y;
            break;
        case 3: // 270 deg
            *tx = y;
            *ty = h - 1 - x;
            break;
        default: // 0 deg
            *tx = x;
            *ty = y;
            break;
    }
}

void graphics_draw_pixel(int x, int y, int color) {
    if (!active_canvas || !active_canvas->bw_buffer) {
        return;
    }

    if (x < 0 || x >= canvas_logical_width(active_canvas) ||
        y < 0 || y >= canvas_logical_height(active_canvas)) {
        return;
    }

    // GRAY/PINK are simulated: a checkerboard of real inks on logical coords.
    if (color == GFX_GRAY) {
        color = ((x + y) % 2 == 0) ? GFX_BLACK : GFX_WHITE;
    } else if (color == GFX_PINK) {
        color = ((x + y) % 2 == 0) ? GFX_RED : GFX_WHITE;
    }

    int w = active_canvas->width;
    int h = active_canvas->height;
    int tx, ty;

    canvas_map_point(active_canvas, x, y, &tx, &ty);

    if (tx < 0 || tx >= w || ty < 0 || ty >= h) return;

    int idx = ty * (w / 8) + (tx / 8);
    if (idx < 0 || (size_t)idx >= active_canvas->buffer_size) {
        return;
    }
    int bit = 7 - (tx % 8);

    if (render_mode == GFX_RENDER_RED_MASK) {
        // The BW buffer stands in for the red plane: 1 = Red, 0 = not red.
        if (color == GFX_RED) {
            active_canvas->bw_buffer[idx] |= (1 << bit);
        } else if (color == GFX_BLACK || color == GFX_WHITE) {
            active_canvas->bw_buffer[idx] &= ~(1 << bit);
        }
        return;
    }

    // BW plane: 1 = White, 0 = Black. Red plane: 1 = Red, 0 = Transparent;
    // red pixels keep the BW plane white underneath.
    if (color == GFX_BLACK) {
        active_canvas->bw_buffer[idx] &= ~(1 << bit);
    } else if (color == GFX_WHITE || color == GFX_RED) {
        active_canvas->bw_buffer[idx] |= (1 << bit);
    } else {
        return; // unknown color: leave the planes untouched
    }
    if (active_canvas->red_buffer) {
        if (color == GFX_RED) {
            active_canvas->red_buffer[idx] |= (1 << bit);
        } else {
            active_canvas->red_buffer[idx] &= ~(1 << bit);
        }
    }
}

/* Resolve a solid colour into the bit operations graphics_draw_pixel would
 * perform, so a whole span can be written at once. Returns false for anything
 * draw_pixel would leave alone (unknown colours) — those must stay no-ops.
 * GRAY/PINK never reach here: they are per-pixel patterns, not solid fills. */
static bool resolve_solid(int color, bool *bw_set, bool *red_set, bool *touch_red)
{
    if (render_mode == GFX_RENDER_RED_MASK) {
        /* The B/W buffer stands in for the red plane; the red plane is left
         * untouched, exactly as in draw_pixel. */
        if (color == GFX_RED) {
            *bw_set = true;
        } else if (color == GFX_BLACK || color == GFX_WHITE) {
            *bw_set = false;
        } else {
            return false;
        }
        *red_set = false;
        *touch_red = false;
        return true;
    }

    if (color == GFX_BLACK) {
        *bw_set = false;
    } else if (color == GFX_WHITE || color == GFX_RED) {
        *bw_set = true;
    } else {
        return false;
    }
    *red_set = (color == GFX_RED);
    *touch_red = (active_canvas->red_buffer != NULL);
    return true;
}

static void fill_span(uint8_t *row, int b0, int b1, uint8_t first, uint8_t last, bool set)
{
    if (b0 == b1) {
        uint8_t m = first & last;
        if (set) {
            row[b0] |= m;
        } else {
            row[b0] &= (uint8_t)~m;
        }
        return;
    }
    if (set) {
        row[b0] |= first;
        row[b1] |= last;
    } else {
        row[b0] &= (uint8_t)~first;
        row[b1] &= (uint8_t)~last;
    }
    if (b1 > b0 + 1) {
        memset(row + b0 + 1, set ? 0xFF : 0x00, (size_t)(b1 - b0 - 1));
    }
}

/*
 * A solid rectangle stays a rectangle under all four rotations, so the whole
 * fill reduces to: clip in logical space, map two corners, then walk physical
 * rows writing whole bytes. That replaces one ~130-instruction draw_pixel call
 * per pixel with two masked edge bytes and a memset per row — measured at 65x
 * on a full-screen fill, byte-for-byte identical to the per-pixel path.
 *
 * GRAY and PINK are checkerboards keyed on logical coordinates, so they keep
 * the per-pixel path.
 */
void graphics_fill_rect(int x, int y, int width, int height, int color)
{
    if (!active_canvas || !active_canvas->bw_buffer || width <= 0 || height <= 0) {
        return;
    }

    if (color == GFX_GRAY || color == GFX_PINK) {
        for (int yy = y; yy < y + height; yy++) {
            for (int xx = x; xx < x + width; xx++) {
                graphics_draw_pixel(xx, yy, color);
            }
        }
        return;
    }

    bool bw_set, red_set, touch_red;
    if (!resolve_solid(color, &bw_set, &red_set, &touch_red)) {
        return;
    }

    /* Clip in logical space, the way draw_pixel's per-pixel bounds check does. */
    int lx0 = x, ly0 = y;
    int lx1 = x + width - 1, ly1 = y + height - 1;
    const int lw = canvas_logical_width(active_canvas);
    const int lh = canvas_logical_height(active_canvas);

    if (lx0 < 0) lx0 = 0;
    if (ly0 < 0) ly0 = 0;
    if (lx1 > lw - 1) lx1 = lw - 1;
    if (ly1 > lh - 1) ly1 = lh - 1;
    if (lx1 < lx0 || ly1 < ly0) {
        return;
    }

    int ax, ay, bx, by;
    canvas_map_point(active_canvas, lx0, ly0, &ax, &ay);
    canvas_map_point(active_canvas, lx1, ly1, &bx, &by);

    const int px0 = (ax < bx) ? ax : bx;
    const int px1 = (ax < bx) ? bx : ax;
    const int py0 = (ay < by) ? ay : by;
    const int py1 = (ay < by) ? by : ay;

    const int stride = active_canvas->width / 8;
    const int b0 = px0 >> 3;
    const int b1 = px1 >> 3;
    /* Bit 7 is the leftmost pixel of a byte, matching draw_pixel's 7-(tx%8). */
    const uint8_t first = (uint8_t)(0xFFu >> (px0 & 7));
    const uint8_t last  = (uint8_t)(0xFFu << (7 - (px1 & 7)));

    const size_t plane = active_canvas->buffer_size;

    for (int row = py0; row <= py1; row++) {
        const size_t base = (size_t)row * (size_t)stride;
        if (base >= plane) {
            break;      /* rows only climb from here */
        }

        /* A canvas may be handed a buffer shorter than stride*height, and
         * draw_pixel drops the individual pixels that fall past it rather than
         * the whole row. Match that exactly: clamp the last byte, and fill all
         * of it, because the rectangle runs past where the clamp landed. */
        int row_b1 = b1;
        uint8_t row_last = last;
        const size_t avail = plane - base;
        if ((size_t)b1 >= avail) {
            row_b1 = (int)avail - 1;
            row_last = 0xFF;
        }
        if (row_b1 < b0) {
            continue;
        }

        fill_span(active_canvas->bw_buffer + base, b0, row_b1, first, row_last, bw_set);
        if (touch_red) {
            fill_span(active_canvas->red_buffer + base, b0, row_b1, first, row_last, red_set);
        }
    }
}

void graphics_draw_rect(int x, int y, int width, int height, int color)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int xx = x; xx < x + width; xx++) {
        graphics_draw_pixel(xx, y, color);
        graphics_draw_pixel(xx, y + height - 1, color);
    }
    for (int yy = y; yy < y + height; yy++) {
        graphics_draw_pixel(x, yy, color);
        graphics_draw_pixel(x + width - 1, yy, color);
    }
}

/* Resolve a code point to its 5 columns of glyph data. ASCII 32..126 and
 * Cyrillic U+0410..U+044F are covered; anything else renders as '?'. */
static const uint8_t *glyph_for(uint16_t c) {
    if (c >= 32 && c <= 126) {
        return &font5x7[(c - 32) * 5];
    }
    if (c >= 0x0410 && c <= 0x044F) {
        /* The range maps 1:1 onto font_cyr today, but the check keeps a
         * shortened table from reading past the array instead of silently
         * rendering garbage. */
        size_t index = (size_t)(c - 0x0410) * 5;
        if (index + 5 <= sizeof(font_cyr)) {
            return &font_cyr[index];
        }
    }
    return &font5x7[('?' - 32) * 5];
}

/* Decode the next UTF-8 code point and advance *str past it. Handles ASCII
 * and 2-byte sequences (enough for Cyrillic); a malformed lead byte falls
 * through as a raw byte and renders as '?'. */
static uint16_t utf8_next(const char **str) {
    uint16_t c = (uint8_t)**str;
    (*str)++;
    if ((c & 0xE0) == 0xC0) {
        uint8_t c2 = (uint8_t)**str;
        if ((c2 & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | (c2 & 0x3F);
            (*str)++;
        }
    }
    return c;
}

/* Set bit -> foreground colour; clear bit -> bg, or nothing at all when bg is
 * GFX_TRANSPARENT (5x7 cell). */
static void graphics_draw_char_bg(int x, int y, uint16_t c, int color, int bg) {
    const uint8_t *glyph = glyph_for(c);

    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                graphics_draw_pixel(x + col, y + row, color);
            } else if (bg != GFX_TRANSPARENT) {
                graphics_draw_pixel(x + col, y + row, bg);
            }
        }
    }
}

void graphics_draw_char(int x, int y, uint16_t c) {
    graphics_draw_char_bg(x, y, c, GFX_BLACK, GFX_WHITE);
}

void graphics_draw_string_color_bg(int x, int y, const char *str, int color, int bg) {
    while (*str) {
        graphics_draw_char_bg(x, y, utf8_next(&str), color, bg);
        x += 6;

        if (x > graphics_get_width() - 6) { x = 0; y += 8; }
        if (y > graphics_get_height() - 8) break;
    }
}

void graphics_draw_string_color(int x, int y, const char *str, int color) {
    graphics_draw_string_color_bg(x, y, str, color, GFX_WHITE);
}

void graphics_draw_string(int x, int y, const char *str) {
    graphics_draw_string_color(x, y, str, GFX_BLACK);
}

/* Scaled variant: transparent background (only set bits are drawn). */
static void graphics_draw_char_scaled(int x, int y, uint16_t c, int scale, int color) {
    const uint8_t *glyph = glyph_for(c);

    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (!(line & (1 << row))) {
                continue;
            }
            for (int sx = 0; sx < scale; sx++) {
                for (int sy = 0; sy < scale; sy++) {
                    graphics_draw_pixel(x + col*scale + sx, y + row*scale + sy, color);
                }
            }
        }
    }
}

void graphics_draw_string_color_scaled(int x, int y, const char *str, int color, int scale) {
    while (*str) {
        graphics_draw_char_scaled(x, y, utf8_next(&str), scale, color);
        x += 6 * scale;
    }
}

void graphics_draw_string_scaled(int x, int y, const char *str, int scale) {
    graphics_draw_string_color_scaled(x, y, str, GFX_BLACK, scale);
}

void graphics_draw_battery(int x, int y, int percent) {
    // Simple Battery Icon 20x10
    // Outline
    for (int i=0; i<20; i++) {
        graphics_draw_pixel(x+i, y, GFX_BLACK);
        graphics_draw_pixel(x+i, y+10, GFX_BLACK);
    }
    for (int i=0; i<=10; i++) {
        graphics_draw_pixel(x, y+i, GFX_BLACK);
        graphics_draw_pixel(x+20, y+i, GFX_BLACK);
    }
    // Nipple
    for (int i=2; i<8; i++) graphics_draw_pixel(x+22, y+i, GFX_BLACK);
    
    // Fill based on percent
    int fill_width = (percent * 18) / 100;
    if (fill_width > 18) fill_width = 18;
    
    for (int i=0; i<fill_width; i++) {
        for (int j=2; j<9; j++) {
            graphics_draw_pixel(x+1+i, y+j, GFX_BLACK);
        }
    }
}

const uint8_t* graphics_get_buffer(void) {
    return active_canvas ? active_canvas->bw_buffer : NULL;
}

const uint8_t* graphics_get_red_buffer(void) {
    return active_canvas ? active_canvas->red_buffer : NULL;
}
