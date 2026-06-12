#!/usr/bin/env python3
"""
vstream.py — convert and stream video to nRF52 e-ink display over BLE NUS.

COMMANDS
  convert   Pre-process video → .ebf (eink binary frames, fast playback)
  play      Stream .ebf or raw video file to the display

QUICK START
  python vstream.py convert ../videos/bad-apple/*.webm
  python vstream.py play bad_apple.ebf --mode sync --enc drle --loop

MODES
  guaranteed  Every frame is rendered.  May be slower than original.
  sync        Real-time clock.  Late frames are skipped to stay on beat.

ENCODING
  raw    4736 B / frame  (uncompressed 1-bpp)
  rle    PackBits full frame  (~1000–2000 B for Bad Apple)
  drle   PackBits XOR-delta  (~200–600 B)  ← recommended

DEPS
  pip install bleak pillow numpy
  brew install ffmpeg   (or apt/choco)
"""

import argparse, asyncio, mmap, os, struct, subprocess, sys, time
from collections import deque
from pathlib import Path

# ── optional imports ──────────────────────────────────────────────────────────
try:
    from bleak import BleakClient, BleakScanner
    BLEAK_OK = True
except ImportError:
    BLEAK_OK = False

try:
    from PIL import Image
    PIL_OK = True
except ImportError:
    PIL_OK = False

try:
    import numpy as np
    NP_OK = True
except ImportError:
    NP_OK = False

# ── display geometry ──────────────────────────────────────────────────────────
DISP_W  = 128
DISP_H  = 296
FB_SIZE = DISP_W * DISP_H // 8   # 4736 bytes

# ── protocol ──────────────────────────────────────────────────────────────────
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
DEFAULT_DEVICE_SELECTOR = "nrf52-E-ink-clock-*"

VS_HDR1, VS_HDR2 = 0xAA, 0x55
VS_FLUSH = 0xBB
VS_STOP  = bytes([0xCC, 0xDD])

VS_RAW  = 0x00
VS_RLE  = 0x01
VS_DRLE = 0x02

ENC_NAMES = {'raw': VS_RAW, 'rle': VS_RLE, 'drle': VS_DRLE}

# ── video LUT (uploaded via LUTW: before streaming) ──────────────────────────
# Same drive scheme as the built-in TURBO (black=VSH1, white=VSL, selected by
# NEW pixel state) but Ph0 TA=8/TB=0 → 8 subframes = ~64ms wave instead of
# TURBO's 14 = 112ms. Matches the old ANIM LUT that was confirmed working.
LUT_VIDEO = bytes(
    [0x55, 0, 0, 0, 0, 0, 0,     # LUT0 black: Ph0=VSH1
     0xAA, 0, 0, 0, 0, 0, 0]     # LUT1 white: Ph0=VSL
    + [0] * 21                   # LUT2/3/4: no red, VCOM=0
    + [0x08, 0, 0, 0, 0]         # Ph0: TA=8, TB=0, RP=0 → 8 frames ≈ 64ms
    + [0] * 30                   # Ph1..Ph6 unused
)
assert len(LUT_VIDEO) == 70

def adv_has_nus(adv) -> bool:
    return NUS_SERVICE in [u.lower() for u in (getattr(adv, "service_uuids", None) or [])]

def _adv_name(dev, adv) -> str:
    return dev.name or (getattr(adv, "local_name", None) or "")

def _adv_rssi(dev, adv) -> int:
    rssi = getattr(adv, "rssi", None)        # bleak >= 0.19
    if rssi is None:
        rssi = getattr(dev, "rssi", None)    # older bleak
    return rssi if rssi is not None else -127

def _rssi_bar(rssi: int, lo: int = -100, hi: int = -40, width: int = 10) -> str:
    frac = max(0.0, min(1.0, (rssi - lo) / (hi - lo)))
    filled = round(width * frac)
    return "▮" * filled + "▯" * (width - filled)

async def scan_matching(match_fn, timeout: float = 12.0, settle: float = 3.0):
    """Collect every advertiser accepted by match_fn. After the first hit keep
    scanning `settle` more seconds so a second nearby device is not missed."""
    found = {}            # address -> (dev, adv) with the latest adv (and RSSI)
    first_hit = None

    def on_adv(dev, adv):
        nonlocal first_hit
        if match_fn(dev, adv):
            found[dev.address] = (dev, adv)
            if first_hit is None:
                first_hit = time.time()

    async with BleakScanner(on_adv):
        t0 = time.time()
        while time.time() - t0 < timeout:
            await asyncio.sleep(0.1)
            if first_hit is not None and time.time() - first_hit >= settle:
                break
    return list(found.values())

