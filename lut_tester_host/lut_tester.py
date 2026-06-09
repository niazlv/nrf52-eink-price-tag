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
    'vgh':     '#e06c75',
    'vgl':     '#61afef',
    'vss':     '#4a4a4a',
    'flt':     '#888888',
}

GROUP_COLORS = ['#e06c75', '#98c379', '#61afef', '#c678dd', '#e5c07b']
GROUP_LABELS = ['WW (0→0)', 'BW (1→0)', 'WB (0→1)', 'BB (1→1)', 'RED']

LUT_SIZE    = 70
VS_GROUPS   = 5
PHASES      = 7
FACTORY_LUT = bytes([
    0x22,0x11,0x10,0x00,0x10,0x00,0x00, 0x11,0x88,0x80,0x80,0x80,0x00,0x00,
    0x6A,0x9B,0x9B,0x9B,0x9B,0x00,0x00, 0x6A,0x9B,0x9B,0x9B,0x9B,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x04,0x18,0x04,0x16,0x01,0x0A,0x0A,
    0x0A,0x0A,0x02,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x04,
    0x04,0x08,0x3C,0x07,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,
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
    return {0: 0, 1: 1, 2: -1, 3: 0}[v & 3]

def vs_color(v):
    return [C['vss'], C['vgh'], C['vgl'], C['flt']][v & 3]

def vs_name(v):
    return ['VSS', 'VGH', 'VGL', 'FLT'][v & 3]


def decode_waveform(lut: bytearray, group: int):
    segments = []
    for phase in range(PHASES):
        vs_byte = lut[group * 7 + phase]
        t_base  = 35 + phase * 5
        tp = [lut[t_base], lut[t_base+1], lut[t_base+2], lut[t_base+3]]
        rp = lut[t_base + 4]
        if rp == 0:
            continue
        for _ in range(rp):
            for slot in range(4):
                v = (vs_byte >> (slot * 2)) & 0x3
                d = tp[slot]
                if d > 0:
                    segments.append((vs_voltage(v), d, v))
    return segments


def dc_balance(lut: bytearray, group: int):
    pos = neg = 0
    for (v_norm, d, _) in decode_waveform(lut, group):
        if v_norm > 0: pos += d
        elif v_norm < 0: neg += d
    return pos, neg, pos - neg


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
            if line:
                self._log_rx(line)

    # ── UI ────────────────────────────────────────────────────────────────

    def _build_ui(self):
        top = tk.Frame(self.root, bg=C['bg2'], pady=6)
        top.pack(fill='x', side='top')
        self._build_top_bar(top)

        dev = tk.Frame(self.root, bg=C['bg3'], pady=4)
        dev.pack(fill='x', side='top')
        self._build_device_bar(dev)

        main = tk.Frame(self.root, bg=C['bg'])
        main.pack(fill='both', expand=True, padx=8, pady=(4, 0))

        left = tk.Frame(main, bg=C['bg'])
        left.pack(side='left', fill='y', padx=(0, 6))
        self._build_editor(left)

        right = tk.Frame(main, bg=C['bg'])
        right.pack(side='left', fill='both', expand=True)
        self._build_waveform_panel(right)
        self._build_dc_panel(right)

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

        tk.Label(parent, text="Paste:", bg=C['bg2'], fg=C['fg2'],
                 font=('Menlo', 11)).pack(side='left', padx=(16, 2))
        self.paste_var = tk.StringVar()
        pe = tk.Entry(parent, textvariable=self.paste_var, width=28,
                      bg=C['bg3'], fg=C['fg'], insertbackground=C['fg'],
                      relief='flat', font=('Menlo', 11))
        pe.pack(side='left')
        pe.bind('<Return>', self._on_paste_enter)

    def _build_device_bar(self, parent):
        """Second toolbar: device-specific controls for the root peripheral_uart firmware."""
        def btn(text, cmd, fg=None, bg=None):
            b = tk.Button(parent, text=text,
                bg=bg or C['bg'], fg=fg or C['fg2'], relief='flat',
                padx=8, pady=3, font=('Menlo', 10),
                command=cmd)
            b.pack(side='left', padx=2)
            return b

        def sep():
            tk.Label(parent, text="│", bg=C['bg3'], fg=C['border'],
                     font=('Menlo', 13)).pack(side='left', padx=4)

        tk.Label(parent, text="Device:", bg=C['bg3'], fg=C['yellow'],
                 font=('Menlo', 10, 'bold')).pack(side='left', padx=(8, 4))

        btn("⏱ TIME",   self._on_dev_time,   fg=C['accent'])
        btn("🔋 BATT",  self._on_dev_batt,   fg=C['green'])

        sep()

        btn("SAVER",  self._on_dev_saver)
        btn("SS:ON",  lambda: self._on_dev_ss(1))
        btn("SS:OFF", lambda: self._on_dev_ss(0))

        sep()

        btn("UPDATE",  self._on_dev_update,  fg=C['accent'])
        btn("FAST",    self._on_dev_fast)
        btn("CLEAN",   self._on_dev_clean)
        btn("FAPPLY",  self._on_dev_fapply,  fg=C['green'])

        sep()

        tk.Label(parent, text="Mode:", bg=C['bg3'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(0, 2))
        mode_menu = tk.OptionMenu(parent, self.mode_var,
                                  "0-Turbo", "1-Bal", "2-Stab",
                                  command=self._on_dev_mode)
        mode_menu.config(bg=C['bg'], fg=C['fg2'], activebackground=C['bg3'],
                         activeforeground=C['fg'], relief='flat',
                         font=('Menlo', 10), width=7, highlightthickness=0)
        mode_menu['menu'].config(bg=C['bg3'], fg=C['fg'], font=('Menlo', 10))
        mode_menu.pack(side='left', padx=(0, 4))

        tk.Label(parent, text="Rot:", bg=C['bg3'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(0, 2))
        rot_menu = tk.OptionMenu(parent, self.rot_var, "0", "1", "2", "3",
                                 command=self._on_dev_rot)
        rot_menu.config(bg=C['bg'], fg=C['fg2'], activebackground=C['bg3'],
                        activeforeground=C['fg'], relief='flat',
                        font=('Menlo', 10), width=2, highlightthickness=0)
        rot_menu['menu'].config(bg=C['bg3'], fg=C['fg'], font=('Menlo', 10))
        rot_menu.pack(side='left', padx=(0, 4))

        btn("DSAVER:ON",  lambda: self._on_dev_dsaver(1))
        btn("DSAVER:OFF", lambda: self._on_dev_dsaver(0))

        sep()

        tk.Label(parent, text="TEXT:", bg=C['bg3'], fg=C['fg2'],
                 font=('Menlo', 10)).pack(side='left', padx=(0, 2))
        te = tk.Entry(parent, textvariable=self.text_cmd_var, width=18,
                      bg=C['bg'], fg=C['fg'], insertbackground=C['fg'],
                      relief='flat', font=('Menlo', 10))
        te.pack(side='left')
        te.bind('<Return>', lambda _: self._on_dev_text())
        btn("Send", self._on_dev_text, fg=C['accent'])

        sep()

        btn("ANIM", self._on_dev_anim, fg=C['orange'])

    # ── LUT editor ────────────────────────────────────────────────────────

    def _build_editor(self, parent):
        tk.Label(parent, text="LUT BYTES", bg=C['bg'],
                 fg=C['accent'], font=('Menlo', 12, 'bold')).pack(anchor='w', pady=(0,4))

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
        tk.Label(f, text="VS (voltage waveform)  bytes 0-34",
                 bg=C['bg'], fg=C['yellow'], font=('Menlo', 10, 'bold')).grid(
            row=0, column=0, columnspan=9, sticky='w', pady=(0, 2))
        for col, txt in enumerate([''] + [str(c) for c in range(7)]):
            tk.Label(f, text=txt, bg=C['bg'], fg=C['fg2'],
                     font=('Menlo', 9), width=3).grid(row=1, column=col)
        for g in range(VS_GROUPS):
            row = g + 2
            tk.Label(f, text=GROUP_LABELS[g], bg=C['bg'],
                     fg=GROUP_COLORS[g], font=('Menlo', 9), width=12,
                     anchor='w').grid(row=row, column=0, sticky='w')
            for col in range(7):
                self._byte_entry(f, row, col + 1, g * 7 + col)

    def _build_timing_section(self, parent):
        f = tk.Frame(parent, bg=C['bg'])
        f.pack(fill='x', pady=(10, 0))
        tk.Label(f, text="Timing (phase blocks)  bytes 35-69",
                 bg=C['bg'], fg=C['yellow'], font=('Menlo', 10, 'bold')).grid(
            row=0, column=0, columnspan=7, sticky='w', pady=(0, 2))
        for col, txt in enumerate(['Phase', 'TP_A', 'TP_B', 'TP_C', 'TP_D', 'RP']):
            tk.Label(f, text=txt, bg=C['bg'], fg=C['fg2'],
                     font=('Menlo', 9), width=5).grid(row=1, column=col)
        for ph in range(PHASES):
            row = ph + 2
            tk.Label(f, text=f"Ph {ph}", bg=C['bg'], fg=C['fg2'],
                     font=('Menlo', 9), width=5).grid(row=row, column=0)
            for sub in range(5):
                idx = 35 + ph * 5 + sub
                e = self._byte_entry(f, row, sub + 1, idx)
                e.config(fg=C['orange'] if sub < 4 else C['green'])

    def _build_waveform_panel(self, parent):
        tk.Label(parent, text="WAVEFORM", bg=C['bg'],
                 fg=C['accent'], font=('Menlo', 12, 'bold')).pack(anchor='w')
        self.wf_canvas = tk.Canvas(parent, bg=C['bg2'], height=280,
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

        margin_l, margin_r, margin_t, margin_b = 70, 10, 10, 24
        plot_w = W - margin_l - margin_r
        plot_h = H - margin_t - margin_b
        track_h = plot_h // VS_GROUPS
        levels = {1: 0.15, 0: 0.5, -1: 0.85}

        all_segs = [decode_waveform(self.lut, g) for g in range(VS_GROUPS)]
        total_t  = max((sum(d for _, d, _ in s) for s in all_segs if s), default=1) or 1

        for g in range(VS_GROUPS):
            segs = all_segs[g]
            ty   = margin_t + g * track_h
            mid  = ty + track_h // 2
            col  = GROUP_COLORS[g]

            c.create_rectangle(margin_l, ty, W - margin_r, ty + track_h - 2,
                               fill=C['bg3'], outline='')
            c.create_text(margin_l - 4, mid, text=GROUP_LABELS[g],
                          fill=col, font=('Menlo', 8), anchor='e')
            c.create_line(margin_l, mid, W - margin_r, mid, fill=C['border'], dash=(2, 4))
            y_pos = ty + int(track_h * 0.15)
            y_neg = ty + int(track_h * 0.85)
            c.create_line(margin_l, y_pos, W - margin_r, y_pos, fill=C['vgh'], dash=(1, 6))
            c.create_line(margin_l, y_neg, W - margin_r, y_neg, fill=C['vgl'], dash=(1, 6))
            c.create_text(margin_l - 4, y_pos, text='+V', fill=C['vgh'], font=('Menlo', 7), anchor='e')
            c.create_text(margin_l - 4, y_neg, text='-V', fill=C['vgl'], font=('Menlo', 7), anchor='e')

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

        for tick in range(0, 101, 25):
            tx = margin_l + int(tick / 100 * plot_w)
            c.create_line(tx, H - margin_b, tx, H - margin_b + 4, fill=C['fg2'])
            c.create_text(tx, H - margin_b + 10, text=f"{tick}%",
                          fill=C['fg2'], font=('Menlo', 7))

        for g in range(VS_GROUPS):
            bar, bar_bg, lbl = self.dc_bars[g]
            pos, neg, balance = dc_balance(self.lut, g)
            total = pos + neg or 1
            bw = bar_bg.winfo_width() or 300
            centre = bw // 2
            max_half = centre - 2
            bar.place_forget()
            if balance == 0:
                bar.config(bg=C['green'])
                bar.place(x=centre - 2, y=0, width=4, height=16)
                status = "OK (balanced)"
            elif balance > 0:
                bar.config(bg=C['vgh'])
                w = min(max_half, int(balance / total * max_half * 2))
                bar.place(x=centre, y=0, width=w, height=16)
                status = f"+{balance} ticks  ← DC HIGH"
            else:
                bar.config(bg=C['vgl'])
                w = min(max_half, int(-balance / total * max_half * 2))
                bar.place(x=centre - w, y=0, width=w, height=16)
                status = f"{balance} ticks  → DC LOW"
            lbl.config(text=f"  pos={pos} neg={neg}  {status}",
                       fg=C['green'] if balance == 0 else C['orange'])

    # ── LUT ↔ StringVars ──────────────────────────────────────────────────

    def _lut_to_vars(self):
        for i, b in enumerate(self.lut):
            self.byte_vars[i].set(f"{b:02X}")

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
        self._refresh_waveform()

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

    def _on_paste_enter(self, _event=None):
        raw = self.paste_var.get().strip().replace(' ','').replace('\n','')
        if raw.lower().startswith('0x'): raw = raw[2:]
        if len(raw) != LUT_SIZE * 2:
            messagebox.showerror("Paste error", f"Need {LUT_SIZE*2} hex chars, got {len(raw)}")
            return
        try:
            for i in range(LUT_SIZE):
                self.lut[i] = int(raw[i*2:i*2+2], 16)
            self._lut_to_vars()
            self._refresh_waveform()
            self.paste_var.set("")
            self._log_info("Pasted LUT")
        except ValueError as e:
            messagebox.showerror("Parse error", str(e))

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

    # ── Helpers ───────────────────────────────────────────────────────────

    def _set_status(self, msg):
        self.root.after(0, lambda: self.status_var.set(msg))


# ══════════════════════════════════════════════════════════════════════════
if __name__ == '__main__':
    root = tk.Tk()
    app  = App(root)
    root.mainloop()
