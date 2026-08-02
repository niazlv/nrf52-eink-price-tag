/*
 * SSD1675A port — STM32Cube HAL.
 *
 * Add lib/eink to your project's include paths and this file to the source
 * list. Assign the six pins in CubeMX (five push-pull outputs, BUSY as input)
 * and either keep the GPIO labels below or point the macros at the ones CubeMX
 * generated in main.h.
 *
 * The panel needs a 9-bit frame (D/C bit followed by 8 data bits). STM32 SPI
 * peripherals support 9-bit data on most families, so if you would rather use
 * SPI + DMA than bit-bang, configure SPI_DATASIZE_9BIT, pack the frame as
 * ((is_data << 8) | byte) and replace the body of ssd1675a_port_write9() with
 * a single HAL_SPI_Transmit(). Everything else stays as it is.
 */

#include "../ssd1675a.h"

#include "main.h"   /* CubeMX-generated: HAL headers + the *_GPIO_Port/*_Pin labels */

/* ── Wiring — point these at your CubeMX labels ─────────────────────────── */

#ifndef SSD1675A_BUSY_PORT
#define SSD1675A_BUSY_PORT EPD_BUSY_GPIO_Port
#define SSD1675A_BUSY_PIN  EPD_BUSY_Pin
#endif
#ifndef SSD1675A_RST_PORT
#define SSD1675A_RST_PORT EPD_RST_GPIO_Port
#define SSD1675A_RST_PIN  EPD_RST_Pin
#endif
#ifndef SSD1675A_CS_PORT
#define SSD1675A_CS_PORT EPD_CS_GPIO_Port
#define SSD1675A_CS_PIN  EPD_CS_Pin
#endif
#ifndef SSD1675A_CLK_PORT
#define SSD1675A_CLK_PORT EPD_CLK_GPIO_Port
#define SSD1675A_CLK_PIN  EPD_CLK_Pin
#endif
#ifndef SSD1675A_MOSI_PORT
#define SSD1675A_MOSI_PORT EPD_MOSI_GPIO_Port
#define SSD1675A_MOSI_PIN  EPD_MOSI_Pin
#endif

/* Panel supply switch, active low (high-side P-MOS). Leave SSD1675A_VCC_PORT
 * undefined if the panel is tied to a permanent rail — power control then
 * compiles away to a no-op. */
#if defined(EPD_VCC_Pin) && !defined(SSD1675A_VCC_PORT)
#define SSD1675A_VCC_PORT EPD_VCC_GPIO_Port
#define SSD1675A_VCC_PIN  EPD_VCC_Pin
#endif

static bool configured;

bool ssd1675a_port_init(void)
{
    /* CubeMX's MX_GPIO_Init() has already configured the pins; this only puts
     * the bus in its idle state. Move the HAL_GPIO_Init() calls here instead if
     * you would rather the driver own its pins. */
    if (configured) {
        return true;
    }

    HAL_GPIO_WritePin(SSD1675A_CS_PORT, SSD1675A_CS_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(SSD1675A_CLK_PORT, SSD1675A_CLK_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(SSD1675A_RST_PORT, SSD1675A_RST_PIN, GPIO_PIN_SET);

    configured = true;
    return true;
}

void ssd1675a_port_write9(uint8_t byte, bool is_data)
{
    if (!configured) {
        return;
    }

    HAL_GPIO_WritePin(SSD1675A_CS_PORT, SSD1675A_CS_PIN, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(SSD1675A_MOSI_PORT, SSD1675A_MOSI_PIN,
                      is_data ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(SSD1675A_CLK_PORT, SSD1675A_CLK_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(SSD1675A_CLK_PORT, SSD1675A_CLK_PIN, GPIO_PIN_RESET);

    for (uint8_t i = 0; i < 8; i++) {
        HAL_GPIO_WritePin(SSD1675A_MOSI_PORT, SSD1675A_MOSI_PIN,
                          (byte & 0x80) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        byte <<= 1;
        HAL_GPIO_WritePin(SSD1675A_CLK_PORT, SSD1675A_CLK_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(SSD1675A_CLK_PORT, SSD1675A_CLK_PIN, GPIO_PIN_RESET);
    }

    HAL_GPIO_WritePin(SSD1675A_CS_PORT, SSD1675A_CS_PIN, GPIO_PIN_SET);
}

void ssd1675a_port_reset(bool asserted)
{
    HAL_GPIO_WritePin(SSD1675A_RST_PORT, SSD1675A_RST_PIN,
                      asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void ssd1675a_port_power(bool on)
{
#ifdef SSD1675A_VCC_PORT
    HAL_GPIO_WritePin(SSD1675A_VCC_PORT, SSD1675A_VCC_PIN,
                      on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    (void)on;   /* panel wired to a permanent rail */
#endif
}

bool ssd1675a_port_busy(void)
{
    return HAL_GPIO_ReadPin(SSD1675A_BUSY_PORT, SSD1675A_BUSY_PIN) == GPIO_PIN_SET;
}

void ssd1675a_port_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}