def pick_device(matches):
    """Show all matches sorted by signal strength and let the user choose one."""
    matches = sorted(matches, key=lambda m: _adv_rssi(*m), reverse=True)
    best = _adv_rssi(*matches[0])
    print(f"\nFound {len(matches)} matching devices:")
    for i, (dev, adv) in enumerate(matches, 1):
        rssi = _adv_rssi(dev, adv)
        delta = best - rssi
        if delta == 0:
            note = "← closest"
        else:
            # free-space path loss: distance ratio ≈ 10^(ΔdB/20)
            note = f"-{delta} dB (~{10 ** (delta / 20):.1f}× farther)"
        print(f"  {i}. {_adv_name(dev, adv) or '(no name)'}  {dev.address}"
              f"  {rssi:4d} dBm  [{_rssi_bar(rssi)}]  {note}")
    while True:
        ans = input(f"Select device [1-{len(matches)}, Enter=1]: ").strip()
        if ans == "":
            return matches[0][0]
        if ans.isdigit() and 1 <= int(ans) <= len(matches):
            return matches[int(ans) - 1][0]
        print("  invalid choice")

async def find_device_by_selector(selector: str, timeout: float = 12.0):
    if selector.endswith("*"):
        prefix = selector[:-1]
        match_fn = lambda dev, adv: _adv_name(dev, adv).startswith(prefix)
    else:
        match_fn = lambda dev, adv: _adv_name(dev, adv) == selector

    matches = await scan_matching(match_fn, timeout=timeout)
    if not matches and selector.endswith("*"):
        matches = await scan_matching(lambda dev, adv: adv_has_nus(adv), timeout=3.0)
    if not matches:
        return None
    if len(matches) == 1:
        dev, adv = matches[0]
        rssi = _adv_rssi(dev, adv)
        print(f"  {_adv_name(dev, adv)}  {dev.address}  {rssi} dBm  [{_rssi_bar(rssi)}]")
        return dev
    return pick_device(matches)

# ── EBF (eink binary frames) format ──────────────────────────────────────────
# 16-byte header:  magic(4) width(2) height(2) fps(f32) n_frames(u32)
# then n_frames × FB_SIZE bytes of raw 1-bpp frames

EBF_MAGIC      = b"EBF1"
EBF_HDR_SIZE   = 16
EBF_HDR_FMT    = "<4sHHfI"   # magic, w, h, fps, n_frames

# ── PackBits encoder ──────────────────────────────────────────────────────────

def packbits_encode(data: bytes) -> bytes:
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        j = i
        while j < n - 1 and data[j] == data[j + 1] and (j - i) < 127:
            j += 1
        run = j - i + 1
        if run >= 3:
            out.append(((run - 1) & 0x7F) | 0x80)
            out.append(data[i])
            i += run
        else:
            j = i
            while j < n and (j - i) < 128:
                if j + 2 < n and data[j] == data[j + 1] == data[j + 2]:
                    break
                j += 1
            lit = j - i
            out.append(lit - 1)
            out.extend(data[i:i + lit])
            i += lit
    return bytes(out)

# ── gray numpy HxW → 1-bpp bytes ─────────────────────────────────────────────

def gray_to_1bpp(gray: "np.ndarray", dither: bool, invert: bool, threshold: int) -> bytes:
    """gray: (H, W) uint8.  Returns FB_SIZE bytes, 1=white 0=black."""
    if dither and PIL_OK:
        img = Image.fromarray(gray, 'L').convert('1', dither=Image.FLOYDSTEINBERG)
        arr = (np.array(img) > 0).astype(np.uint8)
    else:
        arr = (gray > threshold).astype(np.uint8)
    if invert:
        arr = 1 - arr
    flat = arr.flatten()
    pad = (-len(flat)) % 8
    if pad:
        flat = np.pad(flat, (0, pad), constant_values=1)
    bits = flat[:FB_SIZE * 8].reshape(-1, 8)
    w = np.array([128, 64, 32, 16, 8, 4, 2, 1], dtype=np.uint8)
    return bytes(np.dot(bits, w).astype(np.uint8))

# ── binary packet ─────────────────────────────────────────────────────────────

def make_packet(frame: bytes, enc: int, prev: bytes | None) -> bytes:
    if enc == VS_DRLE:
        if prev is None:
            payload = packbits_encode(frame)
            typ = VS_RLE
        else:
            delta = bytes(a ^ b for a, b in zip(frame, prev))
            payload = packbits_encode(delta)
            typ = VS_DRLE
    elif enc == VS_RLE:
        payload = packbits_encode(frame)
        typ = VS_RLE
    else:
        payload = frame
        typ = VS_RAW
    ln = len(payload)
    return bytes([VS_HDR1, VS_HDR2, typ, (ln >> 8) & 0xFF, ln & 0xFF]) + payload + bytes([VS_FLUSH])

# ── ffmpeg source ─────────────────────────────────────────────────────────────

