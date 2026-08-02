#ifndef LIFE_H
#define LIFE_H

#include <stdint.h>
#include <stdbool.h>

#define LIFE_CELL_SIZE 8
#define LIFE_MAX_DIM   (296 / LIFE_CELL_SIZE + 1)

typedef struct {
    uint8_t cells[LIFE_MAX_DIM][LIFE_MAX_DIM];
    uint32_t seed;
    bool initialized;
} life_world_t;

void life_init(life_world_t *world, uint32_t seed);
void life_update(life_world_t *world, int width, int height);

#endif // LIFE_H
