#!/usr/bin/env python3
"""
SSD1675A LUT Tester + Device Controller — macOS BLE companion.

Supports both:
  • LUT-Tester   (lut_tester sub-project firmware)
  • nrf52-E-ink-clock-DEV  (root peripheral_uart firmware)

Layout:
  Top bar   — BLE connect + LUT actions + test patterns
  Dev bar   — Device-specific controls (time, display modes, text, etc.)
  Left      — 70-byte LUT hex editor
  Right     — Waveform canvas + DC balance bars
  Bottom    — Status + console log
"""

import asyncio
import threading
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import datetime, struct, time

try:
    from bleak import BleakClient, BleakScanner
    BLEAK_OK = True
except ImportError:
    BLEAK_OK = False

try:
    from PIL import Image, ImageDraw, ImageFont
    PIL_OK = True
except ImportError:
    PIL_OK = False

# Physical display dimensions (128 wide × 296 tall)
DISP_W = 128
DISP_H = 296
FB_SIZE = DISP_W * DISP_H // 8          # 4736 bytes
FW_BYTES_PER_CHUNK = 48                  # 96 hex chars per FW/RW line
RW_BYTES_PER_CHUNK = 48

# NUS UUIDs
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# Known device names (shown in the device selector)
DEVICE_NAMES = [
    "LUT-Tester",
    "nrf52-E-ink-clock-DEV",
]

# Dark theme
C = {
    'bg':      '#1e1e1e',
    'bg2':     '#252526',
    'bg3':     '#2d2d2d',
    'border':  '#3c3c3c',
    'fg':      '#d4d4d4',
    'fg2':     '#9d9d9d',
    'accent':  '#569cd6',
    'green':   '#4ec9b0',
    'red':     '#f44747',
    'orange':  '#ce9178',
    'yellow':  '#dcdcaa',
    'vgh':     '#e06c75',   # VSH1 +15V → black
    'vgl':     '#61afef',   # VSL  −15V → white/erase
    'vss':     '#4a4a4a',   # VSS  0V   → neutral
    'vsh2':    '#d19a66',   # VSH2 +5V  → red reveal
}

# LUT row semantics confirmed by experiment (NOT prev→new transitions).
# LUT0=BLACK pixel, LUT1=WHITE pixel, LUT2=LUT3=RED pixel, LUT4=VCOM (always zeros!).
GROUP_COLORS = ['#d4d4d4', '#98c379', '#e06c75', '#e06c75', '#666666']
GROUP_LABELS = ['BLACK (00)', 'WHITE (01)', 'RED (10)', 'RED (11)', 'VCOM (nul!)']

_GROUP_TIPS = [
    "BLACK (code 00) — bw=0, red=0\n"
    "Waveform applied to pixels rendered black.\n"
    "VSH1 (+15V) drives black pigment to the surface; VSL (−15V) erases it.\n"
    "Good black: erase phase first, then VSH1 bursts with VSS gaps.\n"
    "▶ Strong VSH1 pulses + clean VSL erase = deep, ghost-free black.",

    "WHITE (code 01) — bw=1, red=0\n"
    "Waveform applied to pixels rendered white (clear).\n"
    "VSL (−15V) sweeps black pigment down; VSS rests.\n"
    "VSH1 is usually not needed for a white result.\n"
    "▶ Mostly VSL phases; reduce TP for faster clear at cost of uniformity.",

    "RED (code 10) — red=1 (first copy)\n"
    "Drives red pigment electrode. Red needs a clean white base first.\n"
    "Key mechanism: VSH1→VSL 'ratchet' lifts red pigment, VSH2 (+5V) carries it up.\n"
    "VSH2 chunks should be ≤60 frames; longer bursts → gray creep.\n"
    "⚠ LUT2 and LUT3 must be identical on this chip.",

    "RED (code 11) — red=1 (second copy)\n"
    "Second copy of the red waveform — same physical effect as LUT2.\n"
    "Keep identical to LUT2 (RED 10) at all times.\n"
    "Divergence between LUT2 and LUT3 may cause uneven red saturation.",

    "VCOM (LUT4) — MUST BE ALL ZEROS!\n"
    "LUT4 drives the VCOM electrode which affects ALL pixels simultaneously.\n"
    "Any non-zero VS byte here applies a global field over the entire display.\n"
    "⚠ Non-zero VCOM = immediate full-screen colour disaster (red tint on everything).\n"
    "Keep all 7 VS bytes at 0x00. Timing bytes are irrelevant but set to 0 too.",
]

_TIMING_COL_TIPS = [
    "TP_A — Timing Pulse A (oscillator frames, ~8ms each).\n"
    "Duration of voltage slot A within one phase.\n"
    "VS byte bit mapping: A=bits[7:6], B=bits[5:4], C=bits[3:2], D=bits[1:0].\n"
    "Range 0-255.  0 = slot A contributes no time (but phase still runs if RP≥0).",

    "TP_B — Timing Pulse B (oscillator frames, ~8ms each).\n"
    "Duration of voltage slot B within one phase.\n"
    "VS byte: B=bits[5:4].\n"
    "Range 0-255.",

    "TP_C — Timing Pulse C (oscillator frames, ~8ms each).\n"
    "Duration of voltage slot C. VS byte: C=bits[3:2].",

    "TP_D — Timing Pulse D (oscillator frames, ~8ms each).\n"
    "Duration of voltage slot D. VS byte: D=bits[1:0].",

    "RP — Repeat count.\n"
    "The full A→B→C→D sequence executes RP+1 times total.\n"
    "RP=0 → runs ONCE (NOT skipped!). RP=1 → runs TWICE.\n"
    "Phase frames = (TP_A + TP_B + TP_C + TP_D) × (RP+1)\n"
    "Total time ≈ Σ frames × 8.0ms + 550ms fixed overhead.\n"
    "▶ Reduce TP values first (fine-grained), reduce RP for coarse steps.",
]

_PHASE_TIPS = [
    "Phase 0 — first activation stage.\nTypically the main drive pulse.",
    "Phase 1 — secondary drive or partial reversal.",
    "Phase 2 — charge balancing / cleanup.",
    "Phase 3 — fine DC compensation.",
    "Phase 4 — optional extra refinement.",
    "Phase 5 — optional; often unused (RP=0).",
    "Phase 6 — optional; often unused (RP=0).",
]


class Tooltip:
    """Delayed hover tooltip for tkinter widgets (matches dark theme)."""
    def __init__(self, widget, text, delay=650, wraplength=400):
        self._w = widget
        self._text = text
        self._delay = delay
        self._wraplength = wraplength
        self._after_id = None
        self._tw = None
        widget.bind('<Enter>', self._schedule, add='+')
        widget.bind('<Leave>', self._cancel,   add='+')
        widget.bind('<ButtonPress>', self._cancel, add='+')

    def _schedule(self, _event=None):
        self._cancel()
        self._after_id = self._w.after(self._delay, self._show)

    def _cancel(self, _event=None):
        if self._after_id:
            self._w.after_cancel(self._after_id)
            self._after_id = None
        if self._tw:
            self._tw.destroy()
            self._tw = None

    def _show(self):
        x = self._w.winfo_rootx() + 20
        y = self._w.winfo_rooty() + self._w.winfo_height() + 4
        self._tw = tk.Toplevel(self._w)
        self._tw.wm_overrideredirect(True)
        self._tw.wm_geometry(f'+{x}+{y}')
        tk.Label(self._tw, text=self._text, justify='left',
                 bg='#2a2a2a', fg='#d4d4d4', relief='solid', borderwidth=1,
                 font=('Menlo', 10), wraplength=self._wraplength,
                 padx=8, pady=6).pack()


LUT_SIZE    = 70
VS_GROUPS   = 5
PHASES      = 7
# v5-balanced: ~8707ms, DC balance BLK=0 WHT=0 RED=-3.3
# Ph2 = DC compensation + ratchet extension (LUT2/3: 0xA8 = A:VSL B:VSL C:VSL)
# Ph5+Ph6 = 0xFF fixation for red reveal. Ph4 RP=7 = main red drive.
FACTORY_LUT = bytes([
    # VS  LUT0 BLACK
    0x22,0x11,0x10,0x00,0x10,0x00,0x00,
    # VS  LUT1 WHITE
    0x11,0x88,0x80,0x80,0x80,0x00,0x00,
    # VS  LUT2 RED  (Ph2=0xA8 compensation)
    0x6A,0x9B,0xA8,0x9B,0x9B,0xFF,0xFF,
    # VS  LUT3 RED  (same)
    0x6A,0x9B,0xA8,0x9B,0x9B,0xFF,0xFF,
    # VS  LUT4 VCOM (always zeros)
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    # Timing  Ph0        Ph1        Ph2        Ph3
    0x00,0x14,0x00,0x12,0x01, 0x06,0x06,0x06,0x06,0x02,
    0x14,0x14,0x14,0x14,0x01, 0x00,0x00,0x00,0x00,0x00,
    # Timing  Ph4        Ph5        Ph6
    0x00,0x00,0x04,0x3B,0x07, 0x14,0x14,0x14,0x3B,0x00,
    0x14,0x14,0x14,0x3B,0x00,
])


# ── BWR frame helpers ──────────────────────────────────────────────────────

