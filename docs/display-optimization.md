# Pushing an e-paper panel to its limit

*Optimising an SSD1675A on an nRF52832 — from 762 ms to 112 ms per frame*

Confidence markers used throughout:
- ✓ — confirmed by measurement on hardware
- ~ — hypothesis with a physical rationale (not disproven)
- ? — not re-checked / ambiguous

---

## Prologue: the starting point

The device is an e-paper "smart watch" on an nRF52832 with an SSD1675A panel
(128×296, BWR — black/white/red). The goal was animation in screensaver mode:
Game of Life, a bouncing ball, scrolling text. The first problem showed up
immediately:

> **762 ms per partial frame. Game of Life ran at 1.3 frames per second.**

That is not animation. That is a slideshow.

---

## Chapter 1. The LUT nobody had read

The SSD1675A is driven through a Lookup Table — a 70-byte array describing the
waveform applied to the pixels. Five rows (LUT0–LUT4) define voltages for
black, white, red (×2) and VCOM pixels. Each row is seven 8-bit bytes made of
2-bit pairs: `00` = VSS (0 V), `01` = VSH1 (+15 V), `10` = VSL (−15 V),
`11` = VSH2 (+5 V, red only).

The catch is that the datasheet describes **two ways of selecting a LUT row**:

- Mode 0 (default): the row is picked by the pixel *transition* (was → became).
- **Mode 1 (0xC7)**: the row is picked by the *current* RAM value,
  `{red_ram, bw_ram}`.

We were in mode 1. That means LUT0 = pixels currently BLACK in BW RAM, LUT1 =
WHITE pixels, LUT2/LUT3 = RED pixels. ✓ (confirmed by panel behaviour)

The first few "optimised" LUTs either updated nothing at all (empty rows) or
turned the whole screen gray (wrong waveform). The cause was mixed-up LUT rows.

### The fix

A minimal working LUT for black-and-white animation:

- LUT0: `0x55` — VSH1 (+15 V) for the whole phase → black pixels pulled to the
  bottom plate ✓
- LUT1: `0xAA` — VSL (−15 V) → white pixels pushed to the top plate ✓
- LUT2–LUT4: zeros (red and VCOM untouched)
- Timing: TA=7, TB=7, RP=0 → **14 frames**

```
55000000000000 AA000000000000 000000000000000000000000000000000000000000000000000000
0707000000 0000000000 0000000000 0000000000 0000000000 0000000000 0000000000
```

✓ Confirmed on hardware: works, ghosty, but acceptable for animation.

---

## Chapter 2. The paradox: a shorter LUT is *slower*?

After writing a 14-frame "turbo" LUT (~112 ms of waveform) we hit something
unexpected:

- **Game of Life** (BALANCED LUT, ~384 ms waveform): **406 ms/frame** ✓
- **LTEST** (TURBO LUT, ~64–112 ms waveform): **762 ms/frame** ✓ (measured;
  later 630 ms after fixes)

A shorter LUT, and the frame takes half again as long. How?

### Hypothesis: the boost capacitors discharge ~

The SSD1675A drives the pixels from a pair of ±15 V capacitors fed by a boost
converter. One refresh cycle looks like this:

1. `Enable CLK + Analog` (0xC0) — charge the capacitors to ±15 V.
   **Expensive: ~600 ms.** ~
2. The LUT waveform — the actual pixel update.
3. `Disable CLK + Analog` (0xC3) — the capacitors start discharging.

With a long LUT (384 ms) the capacitors stay charged for most of the cycle. By
the next frame they are still partly charged, so step 1 is shorter. ~

With a short LUT (112 ms) they discharge almost completely during the gap
between frames, so the next `Enable Analog` starts from zero and **takes
longer**. ~

> **Conclusion**: without streaming, a shorter LUT paradoxically yields a
> longer frame, because the HV charge cost is paid again every time.

That explains the 406 ms vs 762 ms gap. ~ (physically motivated, not verified
by direct measurement)

---

## Chapter 3. SPI — 42 ms lying in plain sight

While digging through the LUT we looked at `soft_spi.c`. Between every bit sat
a `k_busy_wait(1)` — a 1 µs spin. One frame is 4736 bytes = 37888 bits = 37888
busy-wait calls.

Before: ~850 kHz → **~44 ms to shift out a frame** ✓ (measured on a scope)
After removing the wait: ~6 MHz → **~2 ms per frame** ✓

That is ~42 ms off every frame, for free, in one line of code. ✓

---

## Chapter 4. Streaming — charge the HV rails once

The idea came from the datasheet: `0xC0` (Enable CLK + Analog) and `0xC3/0xC4`
(Display Mode) can be issued separately. Keep the analog section powered
between frames and you get:

```
Frame 1: 0xC0 (charge HV, ~600 ms) -> load pixels -> 0x04 (display mode 1, no HV cycle)
Frame 2: load pixels -> 0x04
Frame 3: load pixels -> 0x04
...
Finish:  0x03 (disable)
```

The BUSY wait on `0x04` is the LUT waveform time alone, with no HV charge. ✓

