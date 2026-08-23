#include "test.h"
#include "ssd1675a.h"

/* ── Fake port: records every 9-bit frame the driver emits ─────────────── */

typedef struct {
    uint8_t byte;
    bool is_data;
} frame_t;

#define MAX_FRAMES 32768
static frame_t frames[MAX_FRAMES];
static size_t frame_count;

static bool fake_init_ok = true;
static long busy_reads;         /* port_busy() returns true this many times */
static uint32_t delay_total_ms;
static int reset_asserts;
static int power_on_calls;

bool ssd1675a_port_init(void) { return fake_init_ok; }

void ssd1675a_port_write9(uint8_t byte, bool is_data)
{
    if (frame_count < MAX_FRAMES) {
        frames[frame_count].byte = byte;
        frames[frame_count].is_data = is_data;
    }
    frame_count++;
}

void ssd1675a_port_reset(bool asserted)
{
    if (asserted) {
        reset_asserts++;
    }
}

void ssd1675a_port_power(bool on)
{
    if (on) {
        power_on_calls++;
    }
}

bool ssd1675a_port_busy(void)
{
    if (busy_reads > 0) {
        busy_reads--;
        return true;
    }
    return false;
}

void ssd1675a_port_delay_ms(uint32_t ms) { delay_total_ms += ms; }

/* The port contract's optional read-back path. A port that cannot read fills
 * the buffer with 0xFF, which the driver reads as "no read path" rather than
 * as data — that is exactly the behaviour this fake wants, so the probe tests
 * exercise the no-read-path branch. */
void ssd1675a_port_read(uint8_t cmd, uint8_t *buf, int n)
{
    (void)cmd;
    for (int i = 0; i < n; i++) {
        buf[i] = 0xFF;
    }
}

static void capture_reset(void)
{
    frame_count = 0;
    delay_total_ms = 0;
}

/* Find command `cmd` at frame index >= from; return its index or -1. */
static long find_cmd(uint8_t cmd, size_t from)
{
    for (size_t i = from; i < frame_count && i < MAX_FRAMES; i++) {
        if (!frames[i].is_data && frames[i].byte == cmd) {
            return (long)i;
        }
    }
    return -1;
}

/* Check that the `n` frames after index `at` are data bytes matching `expect`
 * (or all `fill` when expect is NULL). */
static int data_after(long at, const uint8_t *expect, uint8_t fill, size_t n)
{
    if (at < 0 || (size_t)(at + 1 + n) > frame_count) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        frame_t *f = &frames[at + 1 + i];
        uint8_t want = expect ? expect[i] : fill;
        if (!f->is_data || f->byte != want) {
            return 0;
        }
    }
    return 1;
}

/* ── Tests (order matters: driver state is process-global) ─────────────── */

static void test_dead_port_makes_noops(void)
{
    fake_init_ok = false;
    capture_reset();

    T_ASSERT(!ssd1675a_init());
    T_ASSERT_EQ(frame_count, 0);

    ssd1675a_update_display();
    ssd1675a_update_partial();
    ssd1675a_display_buffer_fast(NULL);
    ssd1675a_sleep();
    ssd1675a_wait_busy();
    T_ASSERT_EQ(frame_count, 0);
}