def _make_phys_buffers(logical_img):
    """Convert logical 296×128 RGB image → physical (bw_buf, red_buf) each 4736 B."""
    img = logical_img.resize((DISP_H, DISP_W), Image.LANCZOS).convert('RGB')
    px  = img.load()
    bw  = bytearray(b'\xff' * FB_SIZE)
    red = bytearray(FB_SIZE)
    for ly in range(DISP_W):
        for lx in range(DISP_H):
            r, g, b = px[lx, ly]
            phx = DISP_W - 1 - ly
            phy = lx
            bi  = phy * (DISP_W // 8) + phx // 8
            bit = 7 - (phx & 7)
            if r > 160 and g < 80 and b < 80:
                red[bi] |= 1 << bit
                bw[bi]  |= 1 << bit
            elif r < 100 and g < 100 and b < 100:
                bw[bi] &= ~(1 << bit)
    return bw, red


def _make_font():
    try:
        return ImageFont.load_default(size=12)
    except TypeError:
        return ImageFont.load_default()


def _pattern_color_bands():
    img = Image.new('RGB', (296, 128), 'white')
    d, f = ImageDraw.Draw(img), _make_font()
    d.rectangle([0, 0, 97, 127],    fill='black')
    d.rectangle([198, 0, 295, 127], fill=(200, 0, 0))
    d.rectangle([98, 0, 197, 127],  outline='black', width=2)
    d.text((14, 56),  "BLACK", fill='white',   font=f)
    d.text((110, 56), "WHITE", fill='black',   font=f)
    d.text((214, 56), "RED",   fill='white',   font=f)
    d.rectangle([0, 0, 295, 12], fill='black')
    d.text((4, 1), "BWR COLOR BANDS TEST — SSD1675A", fill='white', font=f)
    for i in range(8):
        cx = 12 + i * 36
        d.ellipse([cx, 102, cx+16, 118], fill='white' if i % 2 == 0 else 'black')
    for i in range(8):
        cx = 12 + i * 36
        d.ellipse([cx, 102, cx+16, 118], outline='black', width=1)
    return img


def _pattern_text_demo():
    img = Image.new('RGB', (296, 128), 'white')
    d, f = ImageDraw.Draw(img), _make_font()
    d.rectangle([0, 0, 295, 12], fill='black')
    d.text((4, 1), "SSD1675A — BWR TEXT & SHAPES TEST", fill='white', font=f)
    d.text((4, 16),  "BLACK", fill='black', font=f)
    d.text((4, 30),  "Hello!", fill='black', font=f)
    d.rectangle([4, 44, 88, 58],   fill='black')
    d.text((8, 45), "FILLED", fill='white', font=f)
    d.rectangle([4, 63, 88, 77],   outline='black', width=2)
    d.text((8, 64), "BORDER", fill='black', font=f)
    for cy in range(5):
        for cx in range(10):
            if (cx + cy) % 2 == 0:
                d.rectangle([4+cx*8, 83+cy*6, 11+cx*8, 88+cy*6], fill='black')
    d.text((4, 116), "1px CHK", fill='black', font=f)
    d.line([(98, 13), (98, 127)], fill='black', width=1)
    d.rectangle([99, 13, 196, 127], fill='black')
    d.text((103, 16), "WHITE", fill='white', font=f)
    d.text((103, 30), "Hello!", fill='white', font=f)
    d.rectangle([103, 44, 187, 58],  fill='white')
    d.text((107, 45), "FILLED", fill='black', font=f)
    d.rectangle([103, 63, 187, 77], outline='white', width=2)
    d.text((107, 64), "BORDER", fill='white', font=f)
    for cy in range(5):
        for cx in range(10):
            if (cx + cy) % 2 == 0:
                d.rectangle([103+cx*8, 83+cy*6, 110+cx*8, 88+cy*6], fill='white')
    d.text((103, 116), "1px CHK", fill='white', font=f)
    d.line([(197, 13), (197, 127)], fill='black', width=1)
    RED = (200, 0, 0)
    d.text((202, 16), "RED",    fill=RED, font=f)
    d.text((202, 30), "Hello!", fill=RED, font=f)
    d.rectangle([202, 44, 286, 58],  fill=RED)
    d.text((206, 45), "FILLED", fill='white', font=f)
    d.rectangle([202, 63, 286, 77], outline=RED, width=2)
    d.text((206, 64), "BORDER", fill=RED, font=f)
    d.ellipse([215, 83, 271, 115], fill=RED)
    d.text((202, 116), "R CIRCLE", fill=RED, font=f)
    return img


def _pattern_resolution():
    img = Image.new('RGB', (296, 128), 'white')
    d, f = ImageDraw.Draw(img), _make_font()
    RED = (200, 0, 0)
    d.rectangle([0, 0, 295, 12], fill='black')
    d.text((4, 1), "RESOLUTION & DITHER TEST — SSD1675A", fill='white', font=f)
    y0 = 14
    for y in range(y0, y0+50, 2):
        d.line([(0, y), (73, y)], fill='black', width=1)
    d.text((2, y0+52), "1px H-lines", fill='black', font=f)
    for x in range(74, 148, 2):
        d.line([(x, y0), (x, y0+50)], fill='black', width=1)
    d.text((76, y0+52), "1px V-lines", fill='black', font=f)
    for cy in range(25):
        for cx in range(37):
            fill = 'black' if (cx + cy) % 2 == 0 else 'white'
            d.rectangle([148+cx*2, y0+cy*2, 149+cx*2, y0+cy*2+1], fill=fill)
    d.text((150, y0+52), "2×2 checker", fill='black', font=f)
    for cy in range(25):
        for cx in range(37):
            if (cx * 3 + cy) % max(1, 37 - cx) == 0:
                d.point((222+cx*2,   y0+cy*2),   fill=RED)
                d.point((222+cx*2+1, y0+cy*2),   fill=RED)
                d.point((222+cx*2,   y0+cy*2+1), fill=RED)
                d.point((222+cx*2+1, y0+cy*2+1), fill=RED)
    d.text((224, y0+52), "red dither", fill=RED, font=f)
    step_w = 37
    for step in range(8):
        density = step / 7.0
        period  = max(1, int(8 * (1 - density) + 1))
        x0 = step * step_w
        for dy in range(36):
            for dx in range(step_w):
                if (dx + dy) % period == 0:
                    d.point((x0+dx, 80+dy), fill='black')
        d.text((x0+1, 118), f"{int(density*100)}%", fill='black', font=f)
    d.line([(0, 79), (295, 79)], fill='black', width=1)
    return img


# ══════════════════════════════════════════════════════════════════════════

def vs_voltage(v):
    # VSS=0V, VSH1=+15V, VSL=-15V, VSH2=+5V (=+1/3 of VSH1 for DC weighting)
    return {0: 0, 1: 1, 2: -1, 3: 1/3}[v & 3]

def vs_color(v):
    return [C['vss'], C['vgh'], C['vgl'], C['vsh2']][v & 3]

def vs_name(v):
    return ['VSS', 'VSH1', 'VSL', 'VSH2'][v & 3]


def decode_waveform(lut: bytearray, group: int):
    # Bit order confirmed: A=bits[7:6], B=bits[5:4], C=bits[3:2], D=bits[1:0]
    # RP semantics confirmed: RP=0 → 1 pass (NOT skipped), total runs = RP+1
    segments = []
    for phase in range(PHASES):
        vs_byte = lut[group * 7 + phase]
        t_base  = 35 + phase * 5
        tp = [lut[t_base], lut[t_base+1], lut[t_base+2], lut[t_base+3]]
        rp = lut[t_base + 4]
        for _ in range(rp + 1):
            for slot in range(4):
                v = (vs_byte >> ((3 - slot) * 2)) & 0x3
                d = tp[slot]
                if d > 0:
                    segments.append((vs_voltage(v), d, v))
    return segments


def dc_balance(lut: bytearray, group: int):
    # Float balance: VSH1=+1, VSL=-1, VSH2=+1/3, VSS=0 (weighted by voltage ratio)
    pos = neg = balance = 0.0
    for (v_norm, d, _) in decode_waveform(lut, group):
        contrib = v_norm * d
        balance += contrib
        if contrib > 0:
            pos += contrib
        elif contrib < 0:
            neg += -contrib
    return pos, neg, balance


# ══════════════════════════════════════════════════════════════════════════
class App:

    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("SSD1675A LUT Tester + Device Controller")
        root.geometry("1280x820")
        root.configure(bg=C['bg'])
        root.resizable(True, True)

        self.lut           = bytearray(FACTORY_LUT)
        self.byte_vars     = [tk.StringVar() for _ in range(LUT_SIZE)]
        self.status_var    = tk.StringVar(value="Disconnected")
        self.target_name_var = tk.StringVar(value=DEVICE_NAMES[0])
        self.text_cmd_var  = tk.StringVar()
        self.mode_var      = tk.StringVar(value="1-Bal")
        self.rot_var       = tk.StringVar(value="1")

        # Telemetry state updated whenever the device sends TELE: / STAT: lines
        self._tele = {
            'fapply_ms': None, 'fast_ms': None, 'full_ms': None,
            'ltest_last': None, 'ltest_min': None, 'ltest_max': None,
            'ltest_frame': None, 'lut': None,
        }
        # ms-per-tick calibration (updated when we have both a timing measurement
        # and the expected tick count for the LUT that produced it)
        self._ms_per_tick = None

        # Phase activity indicator widgets: [phase][group] → tk.Frame dot
        self._phase_indicators = []

        # LUT history: list of dicts {lut, ticks, time_ms, rating, note}
        self._history = []

        # LGET receive buffer: 7 slots, each holds 20 hex chars (10 bytes)
        self._lget_buf = [None] * 7

        # Sweep state
        self._sweep_running  = False
        self._sweep_results  = []  # list of (byte_val, time_ms)

        self.client    = None
        self.connected = False
        self._ble_loop = asyncio.new_event_loop()
        threading.Thread(target=self._ble_thread, daemon=True).start()

        self._build_ui()
        self._lut_to_vars()
        self._refresh_waveform()

    # ── BLE thread ────────────────────────────────────────────────────────

    def _ble_thread(self):
        asyncio.set_event_loop(self._ble_loop)
        self._ble_loop.run_forever()

    def _ble_run(self, coro):
        asyncio.run_coroutine_threadsafe(coro, self._ble_loop)

    async def _ble_connect(self):
        target = self.target_name_var.get().strip()
        self._set_status(f"Scanning for '{target}'…")
        self._log_info(f"Scanning for '{target}'…")
        try:
            device = await BleakScanner.find_device_by_name(target, timeout=8.0)
            if device is None:
                self._set_status(f"'{target}' not found")
                self._log_info("Device not found")
                return
            self.client = BleakClient(device, disconnected_callback=self._on_disconnected)
            await self.client.connect()
            await self.client.start_notify(NUS_TX_UUID, self._on_notify)
            self.connected = True
            mtu = getattr(self.client, 'mtu_size', '?')
            self._set_status(f"Connected → {device.address}")
            self._log_info(f"Connected  MTU={mtu}  max_write={max(20, mtu-3) if isinstance(mtu, int) else '?'}B")
            self.root.after(0, lambda: self.btn_connect.config(text="Disconnect"))
            # Switch device to machine-readable mode automatically
            await asyncio.sleep(0.3)
            await self._ble_send_bytes(b"HOST:1\n")
            self._log_info("HOST:1 sent — telemetry mode active")
        except Exception as e:
            self._set_status(f"Error: {e}")
            self._log('!', str(e), 'err')

    async def _ble_disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()

    async def _ble_send_bytes(self, data: bytes):
        if not (self.client and self.client.is_connected):
            return
        chunk = max(20, getattr(self.client, 'mtu_size', 247) - 3)
        for i in range(0, len(data), chunk):
            await self.client.write_gatt_char(NUS_RX_UUID, data[i:i+chunk], response=False)

    def _on_disconnected(self, _client):
        self.connected = False
        self._set_status("Disconnected")
        self._log_info("Disconnected")
        self.root.after(0, lambda: self.btn_connect.config(text="Connect"))

    def _on_notify(self, _handle, data: bytearray):
        msg = data.decode('utf-8', errors='replace')
        for line in msg.splitlines():
            line = line.strip()
            if not line:
                continue
            self._log_rx(line)
            if line.startswith('TELE:') or line.startswith('STAT:'):
                self._parse_tele(line)
            elif line.startswith('LUT:') and len(line) > 6 and line[5] == ':':
                self._parse_lget_line(line)

    # ── Telemetry ─────────────────────────────────────────────────────────

    def _calc_lut_ticks(self, lut=None):
        """Total frames: sum((TA+TB+TC+TD)*(RP+1)) over all 7 phases. RP=0 → 1 pass."""
        b = lut if lut is not None else self.lut
        total = 0
        for ph in range(7):
            base = 35 + ph * 5
            ta, tb, tc, td, rp = b[base], b[base+1], b[base+2], b[base+3], b[base+4]
            total += (ta + tb + tc + td) * (rp + 1)
        return total

    def _parse_tele(self, line: str):
        """Parse a TELE: or STAT: line and update self._tele."""
        # key=value pairs separated by spaces, prefix is TELE:xxx or STAT:xxx
        parts = line.split()
        kind  = parts[0]  # e.g. "TELE:fast" or "STAT:lut=custom"

        kv = {}
        for p in parts[1:]:
            if '=' in p:
                k, v = p.split('=', 1)
                kv[k] = v
        # Also parse the first token's key=value if it starts with STAT:k=v
        if ':' in kind:
            sub = kind.split(':', 1)[1]
            if '=' in sub:
                k, v = sub.split('=', 1)
                kv[k] = v
            else:
                kv['_kind'] = sub

        kind_tag = kv.get('_kind', kind.split(':')[1] if ':' in kind else '')

        def _ms(key):
            v = kv.get(key, '')
            return int(v.rstrip('ms')) if v.rstrip('ms').isdigit() else None

        time_ms = _ms('time')

        if kind_tag == 'fast' and time_ms is not None:
            self._tele['fast_ms'] = time_ms
            self._calibrate(time_ms)
        elif kind_tag == 'full' and time_ms is not None:
            self._tele['full_ms'] = time_ms
        elif kind_tag == 'fapply' and time_ms is not None:
            self._tele['fapply_ms'] = time_ms
            self._calibrate(time_ms)
        elif kind_tag == 'ltest':
            self._tele['ltest_last']  = _ms('last')
            self._tele['ltest_min']   = _ms('min')
            self._tele['ltest_max']   = _ms('max')
            if 'frame' in kv:
                try: self._tele['ltest_frame'] = int(kv['frame'])
                except ValueError: pass

        if 'lut' in kv:
            self._tele['lut'] = kv['lut']

        # STAT line also carries frame/last/min/max
        if 'last' in kv:
            try: self._tele['ltest_last'] = int(kv['last'])
            except ValueError: pass
        if 'min' in kv:
            try: self._tele['ltest_min'] = int(kv['min'])
            except ValueError: pass
        if 'max' in kv:
            try: self._tele['ltest_max'] = int(kv['max'])
            except ValueError: pass
        if 'frame' in kv:
            try: self._tele['ltest_frame'] = int(kv['frame'])
            except ValueError: pass

        self.root.after(0, self._refresh_tele_bar)

        # Sweep: if a sweep is running, record this measurement
        if self._sweep_running and (kind_tag in ('fast', 'fapply')) and time_ms is not None:
            self.root.after(0, lambda t=time_ms: self._sweep_record(t))

    def _parse_lget_line(self, line: str):
        """Handle LUT:N: reply chunks from LGET command (7 chunks × 10 bytes each).
        Format: 'LUT:N:XXXXXXXXXXXXXXXXXXXX'  where N is 0-6."""
        if len(line) < 26 or line[3] != ':' or line[5] != ':':
            return
        try:
            idx = int(line[4])
        except ValueError:
            return
        if idx < 0 or idx >= 7:
            return
        self._lget_buf[idx] = line[6:26].strip()  # exactly 20 hex chars
        if any(s is None for s in self._lget_buf):
            return
        hex_str = ''.join(self._lget_buf)
        self._lget_buf = [None] * 7
        if len(hex_str) != LUT_SIZE * 2:
            self._log_info(f"[LGET] unexpected length {len(hex_str)}, expected {LUT_SIZE * 2}")
            return
        try:
            lut = bytearray(int(hex_str[i*2:i*2+2], 16) for i in range(LUT_SIZE))
        except ValueError as e:
            self._log_info(f"[LGET] hex parse error: {e}")
            return
        self.root.after(0, lambda b=lut: self._apply_lget(b))

    def _apply_lget(self, lut: bytearray):
        """Load a LUT received from the device into the editor (main thread)."""
        self.lut[:] = lut
        self._lut_to_vars()
        self._refresh_waveform()
        self._log_info("[LGET] LUT loaded from device")

    def _calibrate(self, actual_ms: int):
        """Update ms/tick estimate when we get a measurement."""
        ticks = self._calc_lut_ticks()
        if ticks > 0:
            self._ms_per_tick = actual_ms / ticks
            self.root.after(0, self._refresh_explorer)

    # ── UI ────────────────────────────────────────────────────────────────

    def _build_ui(self):
        top = tk.Frame(self.root, bg=C['bg2'], pady=6)
        top.pack(fill='x', side='top')
        self._build_top_bar(top)

        dev = tk.Frame(self.root, bg=C['bg3'], pady=4)
        dev.pack(fill='x', side='top')
        self._build_device_bar(dev)

        tele = tk.Frame(self.root, bg='#1a1a2e', pady=3)
        tele.pack(fill='x', side='top')
        self._build_tele_bar(tele)

        main = tk.Frame(self.root, bg=C['bg'])
        main.pack(fill='both', expand=True, padx=8, pady=(4, 0))

        left = tk.Frame(main, bg=C['bg'])
        left.pack(side='left', fill='y', padx=(0, 6))
        self._build_editor(left)

        right = tk.Frame(main, bg=C['bg'])
        right.pack(side='left', fill='both', expand=True)
        self._build_waveform_panel(right)
        self._build_dc_panel(right)
        self._build_explorer_panel(right)

        bot = tk.Frame(self.root, bg=C['bg2'])
        bot.pack(fill='x', side='bottom')
        self._build_console(bot)

    def _build_top_bar(self, parent):
        if not BLEAK_OK:
            tk.Label(parent, text="⚠ bleak not installed — pip install bleak",
                     bg=C['bg2'], fg=C['red'], font=('Menlo', 11)).pack(side='left', padx=10)

        # Device name selector
        tk.Label(parent, text="Device:", bg=C['bg2'], fg=C['fg2'],
                 font=('Menlo', 11)).pack(side='left', padx=(8, 2))
        dev_menu = tk.OptionMenu(parent, self.target_name_var, *DEVICE_NAMES)
        dev_menu.config(bg=C['bg3'], fg=C['fg'], activebackground=C['bg3'],
                        activeforeground=C['accent'], relief='flat',
                        font=('Menlo', 11), width=22, highlightthickness=0)
        dev_menu['menu'].config(bg=C['bg3'], fg=C['fg'], font=('Menlo', 11))
        dev_menu.pack(side='left', padx=(0, 4))

        self.btn_connect = tk.Button(parent, text="Connect",
            bg=C['bg3'], fg=C['accent'], relief='flat', padx=12, pady=4,
            font=('Menlo', 12, 'bold'), command=self._on_connect_click)
        self.btn_connect.pack(side='left', padx=8)

        tk.Label(parent, text="│", bg=C['bg2'], fg=C['border'],
                 font=('Menlo', 14)).pack(side='left', padx=4)

        tk.Button(parent, text="APPLY", bg='#2d4f2d', fg=C['green'],
            relief='flat', padx=12, pady=4, font=('Menlo', 12, 'bold'),
            command=self._on_apply).pack(side='left', padx=4)

        tk.Button(parent, text="CLEAR", bg=C['bg3'], fg=C['fg'],
            relief='flat', padx=12, pady=4, font=('Menlo', 11),
            command=self._on_clear).pack(side='left', padx=4)

        tk.Button(parent, text="Factory", bg=C['bg3'], fg=C['fg2'],
            relief='flat', padx=10, pady=4, font=('Menlo', 11),
            command=self._on_factory).pack(side='left', padx=4)

        tk.Button(parent, text="↓ GET LUT", bg='#2d3848', fg=C['accent'],
            relief='flat', padx=10, pady=4, font=('Menlo', 11, 'bold'),
            command=self._on_get_lut,
            activebackground='#3a4a58', activeforeground=C['fg']
            ).pack(side='left', padx=4)

        tk.Button(parent, text="Import hex…", bg=C['bg3'], fg=C['fg2'],
            relief='flat', padx=10, pady=4, font=('Menlo', 11),
            command=self._on_import).pack(side='left', padx=4)

        tk.Button(parent, text="Export hex…", bg=C['bg3'], fg=C['fg2'],
            relief='flat', padx=10, pady=4, font=('Menlo', 11),
            command=self._on_export).pack(side='left', padx=4)

        tk.Button(parent, text="Send Frame…", bg='#2d2d4f', fg=C['accent'],
            relief='flat', padx=10, pady=4, font=('Menlo', 11),
            command=self._on_send_frame).pack(side='left', padx=4)

        tk.Label(parent, text="│", bg=C['bg2'], fg=C['border'],
                 font=('Menlo', 14)).pack(side='left', padx=4)
        for n, label in ((1, "Bands"), (2, "Text"), (3, "Grid")):
            tk.Button(parent, text=f"P{n}:{label}", bg='#2d3d2d', fg=C['green'],
                relief='flat', padx=8, pady=4, font=('Menlo', 11),
                command=lambda n=n: self._on_test_pattern(n)).pack(side='left', padx=2)

        tk.Button(parent, text="Paste LUT…", bg='#3d2d4f', fg='#c678dd',
            relief='flat', padx=10, pady=4, font=('Menlo', 11),
            command=self._on_paste_dialog,
            activebackground='#4a3a5a', activeforeground=C['fg']
            ).pack(side='left', padx=(8, 4))

    def _build_device_bar(self, parent):
        """Device controls split across two compact rows."""

        def row_frame(bg=C['bg3']):
            f = tk.Frame(parent, bg=bg, pady=3)
            f.pack(fill='x')
            return f

        def btn(parent, text, cmd, fg=None, bg=None, bold=False):
            font = ('Menlo', 10, 'bold') if bold else ('Menlo', 10)
            b = tk.Button(parent, text=text,
                bg=bg or C['bg'], fg=fg or C['fg2'], relief='flat',
                padx=7, pady=2, font=font, command=cmd)
            b.pack(side='left', padx=1)
            return b

        def sep(parent):
            tk.Label(parent, text="│", bg=parent.cget('bg'), fg=C['border'],
                     font=('Menlo', 12)).pack(side='left', padx=3)

        def lbl(parent, text, fg=C['fg2']):
            tk.Label(parent, text=text, bg=parent.cget('bg'), fg=fg,
                     font=('Menlo', 10)).pack(side='left', padx=(4, 1))

        # ── Row 1: display & screensaver controls ─────────────────────────
        r1 = row_frame(C['bg3'])

        lbl(r1, "Display:", C['yellow'])
        btn(r1, "TIME",   self._on_dev_time,   fg=C['accent'])
        btn(r1, "BATT",   self._on_dev_batt,   fg=C['green'])

        sep(r1)

        btn(r1, "SAVER",  self._on_dev_saver)
        btn(r1, "SS:ON",  lambda: self._on_dev_ss(1), fg=C['green'])
        btn(r1, "SS:OFF", lambda: self._on_dev_ss(0), fg=C['red'])

        sep(r1)

        btn(r1, "UPDATE", self._on_dev_update, fg=C['accent'], bold=True)
        btn(r1, "FAST",   self._on_dev_fast)
        btn(r1, "CLEAN",  self._on_dev_clean)
        btn(r1, "FAPPLY", self._on_dev_fapply, fg=C['green'], bold=True)

        sep(r1)

        lbl(r1, "Mode:")
        mode_menu = tk.OptionMenu(r1, self.mode_var,
                                  "0-Turbo", "1-Bal", "2-Stab",
                                  command=self._on_dev_mode)
        mode_menu.config(bg=C['bg'], fg=C['fg2'], activebackground=C['bg3'],
                         activeforeground=C['fg'], relief='flat',
                         font=('Menlo', 10), width=7, highlightthickness=0)
        mode_menu['menu'].config(bg=C['bg3'], fg=C['fg'], font=('Menlo', 10))
        mode_menu.pack(side='left', padx=(0, 2))

        lbl(r1, "Rot:")
        rot_menu = tk.OptionMenu(r1, self.rot_var, "0", "1", "2", "3",
                                 command=self._on_dev_rot)
        rot_menu.config(bg=C['bg'], fg=C['fg2'], activebackground=C['bg3'],
                        activeforeground=C['fg'], relief='flat',
                        font=('Menlo', 10), width=2, highlightthickness=0)
        rot_menu['menu'].config(bg=C['bg3'], fg=C['fg'], font=('Menlo', 10))
        rot_menu.pack(side='left', padx=(0, 2))

        # ── Row 2: animation, LUT test & text ────────────────────────────
        r2 = row_frame('#282828')

        lbl(r2, "Anim:", C['yellow'])
        btn(r2, "DSAVER:ON",  lambda: self._on_dev_dsaver(1), fg=C['green'])
        btn(r2, "DSAVER:OFF", lambda: self._on_dev_dsaver(0))
        btn(r2, "ANIM",       self._on_dev_anim, fg=C['orange'])

        sep(r2)

        lbl(r2, "LUT test:", C['yellow'])
        btn(r2, "LTEST",      self._on_dev_ltest,
            fg=C['yellow'], bold=True)
        btn(r2, "LTEST:OFF",  lambda: self._send_cmd("LTEST 0\n", "LTEST 0"))
        btn(r2, "LUTUSE:ON",  lambda: self._on_dev_lutuse(1), fg=C['green'])
        btn(r2, "LUTUSE:OFF", lambda: self._on_dev_lutuse(0), fg=C['red'])

        sep(r2)

        lbl(r2, "TEXT:")
        te = tk.Entry(r2, textvariable=self.text_cmd_var, width=22,
                      bg=C['bg'], fg=C['fg'], insertbackground=C['fg'],
                      relief='flat', font=('Menlo', 10))
        te.pack(side='left', padx=(0, 2))
        te.bind('<Return>', lambda _: self._on_dev_text())
        btn(r2, "Send", self._on_dev_text, fg=C['accent'])

    # ── LUT editor ────────────────────────────────────────────────────────

    def _build_editor(self, parent):
        hdr_row = tk.Frame(parent, bg=C['bg'])
        hdr_row.pack(fill='x', pady=(0, 4))
        tk.Label(hdr_row, text="LUT BYTES", bg=C['bg'],
                 fg=C['accent'], font=('Menlo', 12, 'bold')).pack(side='left')
        tk.Button(hdr_row, text=" ? ", command=self._on_lut_help,
                  bg=C['bg3'], fg=C['yellow'], font=('Menlo', 10, 'bold'),
                  relief='flat', cursor='hand2', padx=4, pady=0,
                  activebackground=C['border'], activeforeground=C['yellow']
                  ).pack(side='left', padx=(8, 0))

        canvas = tk.Canvas(parent, bg=C['bg'], highlightthickness=0, width=420)
        scroll = tk.Scrollbar(parent, orient='vertical', command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)

        frame = tk.Frame(canvas, bg=C['bg'])
        canvas.create_window((0, 0), window=frame, anchor='nw')
        frame.bind('<Configure>', lambda e: canvas.configure(
            scrollregion=canvas.bbox('all')))

        self.byte_entries = []
        self._build_vs_section(frame)
        self._build_timing_section(frame)

    def _section_label(self, parent, text):
        tk.Label(parent, text=text, bg=C['bg'], fg=C['yellow'],
                 font=('Menlo', 10, 'bold')).grid(
            row=0, column=0, columnspan=8, sticky='w', pady=(6, 2))

    def _byte_entry(self, parent, row, col, idx):
        v = self.byte_vars[idx]
        e = tk.Entry(parent, textvariable=v, width=3,
                     bg=C['bg3'], fg=C['fg'], insertbackground=C['fg'],
                     relief='flat', font=('Menlo', 11), justify='center',
                     highlightthickness=1, highlightcolor=C['accent'],
                     highlightbackground=C['border'])
        e.grid(row=row, column=col, padx=2, pady=1)
        v.trace_add('write', lambda *_: self._on_var_change(idx))
        self.byte_entries.append(e)
        return e

    def _build_vs_section(self, parent):
        f = tk.Frame(parent, bg=C['bg'])
        f.pack(fill='x', pady=(4, 0))
        hdr = tk.Label(f, text="VS (voltage waveform)  bytes 0-34",
                       bg=C['bg'], fg=C['yellow'], font=('Menlo', 10, 'bold'))
        hdr.grid(row=0, column=0, columnspan=9, sticky='w', pady=(0, 2))
        Tooltip(hdr,
                "VS (Voltage Source) section — bytes 0-34\n"
                "5 groups × 7 phases = 35 bytes.\n"
                "Each byte encodes 4 × 2-bit voltage slots (A=bits[7:6], B=[5:4], C=[3:2], D=[1:0]):\n"
                "  00=VSS (0V, neutral)  01=VSH1 (+15V, black drive)\n"
                "  10=VSL (−15V, erase)  11=VSH2 (+5V, red reveal)\n"
                "Groups: LUT0=BLACK, LUT1=WHITE, LUT2/3=RED, LUT4=VCOM (must be 0!)")
        for col, txt in enumerate([''] + [str(c) for c in range(7)]):
            lbl = tk.Label(f, text=txt, bg=C['bg'], fg=C['fg2'],
                           font=('Menlo', 9), width=3)
            lbl.grid(row=1, column=col)
            if txt:
                Tooltip(lbl, f"Phase {txt} column.\n"
                        f"Timing for this phase is set by row Ph {txt} in the Timing section.\n"
                        "The byte value encodes 4 voltage slots (2 bits each).")
        for g in range(VS_GROUPS):
            row = g + 2
            lbl = tk.Label(f, text=GROUP_LABELS[g], bg=C['bg'],
                           fg=GROUP_COLORS[g], font=('Menlo', 9), width=12,
                           anchor='w')
            lbl.grid(row=row, column=0, sticky='w')
            Tooltip(lbl, _GROUP_TIPS[g])
            for col in range(7):
                self._byte_entry(f, row, col + 1, g * 7 + col)

    def _build_timing_section(self, parent):
        f = tk.Frame(parent, bg=C['bg'])
        f.pack(fill='x', pady=(10, 0))
        hdr = tk.Label(f, text="Timing (phase blocks)  bytes 35-69",
                       bg=C['bg'], fg=C['yellow'], font=('Menlo', 10, 'bold'))
        hdr.grid(row=0, column=0, columnspan=8, sticky='w', pady=(0, 2))
        Tooltip(hdr,
                "Timing section — bytes 35-69\n"
                "Defines HOW LONG each voltage slot is applied. 7 phases × 5 bytes.\n"
                "ALL 5 LUT rows share the SAME timing — phases are global.\n"
                "The 'Active?' column shows which LUT rows have a non-zero VS byte\n"
                "in that phase (i.e. actually do something during that phase).\n"
                "Total time ≈ Σ (TP_A+TP_B+TP_C+TP_D)×(RP+1) × 8ms + 550ms")
        col_headers = ['Phase', 'TP_A', 'TP_B', 'TP_C', 'TP_D', 'RP']
        for col, txt in enumerate(col_headers):
            lbl = tk.Label(f, text=txt, bg=C['bg'], fg=C['fg2'],
                           font=('Menlo', 9), width=5)
            lbl.grid(row=1, column=col)
            if col > 0:
                Tooltip(lbl, _TIMING_COL_TIPS[col - 1])
        # "Active?" column header — dots showing which LUT rows are active per phase
        act_hdr = tk.Label(f, text="Active?", bg=C['bg'], fg=C['fg2'],
                           font=('Menlo', 9))
        act_hdr.grid(row=1, column=6, padx=(8, 0), sticky='w')
        Tooltip(act_hdr,
                "Colored dot = this LUT row has a non-zero VS byte in this phase\n"
                "(i.e. it actually applies a voltage during this phase block).\n"
                "Grey dot = VS byte is 0x00 → row is idle this phase.\n\n"
                + "  ".join(f"{'▮'} {GROUP_LABELS[g]}" for g in range(VS_GROUPS)))

        self._phase_indicators = []
        for ph in range(PHASES):
            row = ph + 2
            ph_lbl = tk.Label(f, text=f"Ph {ph}", bg=C['bg'], fg=C['fg2'],
                              font=('Menlo', 9), width=5)
            ph_lbl.grid(row=row, column=0)
            Tooltip(ph_lbl, _PHASE_TIPS[ph])
            for sub in range(5):
                idx = 35 + ph * 5 + sub
                e = self._byte_entry(f, row, sub + 1, idx)
                e.config(fg=C['orange'] if sub < 4 else C['green'])

            # Activity indicator: one colored square per LUT group
            dot_frame = tk.Frame(f, bg=C['bg'])
            dot_frame.grid(row=row, column=6, padx=(8, 0), sticky='w')
            ph_dots = []
            for g in range(VS_GROUPS):
                dot = tk.Label(dot_frame, text='▮', bg=C['bg'],
                               fg=C['bg3'], font=('Menlo', 10))
                dot.pack(side='left', padx=0)
                Tooltip(dot, f"{GROUP_LABELS[g]}\n"
                             f"Phase {ph} VS byte = lut[{g*7+ph}]\n"
                             "Lit = non-zero (active this phase)\n"
                             "Grey = 0x00 (idle this phase)")
                ph_dots.append(dot)
            self._phase_indicators.append(ph_dots)

    def _build_waveform_panel(self, parent):
        tk.Label(parent, text="WAVEFORM", bg=C['bg'],
                 fg=C['accent'], font=('Menlo', 12, 'bold')).pack(anchor='w')
        self.wf_canvas = tk.Canvas(parent, bg=C['bg2'], height=295,
                                   highlightthickness=1,
                                   highlightbackground=C['border'])
        self.wf_canvas.pack(fill='x', pady=(2, 4))
        self.wf_canvas.bind('<Configure>', lambda e: self._refresh_waveform())

    def _build_dc_panel(self, parent):
        tk.Label(parent, text="DC BALANCE  (pos ticks − neg ticks per group)",
                 bg=C['bg'], fg=C['accent'], font=('Menlo', 11, 'bold')).pack(anchor='w', pady=(4, 2))
        self.dc_frame = tk.Frame(parent, bg=C['bg'])
        self.dc_frame.pack(fill='x')
        self.dc_bars = []
        for g in range(VS_GROUPS):
            row = tk.Frame(self.dc_frame, bg=C['bg'])
            row.pack(fill='x', pady=1)
            tk.Label(row, text=GROUP_LABELS[g], width=12, anchor='w',
                     bg=C['bg'], fg=GROUP_COLORS[g],
                     font=('Menlo', 10)).pack(side='left')
            bar_bg = tk.Frame(row, bg=C['bg3'], height=16, width=300)
            bar_bg.pack(side='left', padx=4)
            bar_bg.pack_propagate(False)
            bar = tk.Frame(bar_bg, bg=C['vgh'], height=16)
            bar.place(x=150, y=0, width=0, height=16)
            val_lbl = tk.Label(row, text="", bg=C['bg'], fg=C['fg2'],
                               font=('Menlo', 10), width=14)
            val_lbl.pack(side='left')
            self.dc_bars.append((bar, bar_bg, val_lbl))

    def _build_tele_bar(self, parent):
        """Compact telemetry strip: live timing + calibration."""
        BG = '#1a1a2e'

        def tlbl(text, fg=C['fg2'], bold=False):
            font = ('Menlo', 10, 'bold') if bold else ('Menlo', 10)
            return tk.Label(parent, text=text, bg=BG, fg=fg, font=font)

        tlbl("Telemetry:", C['yellow'], bold=True).pack(side='left', padx=(8, 6))

        self._tele_labels = {}
        specs = [
            ('fapply',  'FAPPLY',  C['green']),
            ('fast',    'FAST',    C['accent']),
            ('full',    'UPDATE',  C['fg2']),
            ('ltest',   'LTEST',   C['orange']),
            ('ticks',   'Frames',  C['yellow']),
            ('rate',    'Pred ms', C['vsh2']),
        ]
        for key, label, fg in specs:
            tlbl(f"{label}:", fg).pack(side='left', padx=(6, 1))
            lv = tk.StringVar(value="—")
            lbl = tk.Label(parent, textvariable=lv, bg=BG, fg=fg,
                           font=('Menlo', 10, 'bold'), width=9, anchor='w')
            lbl.pack(side='left', padx=(0, 4))
            self._tele_labels[key] = lv

        tk.Button(parent, text="STAT", bg='#252540', fg=C['fg2'],
                  relief='flat', padx=6, pady=1, font=('Menlo', 9),
                  command=lambda: self._send_cmd("STAT\n", "STAT")).pack(side='right', padx=8)

    def _refresh_tele_bar(self):
        t = self._tele
        def fmt(v): return f"{v}ms" if v is not None else "—"
        self._tele_labels['fapply'].set(fmt(t['fapply_ms']))
        self._tele_labels['fast'].set(fmt(t['fast_ms']))
        self._tele_labels['full'].set(fmt(t['full_ms']))
        ltest = "—"
        if t['ltest_last'] is not None:
            ltest = f"{t['ltest_last']}ms"
            if t['ltest_min'] is not None:
                ltest += f" [{t['ltest_min']}-{t['ltest_max']}]"
        self._tele_labels['ltest'].set(ltest)
        frames = self._calc_lut_ticks()
        self._tele_labels['ticks'].set(str(frames))
        # Predicted time: frames × 8.0ms + 550ms fixed overhead
        pred_ms = frames * 8.0 + 550
        self._tele_labels['rate'].set(f"~{pred_ms:.0f}")

    # ── LUT Explorer ──────────────────────────────────────────────────────

    def _build_explorer_panel(self, parent):
        """LUT Explorer: quick timing adjust + history + byte sweep."""
        tk.Label(parent, text="LUT EXPLORER", bg=C['bg'],
                 fg=C['accent'], font=('Menlo', 11, 'bold')).pack(anchor='w', pady=(8, 2))

        # ── Quick adjust row ────────────────────────────────────────────
        adj = tk.Frame(parent, bg=C['bg'])
        adj.pack(fill='x', pady=(0, 4))

        tk.Label(adj, text="Quick adjust:", bg=C['bg'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(0, 4))

        def abtn(text, fn, fg=C['fg2']):
            tk.Button(adj, text=text, bg=C['bg3'], fg=fg, relief='flat',
                      padx=6, pady=2, font=('Menlo', 10),
                      command=fn).pack(side='left', padx=1)

        abtn("TP−−", lambda: self._adjust_tp(-2), C['vgl'])
        abtn("TP−",  lambda: self._adjust_tp(-1), C['vgl'])
        abtn("TP+",  lambda: self._adjust_tp(+1), C['vgh'])
        abtn("TP++", lambda: self._adjust_tp(+2), C['vgh'])
        tk.Label(adj, text="  ", bg=C['bg']).pack(side='left')
        abtn("RP−",  lambda: self._adjust_rp(-1), C['vgl'])
        abtn("RP+",  lambda: self._adjust_rp(+1), C['vgh'])

        tk.Label(adj, text="  ", bg=C['bg']).pack(side='left')

        tk.Button(adj, text="Apply+LTEST", bg='#2d2d2d', fg=C['yellow'],
                  relief='flat', padx=8, pady=2, font=('Menlo', 10, 'bold'),
                  command=self._on_apply_ltest).pack(side='left', padx=2)
        tk.Button(adj, text="Apply+FAST", bg='#2d2d2d', fg=C['accent'],
                  relief='flat', padx=8, pady=2, font=('Menlo', 10),
                  command=self._on_apply_fast).pack(side='left', padx=2)

        # ── Calibration info row ────────────────────────────────────────
        cal = tk.Frame(parent, bg=C['bg'])
        cal.pack(fill='x', pady=(0, 4))
        self._explorer_info_var = tk.StringVar(value="Expected: — ticks   Actual: —   Rate: —")
        tk.Label(cal, textvariable=self._explorer_info_var,
                 bg=C['bg'], fg=C['fg2'], font=('Menlo', 9)).pack(side='left')

        # ── Byte sweep row ──────────────────────────────────────────────
        sw = tk.Frame(parent, bg=C['bg'])
        sw.pack(fill='x', pady=(0, 6))

        tk.Label(sw, text="Sweep byte:", bg=C['bg'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(0, 3))
        self._sweep_byte_var = tk.StringVar(value="57")
        tk.Entry(sw, textvariable=self._sweep_byte_var, width=4,
                 bg=C['bg3'], fg=C['fg'], relief='flat',
                 font=('Menlo', 10)).pack(side='left', padx=2)

        tk.Label(sw, text="from", bg=C['bg'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(4, 2))
        self._sweep_from_var = tk.StringVar(value="0")
        tk.Entry(sw, textvariable=self._sweep_from_var, width=4,
                 bg=C['bg3'], fg=C['fg'], relief='flat',
                 font=('Menlo', 10)).pack(side='left', padx=2)

        tk.Label(sw, text="to", bg=C['bg'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(2, 2))
        self._sweep_to_var = tk.StringVar(value="20")
        tk.Entry(sw, textvariable=self._sweep_to_var, width=4,
                 bg=C['bg3'], fg=C['fg'], relief='flat',
                 font=('Menlo', 10)).pack(side='left', padx=2)

        tk.Label(sw, text="step", bg=C['bg'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(4, 2))
        self._sweep_step_var = tk.StringVar(value="1")
        tk.Entry(sw, textvariable=self._sweep_step_var, width=3,
                 bg=C['bg3'], fg=C['fg'], relief='flat',
                 font=('Menlo', 10)).pack(side='left', padx=2)

        self._sweep_btn = tk.Button(sw, text="▶ Sweep",
                 bg='#2d3520', fg=C['green'], relief='flat',
                 padx=8, pady=2, font=('Menlo', 10, 'bold'),
                 command=self._on_sweep_toggle)
        self._sweep_btn.pack(side='left', padx=6)

        self._sweep_progress_var = tk.StringVar(value="")
        tk.Label(sw, textvariable=self._sweep_progress_var,
                 bg=C['bg'], fg=C['fg2'], font=('Menlo', 9)).pack(side='left', padx=4)

        # ── History ─────────────────────────────────────────────────────
        tk.Label(parent, text="LUT HISTORY", bg=C['bg'],
                 fg=C['accent'], font=('Menlo', 10, 'bold')).pack(anchor='w', pady=(4, 2))

        hist_frame = tk.Frame(parent, bg=C['bg'])
        hist_frame.pack(fill='x')

        cols = ('idx', 'time', 'ticks', 'rating', 'preview')
        self._hist_tree = ttk.Treeview(hist_frame, columns=cols,
                                       show='headings', height=5)
        self._hist_tree.heading('idx',     text='#')
        self._hist_tree.heading('time',    text='Time')
        self._hist_tree.heading('ticks',   text='Ticks')
        self._hist_tree.heading('rating',  text='★')
        self._hist_tree.heading('preview', text='LUT[0..6]')
        self._hist_tree.column('idx',     width=28,  anchor='center')
        self._hist_tree.column('time',    width=70,  anchor='center')
        self._hist_tree.column('ticks',   width=50,  anchor='center')
        self._hist_tree.column('rating',  width=50,  anchor='center')
        self._hist_tree.column('preview', width=240, anchor='w')
        style = ttk.Style()
        style.theme_use('default')
        style.configure('Treeview', background=C['bg3'], foreground=C['fg'],
                        fieldbackground=C['bg3'], rowheight=18, font=('Menlo', 9))
        style.configure('Treeview.Heading', background=C['bg2'],
                        foreground=C['fg2'], font=('Menlo', 9, 'bold'))
        self._hist_tree.pack(side='left', fill='x', expand=True)

        hs = tk.Scrollbar(hist_frame, orient='vertical',
                          command=self._hist_tree.yview)
        self._hist_tree.configure(yscrollcommand=hs.set)
        hs.pack(side='right', fill='y')

        # History action buttons
        hbtn = tk.Frame(parent, bg=C['bg'])
        hbtn.pack(fill='x', pady=(3, 0))

        def hb(text, cmd, fg=C['fg2'], bg=None):
            tk.Button(hbtn, text=text, bg=bg or C['bg3'], fg=fg, relief='flat',
                      padx=7, pady=2, font=('Menlo', 10),
                      command=cmd).pack(side='left', padx=2)

        hb("Save current", self._hist_save, fg=C['green'])
        hb("Load selected", self._hist_load, fg=C['accent'])
        hb("★★★", lambda: self._hist_rate(3))
        hb("★★★★", lambda: self._hist_rate(4))
        hb("★★★★★", lambda: self._hist_rate(5), fg=C['yellow'])
        hb("✗ Bad", lambda: self._hist_rate(1), fg=C['red'])
        hb("Delete", self._hist_delete, fg=C['red'])

    def _refresh_explorer(self):
        frames = self._calc_lut_ticks()
        best_ms = self._tele.get('fast_ms') or self._tele.get('fapply_ms')
        actual  = f"{best_ms}ms" if best_ms else "—"
        pred_ms = frames * 8.0 + 550
        self._explorer_info_var.set(
            f"Frames: {frames}   Predicted: ~{pred_ms:.0f}ms   Actual: {actual}")

    # ── Quick timing adjustments ─────────────────────────────────────────

    def _adjust_tp(self, delta: int):
        """Change all TP (timing pulse) bytes by delta, clamp 0–255."""
        for ph in range(7):
            base = 35 + ph * 5
            for sub in range(4):  # TP_A, TP_B, TP_C, TP_D
                idx = base + sub
                self.lut[idx] = max(0, min(255, self.lut[idx] + delta))
        self._lut_to_vars()
        self._refresh_waveform()

    def _adjust_rp(self, delta: int):
        """Change all RP (repeat) bytes by delta, clamp 0–255."""
        for ph in range(7):
            idx = 35 + ph * 5 + 4
            self.lut[idx] = max(0, min(255, self.lut[idx] + delta))
        self._lut_to_vars()
        self._refresh_waveform()

    def _on_apply_ltest(self):
        if not self._vars_to_lut():
            messagebox.showerror("Parse error", "Invalid hex value in LUT editor")
            return
        hex_str = self.lut.hex().upper()
        cmd = f"LUTW:{hex_str}\nLUTUSE:1\nLTEST\n".encode()
        if self.connected:
            self._ble_run(self._ble_send_bytes(cmd))
            self._log_tx(f"LUTW+LUTUSE:1+LTEST ({self._calc_lut_ticks()} ticks)")
        else:
            self._log_info("[offline] Apply+LTEST")

    def _on_apply_fast(self):
        if not self._vars_to_lut():
            messagebox.showerror("Parse error", "Invalid hex value in LUT editor")
            return
        hex_str = self.lut.hex().upper()
        cmd = f"LUTW:{hex_str}\nLUTUSE:1\nFAST\n".encode()
        if self.connected:
            self._ble_run(self._ble_send_bytes(cmd))
            self._log_tx(f"LUTW+LUTUSE:1+FAST ({self._calc_lut_ticks()} ticks)")
        else:
            self._log_info("[offline] Apply+FAST")

    # ── Byte sweep ────────────────────────────────────────────────────────

    def _on_sweep_toggle(self):
        if self._sweep_running:
            self._sweep_running = False
            self._sweep_btn.config(text="▶ Sweep", fg=C['green'])
            self._sweep_progress_var.set("stopped")
            return
        try:
            byte_idx = int(self._sweep_byte_var.get())
            frm      = int(self._sweep_from_var.get())
            to       = int(self._sweep_to_var.get())
            step     = max(1, int(self._sweep_step_var.get()))
        except ValueError:
            messagebox.showerror("Sweep", "Invalid sweep parameters")
            return
        if not (0 <= byte_idx < LUT_SIZE):
            messagebox.showerror("Sweep", f"Byte index must be 0–{LUT_SIZE-1}")
            return
        if not self.connected:
            messagebox.showwarning("Sweep", "Not connected")
            return

        self._sweep_running = True
        self._sweep_results = []
        self._sweep_pending_val = None
        self._sweep_btn.config(text="■ Stop", fg=C['red'])
        self._sweep_progress_var.set("starting…")
        self._ble_run(self._run_sweep(byte_idx, frm, to, step))

    async def _run_sweep(self, byte_idx, frm, to, step):
        values = list(range(frm, to + 1, step))
        orig_val = self.lut[byte_idx]

        for i, val in enumerate(values):
            if not self._sweep_running:
                break

            # Set the byte, send LUTW + FAST, wait for TELE response
            self.lut[byte_idx] = val
            self._sweep_pending_val = val
            hex_str = self.lut.hex().upper()
            await self._ble_send_bytes(
                f"LUTW:{hex_str}\nLUTUSE:1\nFAST\n".encode())

            pct = int((i + 1) / len(values) * 100)
            self.root.after(0, lambda p=pct, v=val, ii=i:
                self._sweep_progress_var.set(
                    f"[{ii+1}/{len(values)}]  byte={v}  {p}%"))

            # Wait for device to reply (TELE:fast will arrive via _parse_tele)
            await asyncio.sleep(1.5)  # allow display update + BLE reply

        # Restore original value if sweep completed or stopped
        self.lut[byte_idx] = orig_val
        self._sweep_running = False
        self.root.after(0, self._sweep_finish)

    def _sweep_record(self, time_ms: int):
        """Called from main thread when TELE reports a timing for current sweep step."""
        if self._sweep_pending_val is not None:
            self._sweep_results.append((self._sweep_pending_val, time_ms))
            self._sweep_pending_val = None
            self._log_info(f"  sweep: val={self._sweep_results[-1][0]}  "
                           f"time={time_ms}ms")

    def _sweep_finish(self):
        self._sweep_btn.config(text="▶ Sweep", fg=C['green'])
        if not self._sweep_results:
            self._sweep_progress_var.set("no results")
            return
        best_val, best_ms = min(self._sweep_results, key=lambda x: x[1])
        worst_val, worst_ms = max(self._sweep_results, key=lambda x: x[1])
        self._sweep_progress_var.set(
            f"done — fastest: val={best_val} {best_ms}ms  "
            f"slowest: val={worst_val} {worst_ms}ms")
        self._log_info(f"Sweep done: {len(self._sweep_results)} points  "
                       f"best={best_val}→{best_ms}ms  worst={worst_val}→{worst_ms}ms")
        # Auto-save each result to history
        for val, ms in self._sweep_results:
            self._history.append({
                'lut': bytes(self.lut),
                'ticks': self._calc_lut_ticks(),
                'time_ms': ms,
                'rating': 0,
                'note': f"sweep byte={val}",
            })
        self._hist_rebuild()

    # ── LUT History ───────────────────────────────────────────────────────

    def _hist_rebuild(self):
        self._hist_tree.delete(*self._hist_tree.get_children())
        for i, h in enumerate(self._history):
            stars = '★' * h['rating'] if h['rating'] > 0 else '—'
            preview = ' '.join(f"{b:02X}" for b in h['lut'][:7]) + '…'
            t = f"{h['time_ms']}ms" if h['time_ms'] else '—'
            self._hist_tree.insert('', 'end', iid=str(i),
                values=(i+1, t, h['ticks'], stars, preview))

    def _hist_save(self):
        if not self._vars_to_lut():
            return
        ms = self._tele.get('fast_ms') or self._tele.get('fapply_ms')
        self._history.append({
            'lut':     bytes(self.lut),
            'ticks':   self._calc_lut_ticks(),
            'time_ms': ms,
            'rating':  0,
            'note':    '',
        })
        self._hist_rebuild()
        self._log_info(f"Saved LUT #{len(self._history)} to history "
                       f"({self._calc_lut_ticks()} ticks  {ms}ms)")

    def _hist_load(self):
        sel = self._hist_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        h = self._history[idx]
        self.lut[:] = h['lut']
        self._lut_to_vars()
        self._refresh_waveform()
        self._log_info(f"Loaded LUT #{idx+1} from history")

    def _hist_rate(self, stars: int):
        sel = self._hist_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        self._history[idx]['rating'] = stars
        self._hist_rebuild()
        self._hist_tree.selection_set(str(idx))

    def _hist_delete(self):
        sel = self._hist_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        self._history.pop(idx)
        self._hist_rebuild()

    def _build_console(self, parent):
        top = tk.Frame(parent, bg=C['bg2'])
        top.pack(fill='x')
        tk.Label(top, textvariable=self.status_var,
                 bg=C['bg2'], fg=C['green'], font=('Menlo', 11),
                 width=42, anchor='w').pack(side='left', padx=10, pady=3)
        tk.Button(top, text="Clear log", bg=C['bg3'], fg=C['fg2'],
                  relief='flat', padx=8, font=('Menlo', 10),
                  command=self._clear_log).pack(side='right', padx=8, pady=3)

        frame = tk.Frame(parent, bg=C['bg2'])
        frame.pack(fill='x')
        self.console = tk.Text(
            frame, height=5, bg='#0d0d0d', fg=C['fg'],
            font=('Menlo', 10), relief='flat',
            state='disabled', wrap='word', insertbackground=C['fg'])
        scroll = tk.Scrollbar(frame, command=self.console.yview)
        self.console.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.console.pack(fill='x', padx=(10, 0), pady=(0, 4))
        self.console.tag_configure('rx',   foreground=C['green'])
        self.console.tag_configure('tx',   foreground=C['accent'])
        self.console.tag_configure('info', foreground=C['fg2'])
        self.console.tag_configure('err',  foreground=C['red'])
        self.console.tag_configure('ts',   foreground='#555555')

    def _log(self, prefix: str, msg: str, tag: str):
        ts = datetime.datetime.now().strftime('%H:%M:%S')
        def _do():
            self.console.configure(state='normal')
            self.console.insert('end', f'{ts} ', 'ts')
            self.console.insert('end', f'{prefix} ', tag)
            self.console.insert('end', f'{msg}\n')
            self.console.configure(state='disabled')
            self.console.see('end')
        self.root.after(0, _do)

    def _log_rx(self, msg):  self._log('←', msg, 'rx')
    def _log_tx(self, msg):  self._log('→', msg, 'tx')
    def _log_info(self, msg): self._log('·', msg, 'info')

    def _clear_log(self):
        self.console.configure(state='normal')
        self.console.delete('1.0', 'end')
        self.console.configure(state='disabled')

    # ── Waveform ──────────────────────────────────────────────────────────

    def _refresh_waveform(self):
        c = self.wf_canvas
        c.delete('all')
        W = c.winfo_width() or 800
        H = c.winfo_height() or 280

        if W < 10:
            self.root.after(50, self._refresh_waveform)
            return

        margin_l, margin_r, margin_t, margin_b = 70, 10, 10, 34
        plot_w = W - margin_l - margin_r
        plot_h = H - margin_t - margin_b
        track_h = plot_h // VS_GROUPS
        # Y fractions: VSH1=+15V at 0.15, VSH2=+5V at 0.28, VSS=0V at 0.5, VSL=-15V at 0.85
        levels = {1: 0.15, 1/3: 0.28, 0: 0.5, -1: 0.85}

        # Detect LUT4 (VCOM) non-zero — catastrophic if set
        lut4_nonzero = any(self.lut[4 * 7 + ph] != 0 for ph in range(PHASES))

        all_segs = [decode_waveform(self.lut, g) for g in range(VS_GROUPS)]
        total_t  = max((sum(d for _, d, _ in s) for s in all_segs if s), default=1) or 1

        for g in range(VS_GROUPS):
            segs = all_segs[g]
            ty   = margin_t + g * track_h
            mid  = ty + track_h // 2
            col  = GROUP_COLORS[g]

            # Highlight VCOM row if non-zero (danger!)
            bg_fill = '#3d1010' if (g == 4 and lut4_nonzero) else C['bg3']
            c.create_rectangle(margin_l, ty, W - margin_r, ty + track_h - 2,
                               fill=bg_fill, outline='')

            lbl_text = GROUP_LABELS[g]
            lbl_col  = C['red'] if (g == 4 and lut4_nonzero) else col
            c.create_text(margin_l - 4, mid, text=lbl_text,
                          fill=lbl_col, font=('Menlo', 8), anchor='e')
            if g == 4 and lut4_nonzero:
                c.create_text(margin_l + plot_w // 2, mid,
                              text="⚠ VCOM NON-ZERO — ALL PIXELS AFFECTED!",
                              fill=C['red'], font=('Menlo', 9, 'bold'))

            c.create_line(margin_l, mid, W - margin_r, mid, fill=C['border'], dash=(2, 4))
            y_pos  = ty + int(track_h * 0.15)
            y_vsh2 = ty + int(track_h * 0.28)
            y_neg  = ty + int(track_h * 0.85)
            c.create_line(margin_l, y_pos, W - margin_r, y_pos, fill=C['vgh'], dash=(1, 6))
            c.create_line(margin_l, y_vsh2, W - margin_r, y_vsh2, fill=C['vsh2'], dash=(1, 6))
            c.create_line(margin_l, y_neg, W - margin_r, y_neg, fill=C['vgl'], dash=(1, 6))
            c.create_text(margin_l - 4, y_pos,  text='+15V', fill=C['vgh'],  font=('Menlo', 7), anchor='e')
            c.create_text(margin_l - 4, y_vsh2, text='+5V',  fill=C['vsh2'], font=('Menlo', 7), anchor='e')
            c.create_text(margin_l - 4, y_neg,  text='-15V', fill=C['vgl'],  font=('Menlo', 7), anchor='e')

            if not segs:
                c.create_text(margin_l + plot_w // 2, mid, text="(no active phases)",
                              fill=C['fg2'], font=('Menlo', 8))
                continue

            x = margin_l
            prev_y = None
            for (v_norm, d, v_raw) in segs:
                px_w = max(1, int(d / total_t * plot_w))
                y    = ty + int(track_h * levels[v_norm])
                seg_col = vs_color(v_raw)
                if prev_y is not None and prev_y != y:
                    c.create_line(x, prev_y, x, y, fill=seg_col, width=2)
                c.create_line(x, y, x + px_w, y, fill=seg_col, width=2)
                c.create_rectangle(x, min(y, mid), x + px_w, max(y, mid),
                                   fill=seg_col, outline='', stipple='gray25')
                x += px_w
                prev_y = y

        # Phase boundary markers — vertical lines + "Ph N" labels on X axis
        phase_frames = []
        for ph in range(PHASES):
            base = 35 + ph * 5
            ta, tb, tc, td, rp = (self.lut[base], self.lut[base+1],
                                   self.lut[base+2], self.lut[base+3], self.lut[base+4])
            phase_frames.append((ta + tb + tc + td) * (rp + 1))

        cumulative = 0
        for ph in range(PHASES):
            pf = phase_frames[ph]
            x_start = margin_l + int(cumulative / total_t * plot_w)
            x_end   = margin_l + int((cumulative + pf) / total_t * plot_w)
            x_mid   = (x_start + x_end) // 2 if pf > 0 else x_start

            # Boundary line (skip the very first)
            if ph > 0:
                c.create_line(x_start, margin_t, x_start, H - margin_b,
                              fill='#555555', dash=(3, 3), width=1)
                c.create_line(x_start, H - margin_b, x_start, H - margin_b + 4,
                              fill=C['fg2'], width=1)

            # Phase label
            lbl_col = C['fg2'] if pf > 0 else '#444444'
            c.create_text(x_mid if pf > 0 else x_start + 6,
                          H - margin_b + 10,
                          text=f"Ph{ph}" if pf > 0 else f"Ph{ph}·",
                          fill=lbl_col, font=('Menlo', 7))

            # Frame count below label (only if phase has content)
            if pf > 0:
                c.create_text(x_mid, H - margin_b + 19,
                              text=f"{pf}f", fill='#555555', font=('Menlo', 6))

            cumulative += pf

        for g in range(VS_GROUPS):
            bar, bar_bg, lbl = self.dc_bars[g]
            pos, neg, balance = dc_balance(self.lut, g)
            total = (pos + neg) or 1
            bw = bar_bg.winfo_width() or 300
            centre = bw // 2
            max_half = centre - 2
            bar.place_forget()
            warn = abs(balance) > 10  # threshold: ~150 V·frame equivalent
            if abs(balance) < 0.5:
                bar.config(bg=C['green'])
                bar.place(x=centre - 2, y=0, width=4, height=16)
                status = "OK (balanced)"
                fg_col = C['green']
            elif balance > 0:
                bar.config(bg=C['red'] if warn else C['vgh'])
                w = min(max_half, int(balance / total * max_half * 2))
                bar.place(x=centre, y=0, width=max(w, 2), height=16)
                status = f"+{balance:.1f}  ← DC HIGH{'  ⚠ WARN' if warn else ''}"
                fg_col = C['red'] if warn else C['orange']
            else:
                bar.config(bg=C['red'] if warn else C['vgl'])
                w = min(max_half, int(-balance / total * max_half * 2))
                bar.place(x=centre - max(w, 2), y=0, width=max(w, 2), height=16)
                status = f"{balance:.1f}  → DC LOW{'  ⚠ WARN' if warn else ''}"
                fg_col = C['red'] if warn else C['orange']
            lbl.config(text=f"  pos={pos:.1f} neg={neg:.1f}  {status}", fg=fg_col)

    # ── LUT ↔ StringVars ──────────────────────────────────────────────────

    def _refresh_phase_indicators(self):
        """Update colored activity dots in the timing table (VS non-zero = lit)."""
        if not self._phase_indicators:
            return
        for ph in range(PHASES):
            for g in range(VS_GROUPS):
                active = self.lut[g * 7 + ph] != 0
                col = GROUP_COLORS[g] if active else C['bg3']
                self._phase_indicators[ph][g].config(fg=col)

    def _lut_to_vars(self):
        for i, b in enumerate(self.lut):
            self.byte_vars[i].set(f"{b:02X}")
        self._refresh_phase_indicators()

    def _vars_to_lut(self) -> bool:
        for i, v in enumerate(self.byte_vars):
            txt = v.get().strip().lstrip('0x').lstrip('0X')
            try:
                self.lut[i] = int(txt, 16) if txt else 0
            except ValueError:
                return False
        return True

    def _on_var_change(self, idx):
        txt = self.byte_vars[idx].get().strip()
        try:
            self.lut[idx] = int(txt, 16) if txt else 0
            self.byte_entries[idx].config(fg=C['fg'])
        except ValueError:
            self.byte_entries[idx].config(fg=C['red'])
        # Update activity dot for this byte if it's in the VS section (bytes 0-34)
        if idx < 35 and self._phase_indicators:
            g, ph = divmod(idx, 7)
            active = self.lut[idx] != 0
            self._phase_indicators[ph][g].config(fg=GROUP_COLORS[g] if active else C['bg3'])
        self._refresh_waveform()
        self._refresh_explorer()

    # ── LUT / frame actions ───────────────────────────────────────────────

    def _on_connect_click(self):
        if not BLEAK_OK:
            messagebox.showerror("bleak missing", "Install bleak:\n  pip install bleak")
            return
        if self.connected:
            self._ble_run(self._ble_disconnect())
        else:
            self._ble_run(self._ble_connect())

    def _on_apply(self):
        if not self._vars_to_lut():
            messagebox.showerror("Parse error", "Invalid hex value in LUT editor")
            return
        hex_str = self.lut.hex().upper()
        cmd = f"LUTW:{hex_str}\nAPPLY\n".encode()
        if self.connected:
            self._ble_run(self._ble_send_bytes(cmd))
            self._log_tx(f"LUTW: ({len(hex_str)} chars)")
            self._log_tx("APPLY")
        else:
            self._log_info(f"[offline] LUTW:{hex_str[:24]}…")
        self._refresh_waveform()

    async def _send_bwr_frame(self, bw_buf, red_buf, desc="frame"):
        # 1. Disable screensaver so it doesn't overwrite our framebuffer during transfer.
        await self._ble_send_bytes(b"SS:0\n")
        await asyncio.sleep(0.35)   # let screensaver thread finish its current cycle

        total_chunks = (FB_SIZE + FW_BYTES_PER_CHUNK - 1) // FW_BYTES_PER_CHUNK
        has_red = any(red_buf)
        grand_total = total_chunks * (2 if has_red else 1)
        sent = 0

        # 2. Send BW buffer in chunks with inter-chunk pacing.
        for off in range(0, FB_SIZE, FW_BYTES_PER_CHUNK):
            chunk = bw_buf[off:off + FW_BYTES_PER_CHUNK]
            await self._ble_send_bytes(f"FW:{off}:{chunk.hex().upper()}\n".encode())
            sent += 1
            if sent % 20 == 0:
                pct = sent * 100 // grand_total
                self._log_info(f"  {desc} FW {off + len(chunk)}/{FB_SIZE}B  {pct}%")
            await asyncio.sleep(0.02)   # 20 ms pacing — avoids NUS RX buffer overflow

        # 3. Send Red buffer if needed.
        if has_red:
            for off in range(0, FB_SIZE, RW_BYTES_PER_CHUNK):
                chunk = red_buf[off:off + RW_BYTES_PER_CHUNK]
                await self._ble_send_bytes(f"RW:{off}:{chunk.hex().upper()}\n".encode())
                sent += 1
                if sent % 20 == 0:
                    pct = sent * 100 // grand_total
                    self._log_info(f"  {desc} RW {off + len(chunk)}/{FB_SIZE}B  {pct}%")
                await asyncio.sleep(0.02)

        # 4. Trigger display update.
        await self._ble_send_bytes(b"FAPPLY\n")
        self._log_tx(f"{desc}: {FB_SIZE}B BW" + (" + Red" if has_red else "") + " → FAPPLY")

    def _on_send_frame(self):
        if not PIL_OK:
            messagebox.showerror("Pillow missing", "Install Pillow:\n  pip install Pillow")
            return
        path = filedialog.askopenfilename(
            title="Open image (black / white / red pixels for BWR display)",
            filetypes=[("Images", "*.png *.jpg *.jpeg *.bmp *.gif"), ("All", "*.*")])
        if not path:
            return
        try:
            img = Image.open(path)
            bw_buf, red_buf = _make_phys_buffers(img)
        except Exception as e:
            messagebox.showerror("Image error", str(e))
            return
        if not self.connected:
            self._log_info(f"[offline] would send BWR frame from {path}")
            return
        self._log_tx(f"Sending {path.split('/')[-1]}…")
        self._ble_run(self._send_bwr_frame(bw_buf, red_buf, path.split('/')[-1]))

    def _on_test_pattern(self, n):
        if not PIL_OK:
            messagebox.showerror("Pillow missing", "Install Pillow:\n  pip install Pillow")
            return
        makers = {1: _pattern_color_bands, 2: _pattern_text_demo, 3: _pattern_resolution}
        labels = {1: "Bands", 2: "Text", 3: "Resolution"}
        try:
            img = makers[n]()
            bw_buf, red_buf = _make_phys_buffers(img)
        except Exception as e:
            messagebox.showerror("Pattern error", str(e))
            return
        if not self.connected:
            self._log_info(f"[offline] pattern P{n}: {labels[n]}")
            return
        self._log_tx(f"Pattern P{n} ({labels[n]})…")
        self._ble_run(self._send_bwr_frame(bw_buf, red_buf, labels[n]))

    def _on_clear(self):
        self._send_cmd("CLEAR\n", "CLEAR")

    def _on_lut_help(self):
        """Open a scrollable SSD1675A LUT field reference dialog."""
        win = tk.Toplevel(self.root)
        win.title("SSD1675A LUT Reference")
        win.configure(bg=C['bg'])
        win.resizable(True, True)
        win.geometry("660x600")
        HELP_TEXT = (
            "SSD1675A LUT Reference — 70 bytes total\n"
            "═══════════════════════════════════════════════════════════════\n\n"
            "STRUCTURE\n"
            "  Bytes  0-34   VS (Voltage Source)  — WHAT voltage to apply\n"
            "  Bytes 35-69   Timing               — HOW LONG to apply it\n\n"
            "───────────────────────────────────────────────────────────────\n"
            "VS SECTION  bytes 0-34  (5 LUT rows × 7 phases)\n\n"
            "LUT row semantics (NOT prev→new transitions — confirmed by experiment):\n"
            "  LUT0 (BLACK)  bw=0, red=0  — waveform for black pixels\n"
            "  LUT1 (WHITE)  bw=1, red=0  — waveform for white pixels\n"
            "  LUT2 (RED)    red=1        — waveform for red pixels (copy 1)\n"
            "  LUT3 (RED)    red=1        — waveform for red pixels (copy 2, same as LUT2)\n"
            "  LUT4 (VCOM)   ALL ZEROS!   — VCOM electrode, affects ALL pixels globally\n"
            "                               ANY non-zero byte = full-screen colour disaster\n\n"
            "VS BYTE ENCODING — each byte = 4 × 2-bit voltage slots\n\n"
            "  Bit order (confirmed): A=bits[7:6], B=bits[5:4], C=bits[3:2], D=bits[1:0]\n\n"
            "  Voltage codes:\n"
            "    00  VSS   — 0 V    (neutral / hold)\n"
            "    01  VSH1  — +15 V  (positive drive — pushes black pigment to surface)\n"
            "    10  VSL   — −15 V  (negative drive — erases / pulls pigment away)\n"
            "    11  VSH2  — +5 V   (weak positive — red pigment reveal; ≤60 frames)\n\n"
            "  RED waveform tips:\n"
            "    - Needs clean white base before red can develop\n"
            "    - 'Ratchet': VSH1 burst → VSL burst lifts red pigment\n"
            "    - VSH2 chunks carry red to surface; limit each chunk to ≤60 frames\n"
            "    - Long VSH2 without VSL reset → gray creep\n\n"
            "───────────────────────────────────────────────────────────────\n"
            "TIMING SECTION  bytes 35-69  (7 phases × 5 bytes)\n\n"
            "  TP_A   Duration of slot A in oscillator frames (~8.0 ms each)  Range 0-255\n"
            "  TP_B   Duration of slot B\n"
            "  TP_C   Duration of slot C\n"
            "  TP_D   Duration of slot D\n"
            "  RP     Repeat count: A→B→C→D repeats (RP+1) times total.\n"
            "          RP=0 → runs ONCE (NOT skipped!)\n"
            "          RP=1 → runs TWICE, etc.\n\n"
            "  Phase frames = (TP_A + TP_B + TP_C + TP_D) × (RP+1)\n"
            "  Total frames = Σ phase_frames over all 7 phases\n"
            "  Predicted time ≈ total_frames × 8.0 ms + 550 ms overhead\n\n"
            "  All 5 LUT rows share the SAME timing bytes (phases are global).\n\n"
            "───────────────────────────────────────────────────────────────\n"
            "TUNING GUIDE\n\n"
            "  Faster updates:\n"
            "    1. Reduce TP values in later phases (Ph 3-6) — fine-grained.\n"
            "    2. Reduce RP on active phases — coarser step.\n"
            "    3. Zero out entire phases not contributing to quality.\n\n"
            "  Less ghosting / better B&W quality:\n"
            "    1. Strong VSL in early phases for LUT1 (WHITE) → clean erase.\n"
            "    2. Strong VSH1 in LUT0 (BLACK) → deep black.\n"
            "    3. Keep DC balance |Σ| < 10 (see DC BALANCE panel).\n\n"
            "  Better red:\n"
            "    1. Clean white phase first (full LUT1-style erase).\n"
            "    2. Ratchet in LUT2: VSH1 burst → VSL burst → VSH2 chunks.\n"
            "    3. VSH2 chunks ≤60 frames each, separated by VSL resets.\n\n"
            "  DC Balance:\n"
            "    Weighted sum per row: VSH1=+1, VSL=-1, VSH2=+1/3, VSS=0.\n"
            "    Warning threshold: |balance| > 10 (≈ 150 V·frame equivalent).\n"
            "    Imbalance → ion drift → image burn-in over years.\n\n"
            "───────────────────────────────────────────────────────────────\n"
            "COMMANDS (BLE / serial)\n\n"
            "  LUTW:xxyy...   Load full 140-char hex (70 bytes)\n"
            "  LUTUSE:1/0     Enable/disable custom LUT for partial updates\n"
            "  LGET           Read current LUT from device (7 BLE chunks)\n"
            "  LTEST          Start ghosting test (3 moving balls: B / W / R)\n"
            "  STAT           Get live frame timing snapshot\n"
            "  HOST:1/0       Enable/disable TELE: telemetry stream\n"
        )
        txt = tk.Text(win, bg=C['bg2'], fg=C['fg'], font=('Menlo', 10),
                      relief='flat', padx=14, pady=10, wrap='none',
                      cursor='arrow', state='normal')
        txt.insert('1.0', HELP_TEXT)
        txt.config(state='disabled')
        sb_y = tk.Scrollbar(win, orient='vertical',   command=txt.yview)
        sb_x = tk.Scrollbar(win, orient='horizontal', command=txt.xview)
        txt.config(yscrollcommand=sb_y.set, xscrollcommand=sb_x.set)
        sb_y.pack(side='right', fill='y')
        sb_x.pack(side='bottom', fill='x')
        txt.pack(fill='both', expand=True)
        tk.Button(win, text="Close", command=win.destroy,
                  bg=C['bg3'], fg=C['fg'], font=('Menlo', 10),
                  relief='flat', padx=16, pady=4,
                  activebackground=C['border']).pack(pady=6)

    def _on_get_lut(self):
        """Request current LUT from device (LGET → 7 × LUT:N: reply chunks)."""
        self._lget_buf = [None] * 7
        self._send_cmd("LGET\n", "LGET")

    def _on_factory(self):
        self.lut[:] = FACTORY_LUT
        self._lut_to_vars()
        self._refresh_waveform()
        self._log_info("Loaded factory LUT")

    def _on_import(self):
        path = filedialog.askopenfilename(
            title="Import LUT hex file",
            filetypes=[("Hex files", "*.hex *.txt"), ("All files", "*.*")])
        if not path: return
        try:
            raw = open(path).read().replace('\n','').replace(' ','').replace(':','')
            if len(raw) != LUT_SIZE * 2:
                raise ValueError(f"Expected {LUT_SIZE*2} hex chars, got {len(raw)}")
            for i in range(LUT_SIZE):
                self.lut[i] = int(raw[i*2:i*2+2], 16)
            self._lut_to_vars()
            self._refresh_waveform()
            self._log_info(f"Imported {path}")
        except Exception as e:
            messagebox.showerror("Import error", str(e))

    def _on_export(self):
        path = filedialog.asksaveasfilename(
            title="Export LUT hex", defaultextension=".hex",
            filetypes=[("Hex files", "*.hex"), ("Text", "*.txt")])
        if not path: return
        open(path, 'w').write(self.lut.hex().upper() + '\n')
        self._log_info(f"Exported {path}")

    def _load_lut_from_hex(self, raw: str, source='paste') -> bool:
        """Parse a hex string (with or without spaces/0x/commas) into self.lut.
        Accepts plain hex, C-array style (0x__, __,), or spaced pairs.
        Returns True on success."""
        import re
        # strip 0x prefixes, commas, spaces, newlines, C-style comments
        raw = re.sub(r'//[^\n]*', '', raw)          # remove // comments
        raw = re.sub(r'/\*.*?\*/', '', raw, flags=re.S)  # remove /* */ comments
        raw = raw.replace('0x', '').replace('0X', '')
        raw = re.sub(r'[^0-9a-fA-F]', '', raw)      # keep only hex chars
        if len(raw) != LUT_SIZE * 2:
            messagebox.showerror("LUT parse error",
                f"Expected {LUT_SIZE * 2} hex chars ({LUT_SIZE} bytes).\n"
                f"Got {len(raw)} hex chars ({len(raw)//2} bytes).\n\n"
                "Accepted formats:\n"
                "  • 140-char plain hex string\n"
                "  • C array:  0x22, 0x11, 0x10, ...\n"
                "  • Spaced:   22 11 10 ...")
            return False
        try:
            for i in range(LUT_SIZE):
                self.lut[i] = int(raw[i*2:i*2+2], 16)
            self._lut_to_vars()
            self._refresh_waveform()
            self._log_info(f"[{source}] LUT loaded ({LUT_SIZE} bytes)")
            return True
        except ValueError as e:
            messagebox.showerror("Parse error", str(e))
            return False

    def _on_paste_dialog(self):
        """Open a proper dialog for pasting a LUT — plain hex, C array, or spaced."""
        win = tk.Toplevel(self.root)
        win.title("Paste LUT")
        win.configure(bg=C['bg'])
        win.geometry("560x280")
        win.resizable(False, False)

        tk.Label(win,
                 text="Paste LUT hex — plain, C-array (0x__, __,), or spaced pairs:",
                 bg=C['bg'], fg=C['fg2'], font=('Menlo', 10)).pack(anchor='w', padx=12, pady=(10,4))

        txt = tk.Text(win, bg=C['bg3'], fg=C['fg'], insertbackground=C['fg'],
                      relief='flat', font=('Menlo', 11), height=7, wrap='word',
                      padx=8, pady=6)
        txt.pack(fill='both', expand=True, padx=12)
        txt.focus_set()

        # macOS tkinter: bind Cmd+V explicitly; clear field first so double-fire
        # (our handler + system handler both firing) doesn't duplicate the content
        def _paste(event=None):
            try:
                clip = win.clipboard_get()
                txt.delete('1.0', 'end')
                txt.insert('1.0', clip)
            except tk.TclError:
                pass
            return 'break'
        txt.bind('<Command-v>', _paste)
        txt.bind('<Control-v>', _paste)

        info = tk.Label(win, text=f"Need {LUT_SIZE} bytes = {LUT_SIZE*2} hex chars",
                        bg=C['bg'], fg=C['fg2'], font=('Menlo', 9))
        info.pack(anchor='w', padx=12, pady=(2,4))

        def _do_load():
            raw = txt.get('1.0', 'end')
            if self._load_lut_from_hex(raw, source='paste'):
                win.destroy()

        btn_frame = tk.Frame(win, bg=C['bg'])
        btn_frame.pack(pady=(0, 10))
        tk.Button(btn_frame, text="Load", command=_do_load,
                  bg='#2d4f2d', fg=C['green'], font=('Menlo', 11, 'bold'),
                  relief='flat', padx=20, pady=4).pack(side='left', padx=6)
        tk.Button(btn_frame, text="Cancel", command=win.destroy,
                  bg=C['bg3'], fg=C['fg2'], font=('Menlo', 11),
                  relief='flat', padx=16, pady=4).pack(side='left', padx=6)

        win.bind('<Escape>', lambda _: win.destroy())
        txt.bind('<Control-Return>', lambda _: _do_load())
        txt.bind('<Command-Return>', lambda _: _do_load())

    def _on_paste_enter(self, _event=None):
        # kept for backward compat; paste_var no longer exists in the UI
        pass

    # ── Device controls (root firmware commands) ──────────────────────────

    def _send_cmd(self, raw: str, log_label=None):
        """Send a raw newline-terminated command string."""
        data = (raw if raw.endswith('\n') else raw + '\n').encode()
        label = log_label or raw.strip()
        if self.connected:
            self._ble_run(self._ble_send_bytes(data))
            self._log_tx(label)
        else:
            self._log_info(f"[offline] {label}")

    def _on_dev_time(self):
        now = datetime.datetime.now()
        cmd = f"TIME={now.hour}:{now.minute}:{now.second}\n"
        self._send_cmd(cmd, f"TIME={now.strftime('%H:%M:%S')}")

    def _on_dev_batt(self):
        self._send_cmd("BATT\n", "BATT")

    def _on_dev_saver(self):
        self._send_cmd("SAVER\n", "SAVER")

    def _on_dev_ss(self, en: int):
        self._send_cmd(f"SS:{en}\n", f"SS:{en}")

    def _on_dev_update(self):
        self._send_cmd("UPDATE\n", "UPDATE")

    def _on_dev_fast(self):
        self._send_cmd("FAST\n", "FAST")

    def _on_dev_clean(self):
        self._send_cmd("CLEAN\n", "CLEAN")

    def _on_dev_fapply(self):
        self._send_cmd("FAPPLY\n", "FAPPLY")

    def _on_dev_mode(self, selection: str):
        idx = selection.split('-')[0]
        self._send_cmd(f"MODE: {idx}\n", f"MODE: {idx}")

    def _on_dev_rot(self, selection: str):
        self._send_cmd(f"ROT: {selection}\n", f"ROT: {selection}")

    def _on_dev_dsaver(self, en: int):
        self._send_cmd(f"DSAVER {en}\n", f"DSAVER {en}")

    def _on_dev_text(self):
        msg = self.text_cmd_var.get().strip()
        if not msg:
            return
        self._send_cmd(f"TEXT: {msg}\n", f"TEXT: {msg}")

    def _on_dev_anim(self):
        self._send_cmd("ANIM\n", "ANIM")

    def _on_dev_lutuse(self, en: int):
        self._send_cmd(f"LUTUSE:{en}\n", f"LUTUSE:{en}")

    def _on_dev_ltest(self):
        self._send_cmd("LTEST\n", "LTEST")

    # ── Helpers ───────────────────────────────────────────────────────────

    def _set_status(self, msg):
        self.root.after(0, lambda: self.status_var.set(msg))


# ══════════════════════════════════════════════════════════════════════════
if __name__ == '__main__':
    root = tk.Tk()
    app  = App(root)
    root.mainloop()
