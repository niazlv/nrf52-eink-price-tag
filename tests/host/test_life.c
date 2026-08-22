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
            T_ASSERT_MSG(world.cells[y][x] <= 1, "cell (%d,%d) = %d",
                         x, y, world.cells[y][x]);
            alive += world.cells[y][x];
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
    T_ASSERT(memcmp(a.cells, b.cells, sizeof(a.cells)) == 0);
    T_ASSERT_EQ(a.seed, b.seed);
}

static void test_cells_stay_binary(void)
{
    life_world_t world;

    life_init(&world, 7);
    for (int i = 0; i < 20; i++) {
        life_update(&world, LIFE_MAX_DIM, LIFE_MAX_DIM);
    }
    for (int y = 0; y < LIFE_MAX_DIM; y++) {
        for (int x = 0; x < LIFE_MAX_DIM; x++) {
            T_ASSERT_MSG(world.cells[y][x] <= 1, "cell (%d,%d) = %d",
                         x, y, world.cells[y][x]);
        }
    }
}

static void test_blinker_oscillates(void)
{
    life_world_t world;
    uint32_t seed = 1;

    while (!seed_is_quiet(seed, 2)) {
        seed++;
    }

    life_init(&world, seed);
    memset(world.cells, 0, sizeof(world.cells));
    world.seed = seed; /* life_init consumed draws; rewind to the quiet seed */

    /* horizontal blinker in open space */
    world.cells[4][3] = 1;
    world.cells[4][4] = 1;
    world.cells[4][5] = 1;

    uint8_t start[LIFE_MAX_DIM][LIFE_MAX_DIM];
    memcpy(start, world.cells, sizeof(start));

    /* one step: becomes vertical */
    life_update(&world, 10, 10);
    T_ASSERT_EQ(world.cells[3][4], 1);
    T_ASSERT_EQ(world.cells[4][4], 1);
    T_ASSERT_EQ(world.cells[5][4], 1);
    T_ASSERT_EQ(world.cells[4][3], 0);
    T_ASSERT_EQ(world.cells[4][5], 0);

    /* second step: back to the original pattern */
    life_update(&world, 10, 10);
    T_ASSERT(memcmp(start, world.cells, sizeof(start)) == 0);
}

static void test_degenerate_dims(void)
{
    life_world_t world;

    life_init(&world, 3);
    life_update(&world, 0, 10);            /* no-op */
    life_update(&world, 10, -1);           /* no-op */
    life_update(NULL, 10, 10);             /* no crash */
    life_update(&world, 100000, 100000);   /* clamped to LIFE_MAX_DIM */
    T_ASSERT(1);
}

int main(void)
{
    T_RUN(test_init);
    T_RUN(test_determinism);
    T_RUN(test_cells_stay_binary);
    T_RUN(test_blinker_oscillates);
    T_RUN(test_degenerate_dims);
    return t_report("life");
}
