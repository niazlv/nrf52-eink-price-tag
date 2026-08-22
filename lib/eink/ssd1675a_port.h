#ifndef SSD1675A_PORT_H
#define SSD1675A_PORT_H

/*
 * Platform contract for the SSD1675A driver.
 *
 * ssd1675a.c is plain C99 with no OS or vendor dependency. Everything that
 * touches hardware goes through the seven functions below; porting the driver
 * to a new platform means implementing exactly these and nothing else (the
 * read-back one may be a stub — see its comment).
 *
 * Reference implementations live in port/:
 *   ssd1675a_port_zephyr_nrf.c  — Zephyr on nRF52 (used by this repo)
 *   ssd1675a_port_stm32_hal.c   — STM32Cube HAL
 *
 * Wiring is a port concern: pin numbers, whether the panel supply is switched
 * by a P-MOS, and whether the 9-bit frame is bit-banged or driven by a real SPI
 * peripheral are all decided inside the port file. The driver never sees a pin.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configure pins/bus. Idempotent: the driver calls it on every (re)init, and
 * an application may call it early to probe for the panel.
 *
 * @return true if the display bus is usable. When false, every ssd1675a_*
 *         entry point becomes a no-op instead of hanging on a busy-wait.
 */
bool ssd1675a_port_init(void);

/**
 * Write one 9-bit frame, MSB first: the command/data bit followed by 8 data
 * bits. This is the SSD1675A's 4-wire-SPI-without-DC variant; on hardware SPI
 * peripherals that only do 8 bits, bit-bang it or use a 9-bit-capable mode.
 *
 * Called once per command and once per framebuffer byte, so a full 128x296
 * plane is 4736 calls — this function's cost dominates refresh time. See the
 * nRF port for what the difference between an abstracted GPIO call and a raw
 * register store is worth in practice (~85 ms vs ~7 ms per plane).
 *
 * @param byte     the 8 payload bits.
 * @param is_data  false = command (D/C low), true = data (D/C high).
 */
void ssd1675a_port_write9(uint8_t byte, bool is_data);

/**
 * Drive the panel's RST line. @p asserted true means "hold in reset"; the
 * driver handles the timing around it, the port only maps the polarity
 * (RST is active low on every SSD1675A module seen so far).
 */
void ssd1675a_port_reset(bool asserted);

/**
 * Switch the panel supply rail. @p on true means "powered". Boards that wire
 * the panel straight to a permanent rail can leave this empty — the driver
 * only uses it to save idle current between refreshes.
 */
void ssd1675a_port_power(bool on);

/** @return true while the controller holds BUSY high (refresh in progress). */
bool ssd1675a_port_busy(void);

/** Block for at least @p ms milliseconds. */
void ssd1675a_port_delay_ms(uint32_t ms);

/**
 * Read-back path, used only by the identification/probe calls in ssd1675a.h.
 *
 * Issue @p cmd and clock @p n parameter bytes back into @p buf. On 3-wire SPI
 * that is: CS low, the 9-bit command frame, then for every byte a D/C=1 bit
 * driven by the host followed by 8 bits driven by the controller (it shifts
 * each one out on the falling clock edge), CS high at the end. The data line
 * is turned around in between, so the host pin must be able to go
 * high-impedance while the controller drives it.
 *
 * Optional: a port that cannot read may fill @p buf with 0xFF. The probe then
 * reports "no read path" instead of a wrong identification.
 */
void ssd1675a_port_read(uint8_t cmd, uint8_t *buf, int n);

#ifdef __cplusplus
}
#endif

#endif /* SSD1675A_PORT_H */