static void test_init_sequence(void)
{
    fake_init_ok = true;
    capture_reset();

    T_ASSERT(ssd1675a_init());
    T_ASSERT(power_on_calls > 0);
    T_ASSERT(reset_asserts > 0);

    T_ASSERT(find_cmd(0x12, 0) >= 0);                    /* SW reset */

    static const uint8_t drv_out[] = { 0x27, 0x01, 0x00 }; /* 295 gates */
    T_ASSERT(data_after(find_cmd(0x01, 0), drv_out, 0, sizeof(drv_out)));

    static const uint8_t ram_x[] = { 0x00, 0x0F };
    T_ASSERT(data_after(find_cmd(0x44, 0), ram_x, 0, sizeof(ram_x)));

    static const uint8_t ram_y[] = { 0x00, 0x00, 0x27, 0x01 };
    T_ASSERT(data_after(find_cmd(0x45, 0), ram_y, 0, sizeof(ram_y)));

    static const uint8_t analog[] = { 0x54 };
    T_ASSERT(data_after(find_cmd(0x74, 0), analog, 0, 1));

    static const uint8_t vcom[] = { SSD1675A_VCOM_DEFAULT };
    T_ASSERT(data_after(find_cmd(0x2C, 0), vcom, 0, 1));

    static const uint8_t source[] = { 0x41, 0xA8, 0x32 };
    T_ASSERT(data_after(find_cmd(0x04, 0), source, 0, sizeof(source)));

    /* the working LUT is uploaded at init and starts with the default */
    long lut_at = find_cmd(0x32, 0);
    T_ASSERT(lut_at >= 0);
    T_ASSERT((size_t)lut_at + 1 + EINK_LUT_SIZE <= frame_count);
    T_ASSERT_EQ(frames[lut_at + 1].byte, 0x22); /* default LUT0 first byte */
}

static void test_lut_byte_editing(void)
{
    T_ASSERT_EQ(ssd1675a_get_lut_byte(0), 0x22);

    ssd1675a_set_lut_byte(0, 0x99);
    T_ASSERT_EQ(ssd1675a_get_lut_byte(0), 0x99);

    /* out-of-range indices are ignored / read as 0 */
    ssd1675a_set_lut_byte(-1, 0xAB);
    ssd1675a_set_lut_byte(EINK_LUT_SIZE, 0xAB);
    T_ASSERT_EQ(ssd1675a_get_lut_byte(-1), 0);
    T_ASSERT_EQ(ssd1675a_get_lut_byte(EINK_LUT_SIZE), 0);

    ssd1675a_reset_lut();
    T_ASSERT_EQ(ssd1675a_get_lut_byte(0), 0x22);
    T_ASSERT(!eink_lut_get_use_custom());
}

static void test_display_buffer(void)
{
    static uint8_t bw[SSD1675A_RAM_BYTES];
    static uint8_t red[SSD1675A_RAM_BYTES];

    for (int i = 0; i < SSD1675A_RAM_BYTES; i++) {
        bw[i] = (uint8_t)i;
        red[i] = (uint8_t)(0xFF - i);
    }

    capture_reset();
    ssd1675a_display_buffer(bw, red);

    long bw_at = find_cmd(0x24, 0);
    T_ASSERT(data_after(bw_at, bw, 0, SSD1675A_RAM_BYTES));

    long red_at = find_cmd(0x26, 0);
    T_ASSERT(data_after(red_at, red, 0, SSD1675A_RAM_BYTES));

    /* the RAM pointer is rewound before each plane */
    T_ASSERT(find_cmd(0x4E, 0) >= 0 && find_cmd(0x4E, 0) < bw_at);

    /* NULL red buffer clears the plane with zeros */
    capture_reset();
    ssd1675a_display_buffer(bw, NULL);
    T_ASSERT(data_after(find_cmd(0x26, 0), NULL, 0x00, SSD1675A_RAM_BYTES));

    /* fast path writes only the BW plane */
    capture_reset();
    ssd1675a_display_buffer_fast(bw);
    T_ASSERT(find_cmd(0x24, 0) >= 0);
    T_ASSERT(find_cmd(0x26, 0) < 0);

    capture_reset();
    ssd1675a_clear_red_ram();
    T_ASSERT(find_cmd(0x24, 0) < 0);
    T_ASSERT(data_after(find_cmd(0x26, 0), NULL, 0x00, SSD1675A_RAM_BYTES));
}

