#include "test.h"
#include "life.h"

/* Mirror of life.c's LCG, used to pick seeds that skip random injection. */
static uint32_t lcg_next(uint32_t seed)
{
    return seed * 1664525u + 1013904223u;
}

/* True if `updates` consecutive life_update() calls with this starting seed
 * will all skip the 2% glider/block injection (each quiet update consumes
 * exactly one random draw). */
static int seed_is_quiet(uint32_t seed, int updates)
{
    for (int i = 0; i < updates; i++) {
        seed = lcg_next(seed);
        if (seed % 100 < 2) {
            return 0;
        }
    }
    return 1;
}

static void test_init(void)
{
    life_world_t world;

    life_init(&world, 42);
    T_ASSERT(world.initialized);

    int alive = 0;
    for (int y = 0; y < LIFE_MAX_DIM; y++) {
        for (int x = 0; x < LIFE_MAX_DIM; x++) {
            alive += life_get(&world, x, y) ? 1 : 0;
        }
    }
    /* ~37% fill (24000/65536); allow a generous band */
    int total = LIFE_MAX_DIM * LIFE_MAX_DIM;
    T_ASSERT_MSG(alive > total / 5 && alive < total / 2,
                 "alive = %d of %d", alive, total);

    life_init(NULL, 42); /* must not crash */
}

static void test_determinism(void)
{
    life_world_t a, b;

    life_init(&a, 1234);
    life_init(&b, 1234);
    for (int i = 0; i < 5; i++) {
        life_update(&a, 16, 16);
        life_update(&b, 16, 16);
    }
    /* compare fields, not the whole struct — its tail padding is undefined */
    T_ASSERT(memcmp(a.cur, b.cur, sizeof(a.cur)) == 0);
    T_ASSERT_EQ(a.seed, b.seed);
}

/* With one bit per cell the "cells hold only 0 or 1" invariant is structural.
 * What packing can actually get wrong is the padding: bits to the right of the
 * active width, and rows below the active height, must stay clear so a later
 * update with a bigger window cannot resurrect them. */
static void test_no_live_cells_outside_window(void)
{
    life_world_t world;
    const int w = 11, h = 9;   /* width deliberately not a multiple of 8 */

    life_init(&world, 7);      /* fills the whole 38x38 board */
    for (int i = 0; i < 5; i++) {
        life_update(&world, w, h);
    }

    for (int y = 0; y < LIFE_MAX_DIM; y++) {
        for (int x = 0; x < LIFE_MAX_DIM; x++) {
            if (x < w && y < h) {
                continue;
            }
            T_ASSERT_MSG(!life_get(&world, x, y),
                         "cell (%d,%d) outside the %dx%d window is alive",
                         x, y, w, h);
        }
    }

    /* Growing the window must not bring the old cells back. */
    life_update(&world, LIFE_MAX_DIM, LIFE_MAX_DIM);
    int alive_far = 0;
    for (int y = h + 2; y < LIFE_MAX_DIM; y++) {
        for (int x = w + 2; x < LIFE_MAX_DIM; x++) {
            alive_far += life_get(&world, x, y) ? 1 : 0;
        }
    }
    T_ASSERT_MSG(alive_far == 0, "%d stale cells came back", alive_far);
}

/* The window does not only grow. display_screens derives it from the canvas,
 * so a ROT: while the dynamic screensaver runs swaps 37x16 for 16x37 — and it
 * is the SHRINK that is dangerous: whatever the wider generation left in the
 * scratch plane rides back in on the next whole-row commit. Rotate both ways
 * and require the board to stay inside its window every time. */
