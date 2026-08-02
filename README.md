# nRF52 e-paper tag

**EN** · [RU](README.ru.md)

Firmware for a repurposed 2.9" BWR electronic shelf label — an nRF52832 driving
an SSD1675A e-paper panel over BLE, with no vendor documentation to work from.

It started on Avito, the Russian classifieds site. First five tags at 250 ₽
each, half of them cracked or drowned in electrolyte. Then seventeen more from
the same seller, of which two survived. Then an entire box of them, filled to
the brim, for 500 ₽ — at a time when one comparable e-paper panel cost 700 ₽ on
AliExpress.

Inside each one: an nRF52832 and an e-paper panel. No SDK, no datasheet, no
pinout, and a manufacturer who never answered the email. Everything in this
repository — the pinout, the waveform tables, the refresh modes, the streaming
path — came out of experiments on a live panel and is written up in
[`docs/`](docs/).

How the tags were cracked open, in Russian:
[Как я купил кота в мешке: реверс-инжиниринг электронных ценников](https://habr.com/ru/articles/1044406/).

> **Once a tag runs this firmware, you never need a toolchain again:**
> <https://pwa.price-tag.sorewa.ru/> is a live web app that drives it over Web
> Bluetooth — images, animation, the waveform editor, OTA updates. Chrome or
> Edge on desktop or Android.
>
> The catch is the first flash: a stock tag ships with the vendor's firmware and
> advertises nothing this app can talk to, so it has to be programmed once over
> SWD ([see below](#building)). After that, everything else is wireless.

## What it does

- Renders clock / status / screensaver screens on a 128×296 black-white-red panel
- Streams animation and video to the panel over BLE at ~110 ms per frame, after
  a stock full refresh took ~9 s
- Takes commands over the Nordic UART Service, from a CLI, a desktop GUI or a
  [live web app](https://pwa.price-tag.sorewa.ru/)
- Updates itself over the air (MCUboot, dual-variant signing) from that same
  web app
- Relays commands tag-to-tag over a connectionless flood mesh, so one phone
  connection reaches a whole fleet

## Highlights

- [`lib/`](lib/) — the display driver and drawing code as a **portable,
  dependency-free C99 library**. No Zephyr, no nRF SDK: six functions separate
  it from the platform, with reference ports for Zephyr, Arduino and STM32 HAL.
- [`docs/eink-lut-reference.md`](docs/eink-lut-reference.md) — the byte-level
  format of the SSD1675A waveform table, decoded experimentally: LUT row
  semantics, the RP repeat rule, the timing formula, and the DC-balance
  arithmetic that stops the panel from ghosting.
- [`docs/display-optimization.md`](docs/display-optimization.md) — how a
  762 ms partial refresh became 112 ms, including the two dead ends and the
  burn-in that came with them.

---

## Repository layout

```text
lib/              portable library — see lib/README.md
  eink/           SSD1675A driver, waveform presets, platform ports
  gfx/            1-bpp canvas, text, dithering, Game of Life
src/              this firmware (Zephyr / nRF Connect SDK)
  app/            display manager, commands, mesh, auth, persistence, battery
  ble/            NUS service, advertising, identity
examples/
  eink_demo/      minimal app built on lib/ alone
lut_tester_host/  host tooling: LUT editor GUI, video streamer, DFU, PWA
docs/             reverse-engineering write-ups
scripts/          build and flashing helpers
boards/           board overlays
```

## Hardware

| Part  | Value                                                              |
| ----- | ------------------------------------------------------------------ |
| MCU   | nRF52832 (512 KB flash, 64 KB RAM)                                 |
| Panel | SSD1675A-class, 128×296, black/white/red                           |
| Bus   | bit-banged 9-bit SPI (the panel puts the D/C bit inside the frame) |
| Board | `nrf52dk/nrf52832` target                                          |

Default pin assignment (override `SSD1675A_PIN_*` from the build system):

| Signal | Pin |
| ------ | --- |
| BUSY   | 6   |
| RST    | 7   |
| CS     | 8   |
| CLK    | 11  |
| MOSI   | 12  |
| VCC    | 19  |

## Building

Requires nRF Connect SDK v3.0.1. Point the Makefile at your install if it lives
somewhere else:

```sh
make                                   # build + package an OTA image
make NCS_DIR=/opt/nordic/ncs/v3.0.1 BOARD=nrf52dk/nrf52832
make flash                             # west flash
make flash-retry                       # retry until it verifies (flaky SWD)
make clean
```

Two OTA variants are built from one source tree — see the variant matrix in the
[`Makefile`](Makefile):

```sh
make                # legacy batch: current layout, default signing key
make VARIANT=v2     # v2 batch: own signing key, factory_data partition
make release        # both, merged into one manifest
```

An OTA size guard fails the build before publishing an image MCUboot would
accept but silently refuse to swap.

### The first flash

A stock tag has to be programmed once over SWD — there is no wireless way in
until this firmware is on it. The nRF52832 is a Cortex-M4, so any CMSIS-DAP
probe works; a WCH-LinkE in CMSIS-DAP mode or a genuine ST-Link/J-Link are all
fine, and none of them needs Nordic's own programmer. Solder to the SWDIO/SWCLK
pads (they differ between board revisions — ring them out).

```sh
make flash            # west flash, for probes west knows about
make flash-openocd    # openocd CLI, spawns its own session
make flash-gdb        # a running OpenOCD on telnet localhost:4444
make flash-retry      # keep retrying until it verifies — for flaky SWD contact
```

`make flash-retry` exists because pogo-pin contact on these boards is
unreliable; it re-flashes until verification passes. After this, every further
update goes over BLE.

## Talking to the tag

Commands arrive over the Nordic UART Service as text lines (`MODE:3`,
`NUKE:5`, `TIME=…`) or as binary opcode frames. Both dispatch through one
registry in [`src/app/cmd_opcodes.h`](src/app/cmd_opcodes.h), so a command is
implemented exactly once. `HELP` lists everything the running firmware knows.

### From a browser — no install

[`lut_tester_host/web/`](lut_tester_host/web/) is deployed and open to anyone at
**<https://pwa.price-tag.sorewa.ru/>**. It is the same app that is in this
repository, with everything it can do: images with dithering, text, animation
and video streaming, the 70-byte waveform editor with its live plot and
DC-balance bars, screensaver and display modes, time, battery and stats, and
OTA updates at [`/dfu.html`](https://pwa.price-tag.sorewa.ru/dfu.html).

It talks to a tag over Web Bluetooth straight from the browser — nothing to
build or install on the host side. **It only finds tags that already run this
firmware**, so a stock tag needs one wired flash over SWD first; the stock
vendor firmware advertises nothing this app can connect to. It is a PWA, so it
installs and keeps working offline once loaded. The interface is in Russian.
Web Bluetooth means Chrome, Edge or Opera on desktop and Android; Safari, iOS
and Firefox do not implement it.

It is a static bundle, so it runs from any host you point it at — `make deploy`
publishes it.

### From a terminal

```sh
pip install -r lut_tester_host/requirements.txt
python3 lut_tester_host/lut_tester.py          # LUT editor + device control GUI
python3 lut_tester_host/vstream_player.py …    # stream video to the panel
python3 lut_tester_host/dfu_upload.py …        # OTA from the command line
```

## Reusing the display code

`lib/` is written to be lifted out of this repository. It has no dependency on
Zephyr, on the nRF SDK, or on anything else here — six functions
(`ssd1675a_port.h`) stand between the driver and the hardware, and the
`lib/eink/port/` directory has working implementations for Zephyr, Arduino and
STM32 HAL.

See [`lib/README.md`](lib/README.md) for the port contract and copy-paste
examples.

## License

MIT for the code written for this project — see [`LICENSE`](LICENSE).

This firmware started from the Nordic `peripheral_uart` sample, and the files
that still carry a Nordic copyright header (`Kconfig`, `prj.conf`,
`CMakeLists.txt`, parts of `src/ble/`) remain under
`LicenseRef-Nordic-5-Clause` as marked. `lib/` is original work and is MIT only.
