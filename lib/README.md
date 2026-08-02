# Portable e-paper library

Two independent, dependency-free C99 modules. Neither knows about Zephyr, nRF,
or this project; both are meant to be copied into whatever toolchain you use —
Arduino IDE, STM32CubeIDE, PlatformIO, a bare Makefile.

```
lib/
├── eink/                          SSD1675A e-paper controller
│   ├── ssd1675a.h/.c              driver — no OS, no vendor SDK, no malloc
│   ├── ssd1675a_config.h          panel geometry + register defaults
│   ├── ssd1675a_port.h            the six functions a platform must provide
│   ├── eink_lut.h/.c              waveform presets, refresh modes, virtual LUTs
│   └── port/
│       ├── ssd1675a_port_zephyr_nrf.c   Zephyr on nRF52 (used by this repo)
│       ├── ssd1675a_port_arduino.cpp    Arduino core template
│       └── ssd1675a_port_stm32_hal.c    STM32Cube HAL template
└── gfx/                           1-bpp drawing on top of any framebuffer
    ├── graphics.h/.c              canvas, rotation, shapes, text, battery icon
    ├── dither.h/.c                ordered Bayer 4x4 dithering (fake gray/pink)
    └── life.h/.c                  Game of Life, display-independent
```

`gfx` never calls `eink`, and `eink` never calls `gfx`. Take one, the other, or
both.

## Porting the display driver

Copy `lib/eink/` and implement six functions. That is the whole contract
(`ssd1675a_port.h`):

```c
bool ssd1675a_port_init(void);                          // configure pins, idempotent
void ssd1675a_port_write9(uint8_t byte, bool is_data);  // 9-bit frame: D/C + 8 bits
void ssd1675a_port_reset(bool asserted);                // RST line
void ssd1675a_port_power(bool on);                      // panel supply (may be empty)
bool ssd1675a_port_busy(void);                          // BUSY line
void ssd1675a_port_delay_ms(uint32_t ms);
```

Start from the closest file in `port/` — the Arduino and STM32 ones are
complete, commented templates, roughly 60 lines each. Pin numbers live in the
port file; the driver never sees a pin.

The panel wants a **9-bit** frame (a data/command bit followed by 8 data bits),
which is why the reference ports bit-bang instead of using a hardware SPI
peripheral. STM32 families that support `SPI_DATASIZE_9BIT` can use real SPI —
see the note at the top of `ssd1675a_port_stm32_hal.c`.

### Minimal use

```c
#include "ssd1675a.h"

static uint8_t bw[SSD1675A_RAM_BYTES];    // 4736 B for a 128x296 panel

ssd1675a_init();
memset(bw, 0xFF, sizeof bw);              // 1 = white
ssd1675a_display_buffer(bw, NULL);        // NULL = clear the red plane
ssd1675a_update_display();                // full B/W/R refresh, ~9 s
ssd1675a_power_off();
```

Fast monochrome updates use a partial waveform and skip the red plane:

```c
ssd1675a_init_partial();
ssd1675a_set_partial_mode(EINK_LUT_MODE_TURBO);
ssd1675a_display_buffer_fast(bw);
ssd1675a_update_partial();                // ~700 ms, dominated by HV recharge
```

For animation, hold the high-voltage rails on across frames — that recharge is
most of the cost, and streaming pays it once instead of per frame:

```c
ssd1675a_begin_streaming();               // ~600 ms, once
while (frames--) {
    ssd1675a_display_buffer_fast(bw);
    ssd1675a_update_frame_stream();       // ~110 ms in TURBO
}
ssd1675a_end_streaming();
```

### Configuring the panel

Everything in `ssd1675a_config.h` is an `#ifndef` default, so nothing needs
editing in-tree. Override from the command line, from your build system's
global config header, or by pointing `SSD1675A_CONFIG_FILE` at your own header:

```c
-DSSD1675A_WIDTH=104 -DSSD1675A_HEIGHT=212     // a different SSD16xx panel
-DSSD1675A_VCOM_DEFAULT=0x50                   // contrast vs. ghosting
-DSSD1675A_USE_CUSTOM_LUT=0                    // run the controller's OTP waveform
```

Register writes are derived from the geometry, so a differently sized panel
works without touching driver code. The scan-timing block
(`SSD1675A_DUMMY_LINE`, `SSD1675A_GATE_WIDTH`, the source voltages) is
panel-specific — copy those from your module's datasheet or vendor init code.

### Waveforms

`eink_lut.h` is the interesting part and is panel-independent: it owns the
70-byte waveform tables (5 LUTs x 7 phases + 7 phases x {TA TB TC TD RP}), the
refresh-mode presets, and runtime-programmable "virtual LUT" slots. It works on
any SSD16xx-family controller that takes a command-0x32 table, whether or not
you use this driver. Setting `SSD1675A_USE_CUSTOM_LUT=0` falls back to the
controller's OTP waveform and disables all of it.

See [`../docs/eink-lut-reference.md`](../docs/eink-lut-reference.md) for the
byte-level format and [`../docs/display-optimization.md`](../docs/display-optimization.md)
for how these tables and the streaming mode were derived (a full refresh went
from ~9 s to ~110 ms per frame).

## Using the graphics module

`gfx` is a 1-bpp canvas with an optional second (red) plane. It has no notion of
a display: you allocate the buffers, draw into them, and hand them to whatever
driver you have.

```c
#include "graphics.h"

graphics_canvas_t canvas;
uint8_t bw[GRAPHICS_BUFFER_SIZE(128, 296)];
uint8_t red[GRAPHICS_BUFFER_SIZE(128, 296)];

graphics_canvas_init(&canvas, 128, 296, bw, red, sizeof bw);
graphics_set_canvas(&canvas);
graphics_clear(GFX_WHITE);
graphics_draw_string(4, 8, "hello");
```

Pass `NULL` as `red_buffer` on a display without a red plane; `GFX_RED` and
`GFX_PINK` then degrade to black and gray.

`dither.c` renders gray and pink through an ordered Bayer 4x4 matrix on the
active canvas, so it works on any 1-bpp target.

## License

MIT, same as the rest of the repository — see [`../LICENSE`](../LICENSE).
