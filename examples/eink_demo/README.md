# eink_demo — minimal SSD1675A example

A small Zephyr application that drives a 128x296 B/W/R e-paper panel using
[`lib/eink`](../../lib/eink) and [`lib/gfx`](../../lib/gfx). It builds against
the library in place — nothing is copied here, so the example never drifts from
the driver the main firmware uses.

It demonstrates three things:

1. **The raw path** — `display_square_without_graphics_library()` builds the two
   RAM planes byte by byte and pushes them out, no drawing library involved.
2. **The normal path** — draw with `lib/gfx` (text, rectangles, dithering),
   then flush with one full refresh.
3. **Waveform tuning** — patch the working LUT through the public byte editor
   (`ssd1675a_set_lut_byte`) to over-drive the red phase, and optionally abort
   the refresh part-way so the red freezes at maximum saturation.

## Screens

Pick one with `DEMO_SCREEN_MANDELBROT` at the top of `src/main.c`:

- `0` (default) — a neofetch-style system-info card with dithered red gradients
  and a QR code.
- `1` — a Mandelbrot fractal quantised to black / red / white
  (`src/mandelbrot.c`, ~40 lines).

`COVER_USE_RED_CUTOFF` controls the mid-refresh abort. It looks better in a
photo but leaves the panel DC-unbalanced, so it ghosts until the next normal
full refresh. Set it to `0` for a properly settled image.

## Wiring

Defaults match the rest of this repo (nRF52832, GPIO port 0):

| Signal | Pin |
| ------ | --- |
| BUSY   | 6   |
| RST    | 7   |
| CS     | 8   |
| CLK    | 11  |
| MOSI   | 12  |
| VCC    | 19  |

They live in [`lib/eink/port/ssd1675a_port_zephyr_nrf.c`](../../lib/eink/port/ssd1675a_port_zephyr_nrf.c)
as `#ifndef` defaults — override `SSD1675A_PIN_*` from the build system rather
than editing the port.

## Building

```sh
make build      # nrf52dk/nrf52832 by default
make flash
make BOARD=nrf54l15dk/nrf54l15/cpuapp build
```

Or directly with west, from the repository root:

```sh
west build -b nrf52dk/nrf52832 examples/eink_demo
```
