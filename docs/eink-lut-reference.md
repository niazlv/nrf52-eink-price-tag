# E-ink BWR 2.9" (SSD1675A-class controller) — LUT reference

Date: 2026-06-09. Source: a series of live experiments on the panel. Claims are
tagged: **[confirmed]** — verified by measurement or observation,
**[hypothesis]** — best available explanation, needs a test,
**[not re-checked]** — a diagnosis that was never re-tested.

---

## 1. Confirmed hardware parameters

| Parameter | Value | Evidence |
|---|---|---|
| Frame time | ≈ 8.0 ms (~125 Hz) | measurements of 2638 ms (256 frames) and 3300 ms (344 frames) |
| Fixed per-refresh overhead | ≈ 0.55 s (booster power-on, temperature, support circuitry) | the same measurements, plus 828–987 ms on a 40-frame waveform |
| RP semantics | a group runs **RP+1** times; RP=0 means one pass, groups are never skipped | the 3.3 s prediction for v3 matched the measured 3300 ms |
| Timing formula | T ≈ frames × 8.0 ms + 550 ms | consistent across three independent data points |
| Table format | 70 bytes: 5 LUTs × 7 VS bytes + 7 groups × (TP_A, TP_B, TP_C, TP_D, RP) | the decoding is consistent with every observed behaviour |
| VS codes (2 bits, phase A = bits 7:6 … D = 1:0) | 00=VSS · 01=VSH1 (+15 V, toward black) · 10=VSL (−15 V, toward white) · 11=VSH2 (+5 V, red development) | behaviour of every version |
| LUT mapping | LUT0 = black (red=0, bw=0) · LUT1 = white (red=0, bw=1) · LUT2 = LUT3 = red (red=1) · **LUT4 = VCOM** | see §3 |
| LUT4 = VCOM | zeros mean VCOM stays at the DC level from 0x2C. **Never write pixel codes there** | v4.1: copying the red waveform into LUT4 turned the whole screen red, black and white went pink, red went "turbo" |
| Stock table | 836 frames ≈ 6.7 s of waveform + overhead = 7.0 s; DC converges to zero at VSH2 ≈ 5.5 V | arithmetic plus a 7000 ms measurement |

Disproven: "repeat must be ≥1" — RP=0 works (one pass). The source of that
error was an empty screen in a partial test, which turned out to be a workflow
problem (see §3.2), not RP.

Disproven: the WW/BW/WB/BB labels (prev→new transitions) for LUT0–LUT3 in the
current BWR mode. Proof: if LUT0 were "white→white", white areas would darken
under the black waveform — never observed in any version; the behaviour always
matched the {red, bw} mapping.

---

## 2. Fixes for the UI visualiser

1. **VS row labels**: `WW/BW/WB/BB/RED` → `BLACK (00) / WHITE (01) / RED (10) /
   RED (11) / VCOM`. Transition labels belong to mono mode with prev/new and do
   not apply here.
2. **A warning** when the LUT4 row contains any non-zero byte (always an error
   on this chip).
3. **A "passes" column** = RP+1. Frames per group = (TPA+TPB+TPC+TPD)×(RP+1).
   Total frames = the sum over groups. Predicted time = frames × 8.0 ms + 550 ms.
4. **A per-LUT DC balance column**: weight VSH1=+1, VSL=−1, VSH2=+VSH2/VSH1
   (at 5/15 that is +0.333), VSS=0; Σ = Σ over groups (RP+1) × Σ over phases
   (weight × TP). Highlight |Σ| > 10 units (≈150 V·frames).
5. **Verification markers after upload**: display the checksum bytes of the
   loaded table. Precedent: a table once failed to arrive (corrupted hex) and
   the old version was tested instead.

---

## 3. Version history: what was loaded, what was seen, what was established

### 3.0 Stock (factory register table)

Markers: LUT0 `22 11 10 00 10`, red `6A 9B 9B 9B 9B`, groups 0/1/4, RP 01/02/07.
Measured: 7000 ms. Observations: B/W normal, red "scarlet" (light red),
gradual burn-in / image sticking.
Established: red is driven by LUT2/LUT3; the waveform structure is
erase → pump → 8 × (shake + VSH2 for 60 frames); 836 frames.