static void test_update_sequences(void)
{
    /* full update: working LUT upload, then 0x22 / 0xC7 / 0x20 */
    capture_reset();
    ssd1675a_update_display();
    long lut_at = find_cmd(0x32, 0);
    T_ASSERT(lut_at >= 0);
    long upd = find_cmd(0x22, lut_at);
    static const uint8_t c7[] = { 0xC7 };
    T_ASSERT(data_after(upd, c7, 0, 1));
    T_ASSERT(find_cmd(0x20, upd) > upd);

    /* partial update with TURBO uploads the turbo table */
    ssd1675a_set_partial_mode(SSD1675A_PARTIAL_MODE_TURBO);
    capture_reset();
    ssd1675a_update_partial();
    lut_at = find_cmd(0x32, 0);
    T_ASSERT(lut_at >= 0);
    T_ASSERT_EQ(frames[lut_at + 1].byte, 0x55); /* lut_turbo signature */
    T_ASSERT(data_after(find_cmd(0x22, lut_at), c7, 0, 1));

    /* streaming: begin 0xC0, frame 0x04, end 0x03 */
    static const uint8_t c0[] = { 0xC0 }, mode1[] = { 0x04 }, off[] = { 0x03 };

    capture_reset();
    ssd1675a_begin_streaming();
    T_ASSERT(data_after(find_cmd(0x22, 0), c0, 0, 1));

    capture_reset();
    ssd1675a_update_frame_stream();
    T_ASSERT(data_after(find_cmd(0x22, 0), mode1, 0, 1));
    T_ASSERT(find_cmd(0x32, 0) < 0); /* no LUT re-upload per frame */

    capture_reset();
    ssd1675a_end_streaming();
    T_ASSERT(data_after(find_cmd(0x22, 0), off, 0, 1));

    /* red flush uploads the flush table, then restores the working one */
    capture_reset();
    ssd1675a_update_display_flush_red();
    long flush_at = find_cmd(0x32, 0);
    T_ASSERT(flush_at >= 0);
    T_ASSERT_EQ(frames[flush_at + 15].byte, 0xAA); /* LUT2 all-VSL */
    long restore_at = find_cmd(0x32, flush_at + 1);
    T_ASSERT(restore_at > flush_at);
    T_ASSERT_EQ(frames[restore_at + 1].byte, 0x22); /* back to working */
}

static void test_busy_timeout(void)
{
    busy_reads = 1000000; /* stuck panel: BUSY never clears */
    capture_reset();

    ssd1675a_wait_busy();
    T_ASSERT_EQ(delay_total_ms, SSD1675A_BUSY_TIMEOUT_MS);

    busy_reads = 0;
}

static void test_sleep_and_partial_reinit(void)
{
    capture_reset();
    ssd1675a_sleep();
    static const uint8_t deep[] = { 0x01 };
    T_ASSERT(data_after(find_cmd(0x10, 0), deep, 0, 1));

    /* after sleep, init_partial must hard-reset (SW reset present) */
    capture_reset();
    T_ASSERT(ssd1675a_init_partial());
    T_ASSERT(find_cmd(0x12, 0) >= 0);

    /* awake and idle: init_partial reconfigures without a reset */
    capture_reset();
    T_ASSERT(ssd1675a_init_partial());
    T_ASSERT(find_cmd(0x12, 0) < 0);
    T_ASSERT(find_cmd(0x2C, 0) >= 0); /* registers still rewritten */
}

static void test_vcom_override(void)
{
    ssd1675a_set_vcom_register(0x50);
    capture_reset();
    T_ASSERT(ssd1675a_init_partial());
    static const uint8_t vcom[] = { 0x50 };
    T_ASSERT(data_after(find_cmd(0x2C, 0), vcom, 0, 1));
    ssd1675a_set_vcom_register(SSD1675A_VCOM_DEFAULT);
}

int main(void)
{
    T_RUN(test_dead_port_makes_noops);
    T_RUN(test_init_sequence);
    T_RUN(test_lut_byte_editing);
    T_RUN(test_display_buffer);
    T_RUN(test_update_sequences);
    T_RUN(test_busy_timeout);
    T_RUN(test_sleep_and_partial_reinit);
    T_RUN(test_vcom_override);
    return t_report("ssd1675a");
}