def ffprobe_fps(path: str) -> float:
    try:
        out = subprocess.check_output(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=r_frame_rate",
             "-of", "default=noprint_wrappers=1:nokey=1", path],
            stderr=subprocess.DEVNULL).decode().strip()
        n, d = (out.split('/') + ['1'])[:2]
        return float(n) / float(d)
    except Exception:
        return 30.0

def ffprobe_dimensions(path: str) -> tuple:
    """Return (width, height) of the first video stream; (0,0) on error."""
    try:
        out = subprocess.check_output(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height",
             "-of", "csv=p=0", path],
            stderr=subprocess.DEVNULL).decode().strip()
        parts = out.split(',')
        return int(parts[0]), int(parts[1])
    except Exception:
        return 0, 0

def build_vf(tw: int, th: int, rotate: int = 0, scale: str = 'fit') -> str:
    """Return an ffmpeg -vf filter string.

    rotate: 0 / 90 / 180 / 270 — clockwise degrees applied BEFORE scaling
    scale modes:
      fit     — keep AR, letterbox / pillarbox with white padding  (default)
      fill    — keep AR, scale to cover, center-crop the overflow
      stretch — stretch to exact target (ignores AR)
    """
    f = []

    if rotate == 90:
        f.append("transpose=1")          # 90° clockwise
    elif rotate == 180:
        f.append("transpose=1")
        f.append("transpose=1")
    elif rotate == 270:
        f.append("transpose=2")          # 90° counter-clockwise

    if scale == 'fill':
        f.append(f"scale={tw}:{th}:force_original_aspect_ratio=increase")
        f.append(f"crop={tw}:{th}:(iw-{tw})/2:(ih-{th})/2")
    elif scale == 'stretch':
        f.append(f"scale={tw}:{th}")
    else:  # 'fit'
        f.append(f"scale={tw}:{th}:force_original_aspect_ratio=decrease")
        f.append(f"pad={tw}:{th}:(ow-iw)/2:(oh-ih)/2:color=white")

    f.append("format=gray")
    return ",".join(f)

def auto_rotate(src_path: str, tw: int, th: int) -> int:
    """Return the rotation (0 or 90) that best fills a portrait/landscape display."""
    sw, sh = ffprobe_dimensions(src_path)
    if sw <= 0 or sh <= 0:
        return 0
    src_is_landscape  = sw > sh
    disp_is_landscape = tw > th
    if src_is_landscape != disp_is_landscape:
        print(f"  auto-rotate: source {sw}×{sh} → rotating 90° to match display {tw}×{th}")
        return 90
    return 0

