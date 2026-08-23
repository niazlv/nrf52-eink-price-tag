#include "life.h"

#include <string.h>

static uint32_t life_rand(life_world_t *world)
{
    world->seed = world->seed * 1664525 + 1013904223;
    return world->seed;
}

/* ── Packed-plane accessors ─────────────────────────────────────────────── */

static inline bool plane_get(const uint8_t plane[][LIFE_ROW_BYTES], int x, int y)
{
    return (plane[y][(unsigned)x >> 3] >> ((unsigned)x & 7u)) & 1u;
}

static inline void plane_set(uint8_t plane[][LIFE_ROW_BYTES], int x, int y, bool v)
{
    uint8_t mask = (uint8_t)(1u << ((unsigned)x & 7u));
    if (v) {
        plane[y][(unsigned)x >> 3] |= mask;
    } else {
        plane[y][(unsigned)x >> 3] &= (uint8_t)~mask;
    }
}

void life_set(life_world_t *world, int x, int y, bool alive)
{
    if (!world || x < 0 || y < 0 || x >= LIFE_MAX_DIM || y >= LIFE_MAX_DIM) {
        return;
    }
    plane_set(world->cur, x, y, alive);
}

void life_clear(life_world_t *world)
{
    if (!world) {
        return;
    }
    memset(world->cur, 0, sizeof(world->cur));
    memset(world->nxt, 0, sizeof(world->nxt));
}

void life_init(life_world_t *world, uint32_t seed)
{
    if (!world) {
        return;
    }

    world->seed = seed;
    memset(world->nxt, 0, sizeof(world->nxt));
    /* Draw order is part of the contract: two worlds seeded alike must evolve
     * alike, so keep one draw per cell in row-major order even though the
     * bits now land eight to a byte. */
    for (int y = 0; y < LIFE_MAX_DIM; y++) {
        for (int x = 0; x < LIFE_MAX_DIM; x++) {
            plane_set(world->cur, x, y, ((life_rand(world) >> 16) < 24000));
        }
    }
    world->initialized = true;
}

static int wrap(int value, int limit)
{
    if (value < 0) {
        return limit - 1;
    }
    if (value >= limit) {
        return 0;
    }
    return value;
}

static int count_neighbors(const life_world_t *world, int x, int y, int width, int height)
{
    int count = 0;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }

            int nx = wrap(x + dx, width);
            int ny = wrap(y + dy, height);
            if (plane_get(world->cur, nx, ny)) {
                count++;
            }
        }
    }

    return count;
}

static void inject_glider(life_world_t *world, int x, int y, int width, int height)
{
    static const uint8_t pattern[3][3] = {
        {0, 1, 0},
        {0, 0, 1},
        {1, 1, 1},
    };

    for (int py = 0; py < 3; py++) {
        for (int px = 0; px < 3; px++) {
            int nx = wrap(x + px, width);
            int ny = wrap(y + py, height);
            plane_set(world->cur, nx, ny, pattern[py][px] != 0);
        }
    }
}

static void inject_random_block(life_world_t *world, int x, int y, int width, int height)
{
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            int nx = wrap(x + px, width);
            int ny = wrap(y + py, height);
            plane_set(world->cur, nx, ny, (life_rand(world) % 2) != 0);
        }
    }
}

void life_update(life_world_t *world, int width, int height)
{
    if (!world || width <= 0 || height <= 0) {
        return;
    }

    if (width > LIFE_MAX_DIM) {
        width = LIFE_MAX_DIM;
    }
    if (height > LIFE_MAX_DIM) {
        height = LIFE_MAX_DIM;
    }

    if ((life_rand(world) % 100) < 2) {
        int rx = life_rand(world) % width;
        int ry = life_rand(world) % height;
        if (life_rand(world) % 2 == 0) {
            inject_glider(world, rx, ry, width, height);
        } else {
            inject_random_block(world, rx, ry, width, height);
        }
    }

    /* Compute into the scratch plane, then commit. The previous in-place trick
     * (next state in bit 1 of the same byte) is what the second plane replaces;
     * both planes together are still a quarter of the old byte-per-cell world.
     *
     * Wipe the scratch first. The commit below copies whole rows, so anything
     * left in `nxt` outside this call's window rides back into `cur` — and the
     * window does change size: the renderer derives it from the canvas, so a
     * ROT: while the dynamic screensaver runs swaps 37x16 for 16x37. Without
     * this a shrink left a frozen block of cells from the wider generation
     * sitting on screen. 380 bytes; it does not register against the
     * neighbour count below. */
    memset(world->nxt, 0, sizeof(world->nxt));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int neighbors = count_neighbors(world, x, y, width, height);
            bool alive = plane_get(world->cur, x, y);
            bool next_state = alive ? (neighbors == 2 || neighbors == 3)
                                    : (neighbors == 3);
            plane_set(world->nxt, x, y, next_state);
        }
    }

    /* Commit whole rows. `nxt` was wiped above and only written inside the
     * window, so copying the full row also clears everything outside it —
     * neither growing nor shrinking the window can resurrect old cells. */
    for (int y = 0; y < height; y++) {
        memcpy(world->cur[y], world->nxt[y], LIFE_ROW_BYTES);
    }
    for (int y = height; y < LIFE_MAX_DIM; y++) {
        memset(world->cur[y], 0, LIFE_ROW_BYTES);
    }
}