### 3.1 Partial test #1

Table: LUT1=`60`, LUT2=`90`, everything else zero, G0 = 4/16.
Measured: 828–987 ms. Observations: a slight twitch, the final screen is blank.
**[not re-checked]** Diagnosis: LUT0 (prev=0, new=0) is a no-op, and the prev
RAM (0x26) did not hold the previous image, so black was never driven. The
workflow was fixed (see §6), but the test was never repeated.

### 3.2 Hybrid (stock VS + fast timings) — a crash

Measured: 2600 ms. Observations: red came out black, black came out reddish.
**[confirmed]** The VS bytes and the timings are one waveform cut in half;
halves of different tables must not be mixed, or bytes land in the wrong time
slots.

### 3.3 v2 (`66 90 90 90` / `66 60 60 60` / `66 80 9B`, RP=01 throughout)

Measured: 2638 ms. Observations: "red actually looks red" (though washed out
and bright), black is grayish, strong strobing flicker (4 fast flips).
**[confirmed]**: back-to-back VSL+VSH1 pairs smear black (VSL erases what was
just accumulated); 112 frames of VSH2 over a white base with a shake produce
visible red. **v2 is the current benchmark for red visibility.**

### 3.4 v3 (`A0 11 10 10` / `50 88 80 20` / `80 93 9B`, G0 32/32, G2 4/4/8/72 ×2)

Measured: 3300 ms. Observations: black became deep ✓; red very washed out and
turns gray at the end.
**[confirmed]**:

- The black architecture works: a large erase reserve in G0, then VSH1 pulses
  separated by **VSS** pauses (not VSL!), then a final fixation.
- A dark base kills red: VSH2 cannot lift black off the surface (black needs
  −15 V to go down), so black raised by VSH1 stays put → gray.
