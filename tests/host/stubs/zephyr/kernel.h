/* Host stand-in for <zephyr/kernel.h>: only the helpers the src/app modules
 * under test actually use. Anything else is a compile error on purpose — a
 * module that needs more than this is not a host-testable module yet. */
#ifndef STUB_ZEPHYR_KERNEL_H
#define STUB_ZEPHYR_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ARG_UNUSED(x) (void)(x)
#define BIT(n) (1u << (n))

#endif
