# Host-side unit tests for the portable lib/ modules.
# Runs on any machine with a C compiler — no Zephyr, no hardware.
#   make test    — build + run everything (ASan + UBSan enabled)

CC     ?= cc
LIB    := ../../lib
CFLAGS := -std=c99 -g -O1 -Wall -Wextra \
          -fsanitize=address,undefined -fno-sanitize-recover=all \
          -I$(LIB)/gfx -I$(LIB)/eink

TESTS := test_graphics test_dither test_life test_eink_lut test_ssd1675a

all: $(TESTS)

test: all
	@rc=0; for t in $(TESTS); do ./$$t || rc=1; done; exit $$rc

test_graphics: test_graphics.c $(LIB)/gfx/graphics.c test.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_dither: test_dither.c $(LIB)/gfx/dither.c $(LIB)/gfx/graphics.c test.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_life: test_life.c $(LIB)/gfx/life.c test.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_eink_lut: test_eink_lut.c $(LIB)/eink/eink_lut.c test.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

test_ssd1675a: test_ssd1675a.c $(LIB)/eink/ssd1675a.c $(LIB)/eink/eink_lut.c test.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

clean:
	rm -f $(TESTS)
	rm -rf *.dSYM

.PHONY: all test clean
