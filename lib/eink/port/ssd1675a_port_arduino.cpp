/*
 * SSD1675A port — Arduino core (AVR, ESP32, RP2040, SAMD, …).
 *
 * Drop this file, ssd1675a.{c,h}, ssd1675a_port.h, ssd1675a_config.h and
 * eink_lut.{c,h} into your sketch folder (or a library folder) and wire the
 * six pins below. Nothing else is needed — the driver is plain C99 and the
 * `extern "C"` block makes it link against a C++ sketch.
 *
 * Sketch skeleton:
 *
 *     #include "ssd1675a.h"
 *     #include "graphics.h"          // optional, from ../../gfx
 *
 *     static uint8_t bw[SSD1675A_RAM_BYTES];
 *
 *     void setup() {
 *         ssd1675a_init();
 *         memset(bw, 0xFF, sizeof bw);          // 1 = white
 *         ssd1675a_display_buffer(bw, nullptr);
 *         ssd1675a_update_display();            // ~9 s
 *         ssd1675a_power_off();
 *     }
 *
 * Note the RAM cost: one plane of a 128x296 panel is 4736 bytes, so an
 * UNO-class AVR (2 KB SRAM) cannot hold a framebuffer — stream rows into RAM
 * with ssd1675a_port_write9() directly, or pick a bigger MCU.
 */

#include <Arduino.h>

extern "C" {
#include "../ssd1675a.h"
}

/* ── Wiring — edit to match your board ──────────────────────────────────── */

#ifndef SSD1675A_PIN_BUSY
#define SSD1675A_PIN_BUSY 5
#endif
#ifndef SSD1675A_PIN_RST
#define SSD1675A_PIN_RST 6
#endif
/** Panel supply switch, active low (high-side P-MOS). Set to -1 if the panel
 *  is tied to a permanent rail; power control then becomes a no-op. */
#ifndef SSD1675A_PIN_VCC
#define SSD1675A_PIN_VCC -1
#endif
#ifndef SSD1675A_PIN_CS
#define SSD1675A_PIN_CS 10
#endif
#ifndef SSD1675A_PIN_CLK
#define SSD1675A_PIN_CLK 13
#endif
#ifndef SSD1675A_PIN_MOSI
#define SSD1675A_PIN_MOSI 11
#endif

static bool configured;

extern "C" bool ssd1675a_port_init(void)
{
    if (configured) {
        return true;
    }

    pinMode(SSD1675A_PIN_BUSY, INPUT);
    pinMode(SSD1675A_PIN_RST, OUTPUT);
    pinMode(SSD1675A_PIN_CS, OUTPUT);
    pinMode(SSD1675A_PIN_CLK, OUTPUT);
    pinMode(SSD1675A_PIN_MOSI, OUTPUT);
    if (SSD1675A_PIN_VCC >= 0) {
        pinMode(SSD1675A_PIN_VCC, OUTPUT);
    }

    digitalWrite(SSD1675A_PIN_CS, HIGH);
    digitalWrite(SSD1675A_PIN_CLK, LOW);
    digitalWrite(SSD1675A_PIN_RST, HIGH);

    configured = true;
    return true;
}

/* Bit-banged because the panel expects a 9-bit frame (D/C bit + 8 data bits),
 * which most SPI peripherals cannot emit. digitalWrite() is slow — a full
 * 4736-byte plane costs on the order of 100 ms on an AVR. If refresh rate
 * matters, replace the two writes below with direct port manipulation
 * (PORTB |= _BV(n)) or your MCU's GPIO registers; the SSD1675A tolerates up to
 * 20 MHz, so the bus is never the limit. */
extern "C" void ssd1675a_port_write9(uint8_t byte, bool is_data)
{
    if (!configured) {
        return;
    }

    digitalWrite(SSD1675A_PIN_CS, LOW);

    digitalWrite(SSD1675A_PIN_MOSI, is_data ? HIGH : LOW);
    digitalWrite(SSD1675A_PIN_CLK, HIGH);
    digitalWrite(SSD1675A_PIN_CLK, LOW);

    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(SSD1675A_PIN_MOSI, (byte & 0x80) ? HIGH : LOW);
        byte <<= 1;
        digitalWrite(SSD1675A_PIN_CLK, HIGH);
        digitalWrite(SSD1675A_PIN_CLK, LOW);
    }

    digitalWrite(SSD1675A_PIN_CS, HIGH);
}

extern "C" void ssd1675a_port_reset(bool asserted)
{
    digitalWrite(SSD1675A_PIN_RST, asserted ? LOW : HIGH);
}

extern "C" void ssd1675a_port_power(bool on)
{
    if (SSD1675A_PIN_VCC < 0) {
        return;   /* panel wired to a permanent rail */
    }
    digitalWrite(SSD1675A_PIN_VCC, on ? LOW : HIGH);
}

extern "C" bool ssd1675a_port_busy(void)
{
    return digitalRead(SSD1675A_PIN_BUSY) == HIGH;
}

extern "C" void ssd1675a_port_delay_ms(uint32_t ms)
{
    delay(ms);
}

/* Read-back is only needed by the panel probe. Left as "no read path" (all
 * 0xFF) in this template; the nRF port shows the bit-banged sequence if you
 * want it (pinMode(MOSI, INPUT_PULLUP) between the D/C bit and the 8 bits). */
extern "C" void ssd1675a_port_read(uint8_t cmd, uint8_t *buf, int n)
{
    (void)cmd;
    for (int i = 0; i < n; i++) {
        buf[i] = 0xFF;
    }
}