static void test_window_shrink_leaves_nothing_behind(void)
{
    life_world_t world;
    const int wide_w = 37, wide_h = 16;
    const int tall_w = 16, tall_h = 37;

    life_init(&world, 12345);   /* fills the whole 38x38 board */

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 6; i++) {
            life_update(&world, wide_w, wide_h);
        }
        int outside = 0;
        for (int y = 0; y < LIFE_MAX_DIM; y++) {
            for (int x = 0; x < LIFE_MAX_DIM; x++) {
                if ((x >= wide_w || y >= wide_h) && life_get(&world, x, y)) {
                    outside++;
                }
            }
        }
        T_ASSERT_MSG(outside == 0,
                     "round %d: %d cells outside the %dx%d window",
                     round, outside, wide_w, wide_h);

        for (int i = 0; i < 6; i++) {
            life_update(&world, tall_w, tall_h);   /* narrower: the shrink */
        }
        outside = 0;
        for (int y = 0; y < LIFE_MAX_DIM; y++) {
            for (int x = 0; x < LIFE_MAX_DIM; x++) {
                if ((x >= tall_w || y >= tall_h) && life_get(&world, x, y)) {
                    outside++;
                }
            }
        }
        T_ASSERT_MSG(outside == 0,
                     "round %d: %d cells outside the %dx%d window after shrink",
                     round, outside, tall_w, tall_h);
        if (t_failures) {
            return;
        }
    }
}

static void test_accessors_round_trip(void)
{
    life_world_t world;

    life_init(&world, 5);
    life_clear(&world);
    for (int y = 0; y < LIFE_MAX_DIM; y++) {
        for (int x = 0; x < LIFE_MAX_DIM; x++) {
            T_ASSERT(!life_get(&world, x, y));
        }
    }

    /* Every bit position within a byte, and the last cell of a row. */
    const int xs[] = {0, 1, 7, 8, 15, 16, LIFE_MAX_DIM - 1};
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        life_set(&world, xs[i], 3, true);
    }
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        T_ASSERT_MSG(life_get(&world, xs[i], 3), "x=%d did not stick", xs[i]);
    }
    /* Setting one cell must not disturb its neighbours in the same byte. */
    T_ASSERT(!life_get(&world, 2, 3));
    T_ASSERT(!life_get(&world, 9, 3));
    T_ASSERT(!life_get(&world, 0, 4));

    life_set(&world, 8, 3, false);
    T_ASSERT(!life_get(&world, 8, 3));
    T_ASSERT(life_get(&world, 7, 3));

    /* Out of range: dropped on write, 0 on read, no crash. */
    life_set(&world, -1, 0, true);
    life_set(&world, 0, -1, true);
    life_set(&world, LIFE_MAX_DIM, 0, true);
    life_set(NULL, 0, 0, true);
    T_ASSERT(!life_get(&world, -1, 0));
    T_ASSERT(!life_get(&world, LIFE_MAX_DIM, 0));
    T_ASSERT(!life_get(NULL, 0, 0));
}

static void test_blinker_oscillates(void)
{
    life_world_t world;
    uint32_t seed = 1;

    while (!seed_is_quiet(seed, 2)) {
        seed++;
    }

    life_init(&world, seed);
    life_clear(&world);
    world.seed = seed; /* life_init consumed draws; rewind to the quiet seed */

    /* horizontal blinker in open space */
    life_set(&world, 3, 4, true);
    life_set(&world, 4, 4, true);
    life_set(&world, 5, 4, true);

    uint8_t start[LIFE_MAX_DIM][LIFE_ROW_BYTES];
    memcpy(start, world.cur, sizeof(start));

    /* one step: becomes vertical */
    life_update(&world, 10, 10);
    T_ASSERT(life_get(&world, 4, 3));
    T_ASSERT(life_get(&world, 4, 4));
    T_ASSERT(life_get(&world, 4, 5));
    T_ASSERT(!life_get(&world, 3, 4));
    T_ASSERT(!life_get(&world, 5, 4));

    /* second step: back to the original pattern */
    life_update(&world, 10, 10);
    T_ASSERT(memcmp(start, world.cur, sizeof(start)) == 0);
}

static void test_degenerate_dims(void)
{
    life_world_t world;

    life_init(&world, 3);
    life_update(&world, 0, 10);            /* no-op */
    life_update(&world, 10, -1);           /* no-op */
    life_update(NULL, 10, 10);             /* no crash */
    life_update(&world, 100000, 100000);   /* clamped to LIFE_MAX_DIM */
    life_clear(NULL);                      /* no crash */
    T_ASSERT(1);
}

int main(void)
{
    T_RUN(test_init);
    T_RUN(test_determinism);
    T_RUN(test_accessors_round_trip);
    T_RUN(test_no_live_cells_outside_window);
    T_RUN(test_window_shrink_leaves_nothing_behind);
    T_RUN(test_blinker_oscillates);
    T_RUN(test_degenerate_dims);
    return t_report("life");
}
