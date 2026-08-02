#include "life.h"

static uint32_t life_rand(life_world_t *world)
{
    world->seed = world->seed * 1664525 + 1013904223;
    return world->seed;
}

void life_init(life_world_t *world, uint32_t seed)
{
    if (!world) {
        return;
    }

    world->seed = seed;
    for (int y = 0; y < LIFE_MAX_DIM; y++) {
        for (int x = 0; x < LIFE_MAX_DIM; x++) {
            world->cells[y][x] = ((life_rand(world) >> 16) < 24000) ? 1 : 0;
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
            if (world->cells[ny][nx] & 1) {
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
            world->cells[ny][nx] = pattern[py][px];
        }
    }
}

static void inject_random_block(life_world_t *world, int x, int y, int width, int height)
{
    for (int py = 0; py < 4; py++) {
        for (int px = 0; px < 4; px++) {
            int nx = wrap(x + px, width);
            int ny = wrap(y + py, height);
            world->cells[ny][nx] = life_rand(world) % 2;
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

    /* In-place update: pack next state into bit 1 of each cell (current state
     * is in bit 0). count_neighbors reads only bit 0 via (cell & 1).
     * After all cells are computed, shift bit 1 down to bit 0. 
     * This avoids needing a full next[MAX_DIM][MAX_DIM] array (~1.4 KB saved). */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int neighbors = count_neighbors(world, x, y, width, height);
            uint8_t alive = world->cells[y][x] & 1;
            uint8_t next_state = alive
                ? (neighbors == 2 || neighbors == 3)
                : (neighbors == 3);
            world->cells[y][x] = alive | (next_state << 1);
        }
    }

    /* Commit: shift bit 1 down to bit 0 */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            world->cells[y][x] = (world->cells[y][x] >> 1) & 1;
        }
    }
}
