#!/usr/bin/env python3
"""
SSD1675A LUT Tester — macOS BLE companion.

Layout:
  Left  — 70-byte hex editor (grouped: VS rows 0-4, Phase blocks 0-6)
  Right — Waveform canvas + DC balance bars
  Top   — BLE controls
  Bottom— Quick actions (Apply, Clear, Import/Export)

LUT structure (70 bytes, command 0x32):
  Bytes  0-34  VS section  — 5 groups × 7 bytes
                             Each byte encodes 4 voltage-source slots (2 bits each):
                             bits[1:0]=slot0  [3:2]=slot1  [5:4]=slot2  [7:6]=slot3
                             Values: 00=VSS(0V)  01=VGH(+15V)  10=VGL(-15V)  11=FLT(float)
  Bytes 35-69  Timing section — 7 phases × 5 bytes
                             TP_A, TP_B, TP_C, TP_D  (duration in line-periods)
                             RP  (repeat count)

DC balance: Σ(VGH × time) + Σ(VGL × time) should equal 0 per group.
"""

import asyncio
import threading
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import struct, time

try:
    from bleak import BleakClient, BleakScanner
    BLEAK_OK = True
except ImportError:
    BLEAK_OK = False

# ── NUS UUIDs ──────────────────────────────────────────────────────────────
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # phone→device (Write)
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # device→phone (Notify)
TARGET_NAME = "LUT-Tester"

# ── Dark theme ─────────────────────────────────────────────────────────────
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
    'vgh':     '#e06c75',   # positive voltage
    'vgl':     '#61afef',   # negative voltage
    'vss':     '#4a4a4a',   # zero
    'flt':     '#888888',   # float
}

GROUP_COLORS = ['#e06c75', '#98c379', '#61afef', '#c678dd', '#e5c07b']
GROUP_LABELS = ['WW (0→0)', 'BW (1→0)', 'WB (0→1)', 'BB (1→1)', 'RED']

LUT_SIZE     = 70
VS_GROUPS    = 5
PHASES       = 7
FACTORY_LUT  = bytes([
    0x22,0x11,0x10,0x00,0x10,0x00,0x00, 0x11,0x88,0x80,0x80,0x80,0x00,0x00,
    0x6A,0x9B,0x9B,0x9B,0x9B,0x00,0x00, 0x6A,0x9B,0x9B,0x9B,0x9B,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x04,0x18,0x04,0x16,0x01,0x0A,0x0A,
    0x0A,0x0A,0x02,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x04,
    0x04,0x08,0x3C,0x07,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,
])


def vs_voltage(v):
    """Normalised voltage for DC calc: VGH=+1, VGL=-1, VSS/FLT=0."""
    return {0: 0, 1: 1, 2: -1, 3: 0}[v & 3]

def vs_color(v):
    return [C['vss'], C['vgh'], C['vgl'], C['flt']][v & 3]

def vs_name(v):
    return ['VSS', 'VGH', 'VGL', 'FLT'][v & 3]


def decode_waveform(lut: bytearray, group: int):
    """
    Return list of (voltage_norm, duration_ticks) for one VS group.
    duration_ticks = TP * RP  (absolute time units for drawing).
    """
    segments = []
    for phase in range(PHASES):
        vs_byte = lut[group * 7 + phase]
        t_base  = 35 + phase * 5
        tp = [lut[t_base], lut[t_base+1], lut[t_base+2], lut[t_base+3]]
        rp  = lut[t_base + 4]
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
    """Return (pos_ticks, neg_ticks, balance) for a group."""
    pos = neg = 0
    for (v_norm, d, _) in decode_waveform(lut, group):
        if v_norm > 0: pos += d
        elif v_norm < 0: neg += d
    return pos, neg, pos - neg