def ffmpeg_frames(path: str, tw: int, th: int, rotate: int = 0, scale: str = 'fit'):
    """Generator yielding (idx, gray_ndarray HxW uint8) from ffmpeg pipe."""
    vf  = build_vf(tw, th, rotate, scale)
    cmd = ["ffmpeg", "-i", path, "-vf", vf,
           "-f", "rawvideo", "-pix_fmt", "gray", "pipe:1"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    nb = tw * th
    idx = 0
    try:
        while True:
            raw = proc.stdout.read(nb)
            if len(raw) < nb:
                break
            yield idx, np.frombuffer(raw, dtype=np.uint8).reshape(th, tw)
            idx += 1
    finally:
        proc.stdout.close()
        proc.wait()

# ── EBF read/write ────────────────────────────────────────────────────────────

class EBFReader:
    def __init__(self, path: str):
        self._f = open(path, 'rb')
        self._mm = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        hdr = struct.unpack_from(EBF_HDR_FMT, self._mm, 0)
        assert hdr[0] == EBF_MAGIC, f"Not an EBF file: {path}"
        self.width, self.height = hdr[1], hdr[2]
        self.fps = hdr[3]
        self.n_frames = hdr[4]

    def __len__(self):
        return self.n_frames

    def __getitem__(self, idx: int) -> bytes:
        off = EBF_HDR_SIZE + idx * FB_SIZE
        return bytes(self._mm[off:off + FB_SIZE])

    def close(self):
        self._mm.close()
        self._f.close()

    def iter_frames(self):
        for i in range(self.n_frames):
            yield i, self[i]

# ── status line helpers ───────────────────────────────────────────────────────

def fmt_time(seconds: float) -> str:
    s = int(seconds)
    return f"{s//60}:{s%60:02d}"

def status_line(prefix, idx, total, fps_actual, pkt_b, cyc_ms, disp_ms,
                skipped=None, src_fps=None):
    bar_w = 20
    frac = idx / total if total > 0 else 0
    filled = int(bar_w * frac)
    bar = "█" * filled + "░" * (bar_w - filled)

    eta_s = (total - idx) / fps_actual if fps_actual > 0 else 0
    skip_part = f"  skip={skipped}({100*skipped/max(1,idx):.0f}%)" if skipped is not None else ""
    src_part = f"/{src_fps:.0f}" if src_fps else ""
    # cyc = avg ACK-to-ACK period (end-to-end frame cost)
    # disp = device-side share (busy-wait + SPI);  net = the rest (BLE/host)
    net_ms = max(0.0, cyc_ms - disp_ms)
    return (f"\r{prefix} [{bar}] {idx:5d}/{total}"
            f"  {fps_actual:.1f}{src_part}fps{skip_part}"
            f"  cyc~{cyc_ms:3.0f} disp~{disp_ms:3.0f} net~{net_ms:3.0f}ms"
            f"  pkt={pkt_b:5d}B  ETA {fmt_time(eta_s)}")

def print_bottleneck(session):
    """One-line verdict: is the frame rate limited by the display or by BLE?"""
    cyc, disp = session.cycle_ms(), session.disp_ms_avg()
    if cyc <= 0:
        return
    net = max(0.0, cyc - disp)
    if disp >= net:
        verdict = "display-bound: shorter LUT wave is the next win"
    else:
        verdict = "BLE-bound: bigger MTU / faster conn interval is the next win"
    print(f"  cycle {cyc:.0f}ms = display {disp:.0f}ms + ble/host {net:.0f}ms → {verdict}")

# ── BLE session ───────────────────────────────────────────────────────────────

def frame_crc(frame: bytes) -> int:
    """XOR checksum matching the device's vs_flush_frame CRC (all FB_SIZE bytes)."""
    x = 0
    for b in frame:
        x ^= b
    return x


class VStreamSession:
    """Pipelined sender: up to `window` frames may be in flight (sent but not
    yet ACKed). The device processes the byte stream strictly in order and
    ACKs each frame after its SPI write, so the ACK round-trip overlaps with
    the next frame's BLE transfer instead of serializing with it."""

    def __init__(self, client: BleakClient, mtu: int = 240, window: int = 3):
        self.client   = client
        self.mtu      = mtu
        self.window   = window
        self._credits = asyncio.Semaphore(window)
        self._drained = asyncio.Event()
        self._drained.set()
        self._ready   = asyncio.Event()
        self._pending = []          # FIFO of (frame_no, expected_crc) awaiting ACK
        self._buf     = ""
        self.last_ms  = 0           # disp ms from most recent ACK
        self.acked    = 0           # total frames ACKed by device
        self.dead     = False       # set on ACK timeout
        # Rolling diagnostics (last 20 ACKs): where does the frame cycle go?
        self._ack_t   = deque(maxlen=21)   # ACK arrival timestamps
        self._disp_h  = deque(maxlen=20)   # device-reported disp ms

    def cycle_ms(self) -> float:
        """Avg ms between ACKs — the real end-to-end frame period."""
        if len(self._ack_t) < 2:
            return 0.0
        return 1000 * (self._ack_t[-1] - self._ack_t[0]) / (len(self._ack_t) - 1)

    def disp_ms_avg(self) -> float:
        """Avg device-side ms (busy-wait for previous wave + SPI + trigger)."""
        return sum(self._disp_h) / len(self._disp_h) if self._disp_h else 0.0

    def _notify(self, _h, data: bytearray):
        self._buf += data.decode('utf-8', errors='replace')
        while '\n' in self._buf:
            line, self._buf = self._buf.split('\n', 1)
            line = line.strip()
            if line.startswith("TELE:vs"):
                self._on_ack(line)
            elif line:
                if line.startswith("VSTREAM:ready"):
                    self._ready.set()
                print(f"\n  dev> {line}")

    def _on_ack(self, line: str):
        try:
            self.last_ms = int(line.split("ms=")[1].split()[0])
        except Exception:
            pass
        try:
            dec = int(line.split("dec=")[1].split()[0])
        except Exception:
            dec = -1
        try:
            crc = int(line.split("crc=")[1].split()[0], 16)
        except Exception:
            crc = -1

        if dec >= 0 and dec != FB_SIZE:
            print(f"\n  [WARN] incomplete decode: dec={dec}/{FB_SIZE}")
        if self._pending:
            frame_no, exp_crc = self._pending.pop(0)
            if crc >= 0 and exp_crc is not None and crc != exp_crc:
                print(f"\n  [CRC MISMATCH] frame {frame_no}: "
                      f"expected={exp_crc:02x} device={crc:02x}")
        self.acked += 1
        self._ack_t.append(time.time())
        self._disp_h.append(self.last_ms)
        self._credits.release()
        if not self._pending:
            self._drained.set()

    async def start(self, pre_cmds=()):
        await self.client.start_notify(NUS_TX, self._notify)
        # Setup commands (e.g. LUTW: video LUT) must land before VSTREAM:start —
        # the device loads the active LUT while priming the display.
        for cmd in pre_cmds:
            await self._raw(cmd)
            await asyncio.sleep(0.3)   # device reply prints as a dev> line
        self._ready.clear()
        await self._raw(b"VSTREAM:start\n")
        # Device primes the display before replying ready (deep-sleep wake +
        # HV charge can take ~1s); don't start blasting frames until then.
        try:
            await asyncio.wait_for(self._ready.wait(), timeout=10.0)
        except asyncio.TimeoutError:
            print("  [WARN] no VSTREAM:ready within 10s — old firmware? continuing anyway")

    async def stop(self):
        await self._raw(VS_STOP)
        await asyncio.sleep(0.3)
        try:
            await self.client.stop_notify(NUS_TX)
        except Exception:
            pass

    async def _raw(self, data: bytes):
        for off in range(0, len(data), self.mtu):
            await self.client.write_gatt_char(NUS_RX, data[off:off + self.mtu], response=False)
            await asyncio.sleep(0)

    async def send(self, pkt: bytes, timeout: float = 15.0,
                   expected_frame: bytes | None = None) -> bool:
        """Queue one frame. Blocks only when the in-flight window is full
        (i.e. the display is the bottleneck). Returns False on ACK timeout."""
        try:
            await asyncio.wait_for(self._credits.acquire(), timeout=timeout)
        except asyncio.TimeoutError:
            print("\n  [WARNING] ACK timeout — is device still connected?")
            self.dead = True
            return False
        exp_crc = frame_crc(expected_frame) if expected_frame is not None else None
        self._pending.append((self.acked + len(self._pending) + 1, exp_crc))
        self._drained.clear()
        await self._raw(pkt)
        return True

    async def drain(self, timeout: float = 15.0) -> bool:
        """Wait until every queued frame has been ACKed (end of playback)."""
        try:
            await asyncio.wait_for(self._drained.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            print(f"\n  [WARNING] drain timeout — {len(self._pending)} frames unACKed")
            return False

# ── playback engines ──────────────────────────────────────────────────────────

async def play_guaranteed(session: VStreamSession, frames, total: int,
                          enc: int, src_fps: float):
    """Send every frame; pace is dictated by display speed only."""
    prev, count = None, 0
    acked0 = session.acked          # session persists across --loop iterations
    t0 = time.time()
    try:
        for idx, frame in frames:
            if frame is None:
                continue
            pkt = make_packet(frame, enc, prev)
            if not await session.send(pkt, expected_frame=frame):
                break
            prev = frame
            count += 1
            elapsed = time.time() - t0
            fps = (session.acked - acked0) / elapsed if elapsed > 0 else 0
            print(status_line("G", count, total, fps, len(pkt),
                              session.cycle_ms(), session.disp_ms_avg(),
                              src_fps=src_fps),
                  end='', flush=True)
        await session.drain()
    except KeyboardInterrupt:
        pass
    elapsed = time.time() - t0
    fps = (session.acked - acked0) / elapsed if elapsed > 0 else 0
    print(f"\n  done: {count} frames in {elapsed:.1f}s = {fps:.2f}fps")
    print_bottleneck(session)

async def play_sync(session: VStreamSession, frames, total: int,
                    enc: int, src_fps: float):
    """Real-time sync to original timestamps; skip frames that are already late."""
    SKIP_TOL = 0.010   # 10ms tolerance before marking a frame as late
    prev = None
    count, skipped = 0, 0
    acked0 = session.acked          # session persists across --loop iterations
    t0 = time.time()
    try:
        for idx, frame in frames:
            deadline = t0 + idx / src_fps
            now = time.time()

            if now > deadline + SKIP_TOL:
                # Behind schedule — skip without encoding
                skipped += 1
                # Do NOT update prev: device still shows the last sent frame
                elapsed = now - t0
                fps = (session.acked - acked0) / elapsed if elapsed > 0 else 0
                print(status_line("S", count + skipped, total, fps, 0,
                                  session.cycle_ms(), session.disp_ms_avg(),
                                  skipped, src_fps),
                      end='', flush=True)
                continue

            if frame is None:
                continue

            # Ahead of schedule (pipelined sends return immediately) — hold
            # until this frame's timestamp so slow sources play at real speed.
            if now < deadline:
                await asyncio.sleep(deadline - now)

            pkt = make_packet(frame, enc, prev)
            if not await session.send(pkt, expected_frame=frame):
                break
            prev = frame
            count += 1
            elapsed = time.time() - t0
            fps = (session.acked - acked0) / elapsed if elapsed > 0 else 0
            print(status_line("S", count + skipped, total, fps, len(pkt),
                              session.cycle_ms(), session.disp_ms_avg(),
                              skipped, src_fps),
                  end='', flush=True)

        await session.drain()
    except KeyboardInterrupt:
        pass
    elapsed = time.time() - t0
    fps = (session.acked - acked0) / elapsed if elapsed > 0 else 0
    shown_pct = 100 * count / (count + skipped) if (count + skipped) > 0 else 0
    print(f"\n  done: {count} shown, {skipped} skipped ({shown_pct:.0f}% shown)"
          f" in {elapsed:.1f}s = {fps:.2f}fps displayed")
    print_bottleneck(session)

# ── connect + play wrapper ────────────────────────────────────────────────────

async def run_play(args):
    if not BLEAK_OK:
        print("pip install bleak"); sys.exit(1)
    if not NP_OK:
        print("pip install numpy"); sys.exit(1)

    path = Path(args.file)
    enc  = ENC_NAMES.get(args.enc.lower(), VS_DRLE)
    tw, th = (DISP_H, DISP_W) if args.landscape else (DISP_W, DISP_H)

    rot   = getattr(args, 'rotate', 0)
    scale = getattr(args, 'scale', 'fit')

    is_ebf = path.suffix.lower() == '.ebf'

    # Determine source fps
    if is_ebf:
        ebf = EBFReader(str(path))
        src_fps   = ebf.fps
        n_frames  = ebf.n_frames
        file_mb   = path.stat().st_size / 1e6
        print(f"EBF  {n_frames} frames  {src_fps:.2f}fps  {file_mb:.1f}MB")
    else:
        if not which_ffmpeg():
            print("ffmpeg not found — install with: brew install ffmpeg")
            sys.exit(1)
        if getattr(args, 'auto_rotate', False):
            rot = auto_rotate(str(path), tw, th)
        src_fps  = ffprobe_fps(str(path))
        sw, sh   = ffprobe_dimensions(str(path))
        dur = ffprobe_duration(str(path))
        n_frames = int(dur * src_fps) if dur else 0
        print(f"Video  {sw}×{sh}  ~{n_frames} frames  {src_fps:.2f}fps"
              f"  rotate={rot}°  scale={scale}")

    override_fps = args.fps if args.fps > 0 else src_fps

    # Compression preview from mid-video (more representative than title frames)
    if is_ebf and n_frames >= 20:
        BLE_BPS = 32_000
        mid = max(10, n_frames // 4)
        S = min(20, n_frames - mid - 1)
        samp_rle  = sum(len(packbits_encode(ebf[mid+i])) for i in range(S)) / S
        samp_drle = sum(len(packbits_encode(bytes(a^b for a,b in zip(ebf[mid+i], ebf[mid+i-1]))))
                        for i in range(1, S)) / (S-1)
        # Pipelined: BLE transfer overlaps the display refresh, so the
        # per-frame cost is max(LUT wave, transfer) + SPI write (~7ms).
        fps_rle  = 1000 / (max(64, 1000 * samp_rle  / BLE_BPS) + 7)
        fps_drle = 1000 / (max(64, 1000 * samp_drle / BLE_BPS) + 7)
        print(f"  rle~{samp_rle:.0f}B({fps_rle:.1f}fps)  drle~{samp_drle:.0f}B({fps_drle:.1f}fps)"
              f"  raw={FB_SIZE}B")

    # Connect BLE
    print(f"\nScanning for '{args.device}'...")
    dev = None
    if args.address:
        dev = await BleakScanner.find_device_by_address(args.address, timeout=12)
    else:
        dev = await find_device_by_selector(args.device, timeout=12)
    if not dev:
        print("Device not found"); sys.exit(1)
    print(f"Connecting {dev.name} ({dev.address})...")

    async with BleakClient(dev) as client:
        mtu_size = getattr(client, 'mtu_size', 247)
        mtu = max(20, mtu_size - 3)
        print(f"Connected  MTU={mtu}B  mode={args.mode}  enc={args.enc}"
              f"  window={args.window}  dither={args.dither}"
              f"  rotate={rot}°  scale={scale}\n")

        session = VStreamSession(client, mtu=mtu, window=args.window)
        pre_cmds = []
        if args.lut == 'video':
            pre_cmds.append(b"LUTW:" + LUT_VIDEO.hex().upper().encode() + b"\n")
        elif args.lut == 'turbo':
            pre_cmds.append(b"LUTUSE:0\n")   # builtin TURBO (112ms wave)
        # 'keep': don't touch whatever LUT the device currently uses
        await session.start(pre_cmds)

        loop_n = 0
        try:
            while True:
                loop_n += 1
                if args.loop:
                    print(f"── Loop {loop_n} ──────────────────")

                if is_ebf:
                    # sync mode: use all frames (original indices) — deadline logic handles skipping
                    # guaranteed mode: pre-filter to --fps target
                    if args.mode == 'sync':
                        ebf_step, gen_total = 1, n_frames
                    else:
                        ebf_step = max(1, round(src_fps / override_fps)) if override_fps < src_fps else 1
                        gen_total = (n_frames + ebf_step - 1) // ebf_step

                    def make_ebf_gen(step=ebf_step):
                        for i in range(0, n_frames, step):
                            yield i, ebf[i]

                    frame_gen = make_ebf_gen()
                else:
                    # stream from ffmpeg, convert on-the-fly
                    raw_gen = ffmpeg_frames(str(path), tw, th, rot, scale)
                    dither_v, invert_v, thr_v = args.dither, args.invert, args.threshold

                    def make_video_gen(rg):
                        for i, gray in rg:
                            yield i, gray_to_1bpp(gray, dither_v, invert_v, thr_v)

                    frame_gen = make_video_gen(raw_gen)
                    gen_total = n_frames

                if args.mode == 'guaranteed':
                    await play_guaranteed(session, frame_gen, gen_total, enc, override_fps)
                else:
                    await play_sync(session, frame_gen, gen_total, enc, override_fps)

                if is_ebf:
                    pass  # EBFReader is reusable
                else:
                    pass  # ffmpeg process already finished

                if not args.loop:
                    break

        except KeyboardInterrupt:
            print("\nStopped.")
        finally:
            print("Sending stop...")
            await session.stop()

        if is_ebf:
            ebf.close()

# ── convert command ───────────────────────────────────────────────────────────

def cmd_convert(args):
    if not NP_OK:
        print("pip install numpy"); sys.exit(1)
    if not PIL_OK:
        print("pip install pillow"); sys.exit(1)
    if not which_ffmpeg():
        print("ffmpeg not found — brew install ffmpeg"); sys.exit(1)

    path = Path(args.input)
    out  = Path(args.output) if args.output else path.with_suffix('.ebf')
    tw, th = (DISP_H, DISP_W) if args.landscape else (DISP_W, DISP_H)

    rot   = getattr(args, 'rotate', 0)
    scale = getattr(args, 'scale', 'fit')
    if getattr(args, 'auto_rotate', False):
        rot = auto_rotate(str(path), tw, th)

    src_fps = ffprobe_fps(str(path))
    sw, sh  = ffprobe_dimensions(str(path))
    print(f"Source: {path.name}  {sw}×{sh}  fps={src_fps:.2f}")
    print(f"Target: {tw}×{th}  rotate={rot}°  scale={scale}"
          f"  dither={args.dither}  threshold={args.threshold}")

    # Two-pass: write header with n_frames=0, fill in at end
    f = open(out, 'wb')
    f.write(struct.pack(EBF_HDR_FMT, EBF_MAGIC, tw, th, src_fps, 0))

    n = 0
    t0 = time.time()
    prev_1bpp = None
    total_compressed = 0   # track compression for report

    for idx, gray in ffmpeg_frames(str(path), tw, th, rot, scale):
        frame_1bpp = gray_to_1bpp(gray, args.dither, args.invert, args.threshold)
        f.write(frame_1bpp)
        n += 1

        # progress
        elapsed = time.time() - t0
        fps = n / elapsed if elapsed > 0 else 0
        print(f"\r  {n} frames  {fps:.1f}fps  {elapsed:.0f}s", end='', flush=True)

    # Update header with actual n_frames
    f.seek(12)
    f.write(struct.pack('<I', n))
    f.close()

    elapsed = time.time() - t0
    size_mb = out.stat().st_size / 1e6
    print(f"\nConverted {n} frames → {out}  ({size_mb:.1f}MB)  in {elapsed:.1f}s")

    # Compression preview (sample from ~25% into the video = more representative)
    if n >= 20:
        ebf = EBFReader(str(out))
        mid = max(10, n // 4)
        S = min(30, n - mid - 1)
        BLE_BPS = 32_000  # realistic Nordic NUS with DLE, ~32 KB/s
        DISP_MS = 64      # turbo partial update

        samp_rle  = sum(len(packbits_encode(ebf[mid + i])) for i in range(S)) / S
        samp_drle = sum(len(packbits_encode(
            bytes(a ^ b for a, b in zip(ebf[mid + i], ebf[mid + i - 1]))))
            for i in range(1, S)) / (S - 1)
        ebf.close()

        def est_fps(b):
            # Pipelined: transfer overlaps refresh → max(), plus ~7ms SPI write
            return 1000 / (max(DISP_MS, 1000 * b / BLE_BPS) + 7)

        print(f"\nCompression sample (~frame {mid}, {S} frames):")
        print(f"  raw   {FB_SIZE:5d}B/frame")
        print(f"  rle   {samp_rle:5.0f}B/frame  est ~{est_fps(samp_rle):.1f}fps")
        print(f"  drle  {samp_drle:5.0f}B/frame  est ~{est_fps(samp_drle):.1f}fps")
        print(f"\nPlay with:")
        print(f"  python vstream.py play {out} --mode sync --enc drle --loop")
        print(f"  python vstream.py play {out} --mode guaranteed --enc drle")

# ── utilities ─────────────────────────────────────────────────────────────────

def which_ffmpeg() -> bool:
    try:
        subprocess.check_output(["ffmpeg", "-version"], stderr=subprocess.DEVNULL)
        return True
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False

def ffprobe_duration(path: str) -> float:
    # format-level duration works for webm/av1 where stream duration is N/A
    for scope in ("format=duration", "stream=duration"):
        extra = ["-select_streams", "v:0"] if scope.startswith("stream") else []
        try:
            out = subprocess.check_output(
                ["ffprobe", "-v", "error"] + extra +
                ["-show_entries", scope,
                 "-of", "default=noprint_wrappers=1:nokey=1", path],
                stderr=subprocess.DEVNULL).decode().strip().split("\n")[0]
            if out and "N/A" not in out:
                return float(out)
        except Exception:
            pass
    return 0.0

# ── CLI ───────────────────────────────────────────────────────────────────────

def build_parser():
    p = argparse.ArgumentParser(
        prog="vstream",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    # ── convert ──────────────────────────────────────────────────────────────
    c = sub.add_parser("convert", help="Convert video → .ebf")
    c.add_argument("input",  help="Input video file")
    c.add_argument("output", nargs="?", help="Output .ebf path (default: same name)")
    c.add_argument("--landscape",   action="store_true", help="296×128 instead of 128×296")
    c.add_argument("--rotate",      type=int, default=0, choices=[0, 90, 180, 270],
                   help="Rotate source clockwise before scaling (default: 0)")
    c.add_argument("--auto-rotate", action="store_true",
                   help="Auto-detect landscape/portrait mismatch and rotate 90° to fill display")
    c.add_argument("--scale",       default="fit", choices=["fit", "fill", "stretch"],
                   help="fit=letterbox(default)  fill=crop-to-cover  stretch=distort")
    c.add_argument("--dither",      action="store_true", help="Floyd-Steinberg dithering")
    c.add_argument("--invert",      action="store_true", help="Invert B/W")
    c.add_argument("--threshold",   type=int, default=127,
                   help="B/W threshold 0-255 (default 127)")

    # ── play ─────────────────────────────────────────────────────────────────
    pl = sub.add_parser("play", help="Play .ebf or video on display")
    pl.add_argument("file", help="Input .ebf or video file")
    pl.add_argument("--device",     default=DEFAULT_DEVICE_SELECTOR,
                    help="BLE device name or prefix wildcard, e.g. nrf52-E-ink-clock-*")
    pl.add_argument("--address",    default=None, help="BLE address (overrides --device)")
    pl.add_argument("--mode",       default="guaranteed",
                    choices=["guaranteed", "sync"],
                    help="guaranteed=every frame  sync=real-time+skip (default: guaranteed)")
    pl.add_argument("--enc",        default="drle",
                    choices=["raw", "rle", "drle"],
                    help="Encoding (default: drle)")
    pl.add_argument("--fps",        type=float, default=0,
                    help="Override playback fps (0=use source fps)")
    pl.add_argument("--landscape",  action="store_true", help="296×128 mode")
    pl.add_argument("--rotate",     type=int, default=0, choices=[0, 90, 180, 270],
                    help="Rotate source clockwise before scaling (video only, default: 0)")
    pl.add_argument("--auto-rotate", action="store_true",
                    help="Auto-detect landscape/portrait mismatch and rotate 90° (video only)")
    pl.add_argument("--scale",      default="fit", choices=["fit", "fill", "stretch"],
                    help="fit=letterbox(default)  fill=crop-to-cover  stretch=distort (video only)")
    pl.add_argument("--dither",     action="store_true",
                    help="Floyd-Steinberg dither (video source only, EBF ignores)")
    pl.add_argument("--invert",     action="store_true", help="Invert B/W")
    pl.add_argument("--threshold",  type=int, default=127,
                    help="B/W threshold (video source only, default 127)")
    pl.add_argument("--window",     type=int, default=3,
                    help="Frames in flight before waiting for ACK (default 3, 1=old stop-and-wait)")
    pl.add_argument("--lut",        default="video", choices=["video", "turbo", "keep"],
                    help="video=upload 64ms LUT (fast, default)  turbo=builtin 112ms"
                         "  keep=use whatever device has")
    pl.add_argument("--loop",       action="store_true", help="Loop forever")

    return p

def main():
    p = build_parser()
    args = p.parse_args()

    if args.cmd == "convert":
        cmd_convert(args)
    elif args.cmd == "play":
        asyncio.run(run_play(args))

if __name__ == "__main__":
    main()