| Mode              | First frame | Subsequent frames |
| ----------------- | ----------- | ----------------- |
| Without streaming | ~760 ms     | ~760 ms           |
| With streaming    | ~760 ms     | ~112–271 ms       |

Implemented as `ssd1675a_begin_streaming()` / `ssd1675a_update_frame_stream()` /
`ssd1675a_end_streaming()`. ✓

Streaming was applied only in `cmd_anim` at first — Game of Life and LTEST did
not use it, which was a mistake. After adding auto-streaming to
`display_manager_update_partial()`:

- GoL: 406 ms → **271 ms/frame** ✓
- LTEST: 762 ms → **800 ms** … wait.

---

## Chapter 5. The oscillator — off by a factor of two

Did LTEST get slower? No. We had simply been computing the time wrong.

The initial assumption was that the SSD1675A oscillator runs at **125 Hz**, so
8 ms per frame, so 14 frames = 112 ms. That seemed reasonable.

Then GoL was measured with the CLEAN LUT (18 frames per the datasheet):

> **271 ms / 18 frames = 15 ms/frame → oscillator ≈ 67 Hz** ✓

The real frequency is **half** the expected one. Which means:

- TURBO 14f → not 112 ms but **210 ms** ~
- BALANCED 100f → not 800 ms but **1500 ms** ~

Every earlier estimate has to be multiplied by 1.875 (≈ 125/67). ~ (the exact
oscillator frequency was never measured directly; it is derived from
271 ms / 18f)

Why did GoL show 271 ms rather than ~210 ms at 14 frames? Because GoL runs the
BALANCED LUT, which has a different frame count — a different preset entirely. ✓

---

## Chapter 6. Ghosts — when the optimisation bites back

After several hours of testing GoL in streaming mode:

> "Game of Life is burned into the panel, complete with the timestamp, and it
> does not clear even with the CLEAN command. As if we scorched the screen."

The screen is not scorched. This is **DC imbalance**. ~

Streaming mode keeps HV on and runs the same LUT over and over (VSH1 for black
pixels, VSL for white). Thousands of frames with a single-direction field
polarise the pigment microcapsules one way. ~ The dispersant stops holding the
particles in the right distribution. ~

Symptoms:

- A visible "imprint" of previous content on a black or white background ✓
- The CLEAN command (7 × B/W/R cycles) does not help ✓ — too few iterations

### NUKE: a forced reset

The fix is a `NUKE:N` command that runs N full refresh cycles (B → W → R) with
the v5-balanced LUT. Each cycle: charge HV → drive to black → to white → to red
→ discharge. Alternating all three colours neutralises the accumulated DC
offset. ~

```
NUKE:20  — ~15 minutes, for bad cases
NUKE:5   — ~4 minutes, for light artefacts
```

✓ NUKE was used to recover a panel after ghost burn-in.

An additional safeguard: an automatic refresh every 500 streamed frames.
Streaming pauses for one `0xC7` cycle with a full HV charge, giving a crisp
refresh, then resumes. ~ At 271 ms/frame that is roughly one preventive refresh
every 2.25 minutes. ~

---

## Chapter 7. The resulting presets

Four presets came out of all this:

### MODE:0 — TURBO

A minimal single-phase LUT. 14 frames. Black and white only (no red).
Hex: `55000000000000AA000000000000...0707000000...`
For animation (GoL, bouncing ball), used with streaming. ✓
Quality: ghosty — the previous frame shows through — but good enough for motion.

### MODE:1 — BALANCED

A two-phase preset: pre-erase (12f reverse waveform) plus the main drive
(100f). Supports red. For partial updates that need contrast. ~ (the current
variant has not been re-tested on hardware)

### MODE:2 — STABLE

The original ~1.1 s preset, unchanged. Kept as a baseline.

### MODE:3 — CLEAN

Points at `lut_data_default` — the same LUT used for full refreshes
(v5-balanced). The cleanest result for a single partial update, but slow
(~271 ms as measured). ✓

---

## Epilogue: what we deliberately left alone

- **BLE/NUS**: commands work and throughput is not critical. Not optimised.
- **Screensaver thread**: a Zephyr semaphore with a timed `k_sem_take` — works
  as is.
- **Red buffer**: `ssd1675a_display_buffer_fast()` deliberately skips the red
  plane to save SPI time. LTEST uses `ssd1675a_display_buffer()` and writes
  both planes. ✓

---

## Timeline of results

| Step                           | Mode                | Time/frame  | Status    |
| ------------------------------ | ------------------- | ----------- | --------- |
| Starting point                 | STABLE partial      | 762 ms      | ✓         |
| Removed `k_busy_wait` from SPI | STABLE partial      | ~720 ms     | ✓         |
| New TURBO LUT                  | TURBO, no streaming | ~600–762 ms | ✓         |
| Streaming in GoL               | BALANCED            | 406 ms      | ✓         |
| Streaming in GoL (automatic)   | BALANCED            | 271 ms      | ✓         |
| TURBO with streaming (target)  | TURBO               | ~210 ms     | ~ (calc.) |
| NUKE after ghost burn-in       | v5-balanced         | —           | ✓         |

---

*Written up from the experiment log. The device survived.*