# ══════════════════════════════════════════════════════════════════════════
class App:

    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("SSD1675A LUT Tester")
        root.geometry("1280x760")
        root.configure(bg=C['bg'])
        root.resizable(True, True)

        self.lut        = bytearray(FACTORY_LUT)
        self.byte_vars  = [tk.StringVar() for _ in range(LUT_SIZE)]
        self.status_var = tk.StringVar(value="Disconnected")

        self.client    = None
        self.connected = False
        self._ble_loop  = asyncio.new_event_loop()
        threading.Thread(target=self._ble_thread, daemon=True).start()

        self._build_ui()
        self._lut_to_vars()
        self._refresh_waveform()

    # ── BLE thread ────────────────────────────────────────────────────────

    def _ble_thread(self):
        asyncio.set_event_loop(self._ble_loop)
        self._ble_loop.run_forever()

    def _ble_run(self, coro):
        """Schedule coroutine on the BLE event loop and ignore result."""
        asyncio.run_coroutine_threadsafe(coro, self._ble_loop)

    async def _ble_connect(self):
        self._set_status("Scanning…")
        self._log_info(f"Scanning for '{TARGET_NAME}'…")
        try:
            device = await BleakScanner.find_device_by_name(TARGET_NAME, timeout=8.0)
            if device is None:
                self._set_status(f"'{TARGET_NAME}' not found")
                self._log_info(f"Device not found")
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
        # Use negotiated MTU; fall back to 20 if not yet known.
        # DLE on the device is configured for 247, macOS typically negotiates ≥185.
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
        # Device may send multiple lines in one notification
        for line in msg.splitlines():
            line = line.strip()
            if line:
                self._log_rx(line)

    # ── UI ────────────────────────────────────────────────────────────────

    def _build_ui(self):
        # Top bar
        top = tk.Frame(self.root, bg=C['bg2'], pady=6)
        top.pack(fill='x', side='top')
        self._build_top_bar(top)

        # Main area
        main = tk.Frame(self.root, bg=C['bg'])
        main.pack(fill='both', expand=True, padx=8, pady=(4, 0))

        left = tk.Frame(main, bg=C['bg'])
        left.pack(side='left', fill='y', padx=(0, 6))
        self._build_editor(left)

        right = tk.Frame(main, bg=C['bg'])
        right.pack(side='left', fill='both', expand=True)
        self._build_waveform_panel(right)
        self._build_dc_panel(right)

        # Bottom: status + console
        bot = tk.Frame(self.root, bg=C['bg2'])
        bot.pack(fill='x', side='bottom')
        self._build_console(bot)

    def _build_top_bar(self, parent):
        if not BLEAK_OK:
            tk.Label(parent, text="⚠ bleak not installed — pip install bleak",
                     bg=C['bg2'], fg=C['red'], font=('Menlo', 11)).pack(side='left', padx=10)
        self.btn_connect = tk.Button(parent, text="Connect",
            bg=C['bg3'], fg=C['accent'], relief='flat', padx=12, pady=4,
            font=('Menlo', 12, 'bold'), command=self._on_connect_click)
        self.btn_connect.pack(side='left', padx=8)

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

        # Paste full hex
        tk.Label(parent, text="Paste:", bg=C['bg2'], fg=C['fg2'],
                 font=('Menlo', 11)).pack(side='left', padx=(16, 2))
        self.paste_var = tk.StringVar()
        pe = tk.Entry(parent, textvariable=self.paste_var, width=28,
                      bg=C['bg3'], fg=C['fg'], insertbackground=C['fg'],
                      relief='flat', font=('Menlo', 11))
        pe.pack(side='left')
        pe.bind('<Return>', self._on_paste_enter)

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

        # Header label + column indices
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
                idx = g * 7 + col
                self._byte_entry(f, row, col + 1, idx)

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
                # Highlight TP bytes that are non-zero
                if sub < 4:
                    e.config(fg=C['orange'])
                else:
                    e.config(fg=C['green'])

        # Remaining bytes 35+7*5=70 — none, covers exactly 35 bytes
        # Extra info rows for bytes 55-69 overlap with Ph 4-6 above

    def _build_waveform_panel(self, parent):
        lbl = tk.Label(parent, text="WAVEFORM", bg=C['bg'],
                       fg=C['accent'], font=('Menlo', 12, 'bold'))
        lbl.pack(anchor='w')

        self.wf_canvas = tk.Canvas(parent, bg=C['bg2'], height=320,
                                   highlightthickness=1,
                                   highlightbackground=C['border'])
        self.wf_canvas.pack(fill='x', pady=(2, 4))
        self.wf_canvas.bind('<Configure>', lambda e: self._refresh_waveform())

    def _build_dc_panel(self, parent):
        lbl = tk.Label(parent, text="DC BALANCE  (pos ticks − neg ticks per group)",
                       bg=C['bg'], fg=C['accent'], font=('Menlo', 11, 'bold'))
        lbl.pack(anchor='w', pady=(4, 2))

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
                 width=36, anchor='w').pack(side='left', padx=10, pady=3)

        tk.Button(top, text="Clear log", bg=C['bg3'], fg=C['fg2'],
                  relief='flat', padx=8, font=('Menlo', 10),
                  command=self._clear_log).pack(side='right', padx=8, pady=3)

        frame = tk.Frame(parent, bg=C['bg2'])
        frame.pack(fill='x')

        self.console = tk.Text(
            frame, height=5, bg='#0d0d0d', fg=C['fg'],
            font=('Menlo', 10), relief='flat',
            state='disabled', wrap='word',
            insertbackground=C['fg'],
        )
        scroll = tk.Scrollbar(frame, command=self.console.yview)
        self.console.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.console.pack(fill='x', padx=(10, 0), pady=(0, 4))

        # Color tags
        self.console.tag_configure('rx',   foreground=C['green'])
        self.console.tag_configure('tx',   foreground=C['accent'])
        self.console.tag_configure('info', foreground=C['fg2'])
        self.console.tag_configure('err',  foreground=C['red'])
        self.console.tag_configure('ts',   foreground='#555555')

    def _log(self, prefix: str, msg: str, tag: str):
        import datetime
        ts = datetime.datetime.now().strftime('%H:%M:%S')
        def _do():
            self.console.configure(state='normal')
            self.console.insert('end', f'{ts} ', 'ts')
            self.console.insert('end', f'{prefix} ', tag)
            self.console.insert('end', f'{msg}\n')
            self.console.configure(state='disabled')
            self.console.see('end')
        self.root.after(0, _do)

    def _log_rx(self, msg: str):
        self._log('←', msg, 'rx')

    def _log_tx(self, msg: str):
        self._log('→', msg, 'tx')

    def _log_info(self, msg: str):
        self._log('·', msg, 'info')

    def _clear_log(self):
        self.console.configure(state='normal')
        self.console.delete('1.0', 'end')
        self.console.configure(state='disabled')

    # ── Waveform drawing ──────────────────────────────────────────────────

    def _refresh_waveform(self):
        c = self.wf_canvas
        c.delete('all')
        W = c.winfo_width() or 800
        H = c.winfo_height() or 320

        if W < 10:
            self.root.after(50, self._refresh_waveform)
            return

        margin_l = 70
        margin_r = 10
        margin_t = 10
        margin_b = 24
        plot_w = W - margin_l - margin_r
        plot_h = H - margin_t - margin_b

        track_h = plot_h // VS_GROUPS
        levels = {1: 0.15, 0: 0.5, -1: 0.85}   # fraction of track height

        # Compute total time ticks for X scaling
        all_segs = [decode_waveform(self.lut, g) for g in range(VS_GROUPS)]
        total_t  = max((sum(d for _, d, _ in s) for s in all_segs if s), default=1) or 1

        for g in range(VS_GROUPS):
            segs = all_segs[g]
            ty   = margin_t + g * track_h
            mid  = ty + track_h // 2
            col  = GROUP_COLORS[g]

            # Track background
            c.create_rectangle(margin_l, ty, W - margin_r, ty + track_h - 2,
                                fill=C['bg3'], outline='')
            # Group label
            c.create_text(margin_l - 4, mid, text=GROUP_LABELS[g],
                          fill=col, font=('Menlo', 8), anchor='e')

            # Zero line
            c.create_line(margin_l, mid, W - margin_r, mid,
                          fill=C['border'], dash=(2, 4))
            # +V / -V dashed guides
            y_pos = ty + int(track_h * 0.15)
            y_neg = ty + int(track_h * 0.85)
            c.create_line(margin_l, y_pos, W - margin_r, y_pos,
                          fill=C['vgh'], dash=(1, 6), width=1)
            c.create_line(margin_l, y_neg, W - margin_r, y_neg,
                          fill=C['vgl'], dash=(1, 6), width=1)
            c.create_text(margin_l - 4, y_pos, text='+V',
                          fill=C['vgh'], font=('Menlo', 7), anchor='e')
            c.create_text(margin_l - 4, y_neg, text='-V',
                          fill=C['vgl'], font=('Menlo', 7), anchor='e')

            if not segs:
                c.create_text(margin_l + plot_w // 2, mid, text="(no active phases)",
                               fill=C['fg2'], font=('Menlo', 8))
                continue

            # Draw waveform
            x = margin_l
            prev_y = None
            for (v_norm, d, v_raw) in segs:
                px_w = max(1, int(d / total_t * plot_w))
                frac = levels[v_norm]
                y    = ty + int(track_h * frac)
                seg_col = vs_color(v_raw)

                if prev_y is not None and prev_y != y:
                    c.create_line(x, prev_y, x, y, fill=seg_col, width=2)

                c.create_line(x, y, x + px_w, y, fill=seg_col, width=2)
                # Fill area under/above zero
                c.create_rectangle(x, min(y, mid), x + px_w, max(y, mid),
                                   fill=seg_col, outline='', stipple='gray25')
                x += px_w
                prev_y = y

        # Time axis ticks
        for tick in range(0, 101, 25):
            tx = margin_l + int(tick / 100 * plot_w)
            c.create_line(tx, H - margin_b, tx, H - margin_b + 4, fill=C['fg2'])
            c.create_text(tx, H - margin_b + 10, text=f"{tick}%",
                          fill=C['fg2'], font=('Menlo', 7))

        # DC balance bars
        for g in range(VS_GROUPS):
            bar, bar_bg, lbl = self.dc_bars[g]
            pos, neg, balance = dc_balance(self.lut, g)
            total = pos + neg or 1
            # bar_bg is 300px wide; centre at 150
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

    # ── LUT ↔ StringVars sync ─────────────────────────────────────────────

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

    # ── Actions ───────────────────────────────────────────────────────────

    def _on_connect_click(self):
        if not BLEAK_OK:
            messagebox.showerror("bleak missing",
                "Install bleak:\n  pip install bleak"); return
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

    def _on_clear(self):
        if self.connected:
            self._ble_run(self._ble_send_bytes(b"CLEAR\n"))
            self._log_tx("CLEAR")
        else:
            self._log_info("[offline] CLEAR")

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
            title="Export LUT hex",
            defaultextension=".hex",
            filetypes=[("Hex files", "*.hex"), ("Text", "*.txt")])
        if not path: return
        hex_str = self.lut.hex().upper()
        open(path, 'w').write(hex_str + '\n')
        self._log_info(f"Exported {path}")

    def _on_paste_enter(self, _event=None):
        raw = self.paste_var.get().strip().replace(' ','').replace('\n','')
        if raw.lower().startswith('0x'): raw = raw[2:]
        if len(raw) != LUT_SIZE * 2:
            messagebox.showerror("Paste error",
                f"Need {LUT_SIZE*2} hex chars, got {len(raw)}")
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

    # ── Helpers ───────────────────────────────────────────────────────────

    def _set_status(self, msg):
        self.root.after(0, lambda: self.status_var.set(msg))


# ══════════════════════════════════════════════════════════════════════════
if __name__ == '__main__':
    root = tk.Tk()
    app  = App(root)
    root.mainloop()
