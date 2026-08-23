#ifndef LIFE_H
#define LIFE_H

#include <stdint.h>
#include <stdbool.h>

#define LIFE_CELL_SIZE 8
#define LIFE_MAX_DIM   (296 / LIFE_CELL_SIZE + 1)

/** Bytes per row once the row is packed one bit per cell. */
#define LIFE_ROW_BYTES ((LIFE_MAX_DIM + 7) / 8)

/*
 * One bit per cell, not one byte. The board only ever holds a binary state, so
 * a byte per cell spent 1444 B of a build whose RAM headroom is measured in
 * hundreds of bytes; two packed planes cost 380 B for the same 38x38 world.
 *
 * `cur` is the live generation, `nxt` is scratch that life_update() fills and
 * then commits. Bits at x >= the active width, and rows at y >= the active
 * height, are held at 0 — life_update() never writes outside the window it was
 * given, so stale cells cannot leak back in when the window grows.
 *
 * Reach for life_get()/life_set() rather than touching the planes: the bit
 * order is an implementation detail and the renderer has no business knowing
 * it.
 */
typedef struct {
    uint8_t cur[LIFE_MAX_DIM][LIFE_ROW_BYTES];
    uint8_t nxt[LIFE_MAX_DIM][LIFE_ROW_BYTES];
    uint32_t seed;
    bool initialized;
} life_world_t;

/** @return true if the cell at (@p x, @p y) is alive. Out-of-range reads 0. */
static inline bool life_get(const life_world_t *world, int x, int y)
{
    if (!world || x < 0 || y < 0 || x >= LIFE_MAX_DIM || y >= LIFE_MAX_DIM) {
        return false;
    }
    return (world->cur[y][(unsigned)x >> 3] >> ((unsigned)x & 7u)) & 1u;
}

/** Set the cell at (@p x, @p y). Out-of-range writes are dropped. */
void life_set(life_world_t *world, int x, int y, bool alive);

/** Clear every cell without disturbing the RNG state. */
void life_clear(life_world_t *world);

void life_init(life_world_t *world, uint32_t seed);
void life_update(life_world_t *world, int width, int height);

#endif // LIFE_H