- VSH2 runs longer than ~60 frames start dragging black up too (VSH2 sits at
  the threshold of black's mobility) → a gray film toward the end. Keep runs
  ≤60 frames and precede each with a VSL shake (which sweeps off creeping black).

### 3.5 "v4", attempt 1 — never loaded

The visualiser screenshot still showed v3 (markers `20 20`, `48/01`,
`80 93 9B`). Cause: a corrupted hex string.
**[confirmed]**: verify the markers after every upload (see §2 item 5).

### 3.6 v4.1 (v4 + a copy of red in LUT4) — a crash, but an informative one

Observations: the whole screen flickers red; black, white and everything else
go pink; red itself is "bright and really deep".
**[confirmed]**: LUT4 = VCOM (the only row that acts on all pixels at once);
zeros keep VCOM at DC. Side finding: the film is capable of a deep red given a
stronger development field.

### 3.7 v4 clean (`20 88 9B`, G0 44/32, G2 4/4/8/60 ×6, LUT4=0)

Measured: ~5.6 s. Observations: black is deep with no tint, but red is **the
palest of them all** — paler than v2, despite 360 frames of VSH2 against 112.
**[confirmed]**: the amount of VSH2 time is not the main driver of red
saturation.

---

## 4. The physics of red — summary

**[confirmed]**

- Red only develops over a clean/white base (v3: dark base → gray).
- VSH2 runs ≤ 60 frames, each preceded by a VSL shake (v3: 72-frame runs → gray
  film).
- Development time alone does not settle it (v4: 360 frames came out paler than
  v2's 112).

**[hypothesis — the "ratchet", consistent with all four data points]**

Red needs a full-amplitude lift: a **VSH1 pulse immediately followed by VSL**.
VSH1 (+15) raises black and red together; VSL (−15) sends black back down
quickly while the slower red lags behind and stays raised; VSH2 then rolls it
the rest of the way to the surface. Checking against each version:

- Stock: G1 = `0x9B` ×3 (VSL→VSH1→VSL→VSH2) — the ratchet is there → red visible.
- v2: shake `0x66` (alternating VSH1/VSL) — the ratchet is there → red visible.
- v3: `0x93` — VSH1 is followed by VSS, black is not swept off → gray.
- v4: VSH1 almost removed (4 frames per cycle) plus a deep hard erase packed red
  onto the bottom → the palest result.

The test for this hypothesis is v5 (§5.2): if red at 300 development frames
**with** the ratchet beats v4's 360 frames **without** it, the hypothesis holds.

---

## 5. Tables

### 5.1 v4 — currently loaded (black excellent, red pale), ~5.6 s

```
A0111010000000   LUT0 black
50888020000000   LUT1 white
20889B00000000   LUT2 red
20889B00000000   LUT3 red
00000000000000   LUT4 VCOM — zeros
2C20000000       G0: 44/32, x1
0A0A0A0A01       G1: 10x4, x2
0404083C05       G2: 4/4/8/60, x6
0C0C000000       G3: 12/12, x1
000000000000000000000000000000
```

On one line:

```
A01110100000005088802000000020889B0000000020889B00000000000000000000002C200000000A0A0A0A010404083C050C0C000000000000000000000000000000000000
```

DC: black 0, white 0, red 0 (exact zeros).

### 5.2 v5 — to be tested (the ratchet from stock), ~5.0 s

Differences from v4: red G0 `0x20→0x80`, G1 `0x88→0x9B` (the stock ratchet ×2),
G2 RP `05→04` (5 passes).

```
A0111010000000   LUT0 black (unchanged)
50888020000000   LUT1 white (unchanged)
809B9B00000000   LUT2 red
809B9B00000000   LUT3 red
00000000000000   LUT4 VCOM — zeros
2C20000000       G0: 44/32, x1
0A0A0A0A01       G1: 10x4, x2
0404083C04       G2: 4/4/8/60, x5
0C0C000000       G3: 12/12, x1
000000000000000000000000000000
```

On one line:

```
A011101000000050888020000000809B9B00000000809B9B00000000000000000000002C200000000A0A0A0A010404083C040C0C000000000000000000000000000000000000
```

Markers after upload: the red row starts `80 9B 9B`, Ph2 = `04 04 08 3C / 04`,
LUT4 is all zeros.
DC: black −4, white +4, red +2.7 units (≈ ±60 V·frames, zero for practical
purposes).

If red beats v4 but not by enough, step toward stock: G1 RP `01→02` (ratchet
×3, as in stock; red then goes to −20 units, compensate with red G0
`0x80→0x20`), then G2 RP up to `07` (8 passes, +12 units per step).

A legitimate depth knob: raise VSH2 via command 0x04 from 5 V to ~6 V — the
field grows only on red pixels. At 6 V, recompute the VSH2 balance weight as
0.4. Risk: a gray film (black's threshold); back off to 5 V if it appears.

### 5.3 Partial (black and white only), ~0.16 s of waveform + overhead

```
00000000000000   LUT0: 0→0, untouched
60000000000000   LUT1: 0→1 (to white): VSH1 4f + VSL 16f
90000000000000   LUT2: 1→0 (to black): VSL 4f + VSH1 16f
00000000000000   LUT3: 1→1, untouched
00000000000000   LUT4 VCOM — zeros
0410000000       G0: 4/16, x1 (RP=0 is valid — confirmed)
000000000000000000000000000000000000000000000000000000000000
```

Properties: unchanged pixels are not driven (no flicker); existing red on the
screen survives a partial update as long as nothing is drawn over it; a partial
update cannot draw *new* red (0x26 is occupied by the previous frame). DC: ±12
units per transition, cancelled by the reverse transition of the same pixel;
static pixels are 0.

---

### 5.4 v5-balanced — **the current default** ✓, ~8707 ms **[confirmed]**

The outcome of the experiment series. Bright, deep red with DC balance driven
to zero for every group.

**Changes relative to stock:**

- RED Ph2 (lut[16], lut[23]): `0x00 → 0xA8` (= A:VSL B:VSL C:VSL — three VSL slots)
- Ph5, Ph6 of LUT2/3: `0x00 → 0xFF` (= all VSH2 — red fixation)
- Ph2 timing: `00 00 00 00 00 → 14 14 14 14 01` (160 frames, 2 passes)
- Ph4 timing: TD=`3B`, RP=`07` (8 passes, the main red drive)
- Ph5/Ph6 timing: TA=TB=TC=`14`, TD=`3B`, RP=`00` (fixation)

**How Ph2 works:** VSL in 3 slots for RED gives −120 units of compensation over
160 frames. At the same time BLACK Ph2=`0x10` (B:VSH1) gives +40 and WHITE
Ph2=`0x80` (A:VSL) gives −40. With TA=TB=TC=TD=20, RP=1, all three groups come
out at zero.

**DC balance:**

| Group | Before | After    |
|-------|--------|----------|
| BLACK | −40    | **0.0**  |
| WHITE | +40    | **0.0**  |
| RED   | +116.7 | **−3.3** |

**Physical meaning of Ph2:** VSL before Ph4 extends the ratchet — the pigment
goes deeper before the drive. It may boost red brightness by increasing the
amplitude of the lift.

```text
VS:
LUT0 BLACK  22 11 10 00 10 00 00
LUT1 WHITE  11 88 80 80 80 00 00
LUT2 RED    6A 9B A8 9B 9B FF FF
LUT3 RED    6A 9B A8 9B 9B FF FF
LUT4 VCOM   00 00 00 00 00 00 00
Timing:
Ph0  00 14 00 12 01
Ph1  06 06 06 06 02
Ph2  14 14 14 14 01   <- compensation
Ph3  00 00 00 00 00
Ph4  00 00 04 3B 07   <- the main red drive
Ph5  14 14 14 3B 00   <- VSH2 fixation
Ph6  14 14 14 3B 00   <- VSH2 fixation
```

On one line (paste into the UI):

```text
22111000100000118880808000006A9BA89B9BFFFF6A9BA89B9BFFFF0000000000000000140012010606060602141414140100000000000000043B071414143B001414143B00
```

Frames: 1026. Predicted: ~8758 ms. Measured: **8707 ms** (+0.6% error — the
formula holds).

---

## 6. Partial-update workflow **[not re-checked since the fix]**

1. Full refresh with image X.
2. Write X into **both 0x24 and 0x26** (reset the 0x4E/0x4F pointers before
   each write).
3. Load the partial LUT (0x32); border 0x3C at a fixed level.
4. Loop: Y → 0x24 → display → BUSY → Y → 0x26.
5. A full refresh every ~20–30 partial updates.

Test: fill white (0xFF into both RAMs), then draw a black rectangle with a
single partial update. Blank result → swap the LUT1/LUT2 bytes (prev/new in the
opposite order). A white rectangle on a darkened background → the buffer
polarity is inverted (in 0x24, bit 1 = white).

The RAM window (0x44/0x45 plus 0x4E/0x4F) lets you send only the changed
rectangle. It does not shorten the waveform (all gates are scanned anyway) but
it does save transfer time.

---

## 7. Waveform construction rules (condensed)

1. VS bytes and timings are a single unit; never mix halves of different tables.
2. LUT4 = VCOM = always zeros.
3. Black: an erase reserve at the start, then VSH1 pulses with VSS pauses (not
   VSL), then a continuous 10–12 frame fixation at the end.
4. Red: a clean white base; no "bare" VSH1 without a following VSL; VSH2 runs
   ≤60 frames with a shake before each; gain saturation through cycle passes
   and — carefully — through the VSH2 level.
5. Drive DC balance to zero for every LUT (formula in §2 item 4); put the
   compensation for red's VSH2 surplus into the erase phases, not the tail of
   the waveform.
6. Verify markers after upload; run 2 full passes (a wash) before judging colour.
7. A custom LUT disables the OTP temperature compensation: calibration is
   room-temperature, so increase TP/RP in the cold.

## 8. Panel hygiene

- VCOM (0x2C) must match the panel's own marking; a wrong VCOM means a DC
  offset on every frame, which no amount of LUT balancing will fix.
- Power off / deep sleep after every update; never interrupt a waveform by
  cutting power.
- After a crash (like v4.1), run 2–3 full passes or white fills to wash the
  panel.
