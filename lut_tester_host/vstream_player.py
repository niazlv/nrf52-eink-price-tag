#!/usr/bin/env python3
"""
vstream_player.py — stream video/GIF to nRF52 e-ink display over BLE NUS.

Protocol (device side: VSTREAM: command):
  1. Send text "VSTREAM:start\n"  → device replies "VSTREAM:ready..."
  2. For each frame, send binary packet:
       [0xAA][0x55][type][len_hi][len_lo][payload...][0xBB]
     type: 0x00=RAW  0x01=RLE(PackBits)  0x02=DRLE(XOR-delta + PackBits)
  3. Device replies "TELE:vs f=N ms=M\r\n" after each flush (ACK for flow control)
  4. Send [0xCC][0xDD] to stop streaming mode

Usage:
  python vstream_player.py bad_apple.gif
  python vstream_player.py bad_apple.mp4 --fps 12 --enc drle --dither
  python vstream_player.py bad_apple.gif --device "nrf52-E-ink-clock-*" --loop

Deps:  pip install bleak pillow
Opt:   pip install numpy opencv-python   (faster processing + video file support)
"""

import asyncio
import argparse
import struct
import time
import sys
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("pip install bleak"); sys.exit(1)
try:
    from PIL import Image
except ImportError:
    print("pip install pillow"); sys.exit(1)

try:
    import numpy as np
    NUMPY_OK = True
except ImportError:
    NUMPY_OK = False

try:
    import cv2
    CV2_OK = True
except ImportError:
    CV2_OK = False

# ── NUS UUIDs ─────────────────────────────────────────────────────────────────
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write → device
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify ← device
DEFAULT_DEVICE_SELECTOR = "nrf52-E-ink-clock-*"

# ── Display geometry ──────────────────────────────────────────────────────────
DISP_W  = 128
DISP_H  = 296
FB_SIZE = DISP_W * DISP_H // 8  # 4736 bytes

# ── Protocol bytes ────────────────────────────────────────────────────────────
VS_HDR   = bytes([0xAA, 0x55])
VS_FLUSH = bytes([0xBB])
VS_STOP  = bytes([0xCC, 0xDD])
VS_RAW   = 0x00
VS_RLE   = 0x01
VS_DRLE  = 0x02

def adv_has_nus(adv) -> bool:
    return NUS_SERVICE in [u.lower() for u in (getattr(adv, "service_uuids", None) or [])]

async def find_device_by_selector(selector: str, timeout: float = 10.0):
    if selector.endswith("*"):
        prefix = selector[:-1]
        device = await BleakScanner.find_device_by_filter(
            lambda dev, adv: ((dev.name or "").startswith(prefix) or
                              (getattr(adv, "local_name", None) or "").startswith(prefix)),
            timeout=timeout)
        if device:
            return device
        return await BleakScanner.find_device_by_filter(
            lambda dev, adv: adv_has_nus(adv),
            timeout=3.0)
    return await BleakScanner.find_device_by_name(selector, timeout=timeout)

# ── PackBits encoder ──────────────────────────────────────────────────────────

def packbits_encode(data: bytes) -> bytes:
    """PackBits RLE.
      ctrl bit7=1: next byte × (ctrl&0x7F)+1 times
      ctrl bit7=0: next (ctrl+1) bytes literal
    """
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        # Measure run length at i
        j = i
        while j < n - 1 and data[j] == data[j + 1] and (j - i) < 127:
            j += 1
        run = j - i + 1
        if run >= 3:
            out.append(((run - 1) & 0x7F) | 0x80)
            out.append(data[i])
            i += run
        else:
            # Collect literal bytes until a run of 3+ begins
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

# ── Frame encoder ─────────────────────────────────────────────────────────────

def frame_to_1bpp(img: Image.Image, dither: bool, invert: bool, landscape: bool) -> bytes:
    tw, th = (DISP_H, DISP_W) if landscape else (DISP_W, DISP_H)

    img = img.convert('L')
    # Scale preserving aspect, then letterbox center
    img.thumbnail((tw, th), Image.LANCZOS)
    bg = Image.new('L', (tw, th), 255)
    bg.paste(img, ((tw - img.width) // 2, (th - img.height) // 2))
    img = bg

    if dither:
        img = img.convert('1', dither=Image.FLOYDSTEINBERG)
    else:
        img = img.point(lambda p: 255 if p > 127 else 0)

    if NUMPY_OK:
        arr = np.array(img, dtype=np.uint8)
        if arr.max() > 1:
            arr = (arr > 127).astype(np.uint8)
        if invert:
            arr = 1 - arr
        flat = arr.flatten()[:FB_SIZE * 8]
        if len(flat) < FB_SIZE * 8:
            flat = np.pad(flat, (0, FB_SIZE * 8 - len(flat)), constant_values=1)
        bits = flat.reshape(-1, 8)
        weights = np.array([128, 64, 32, 16, 8, 4, 2, 1], dtype=np.uint8)
        buf = np.dot(bits, weights).astype(np.uint8)
        return bytes(buf[:FB_SIZE])
    else:
        buf = bytearray(FB_SIZE)
        px_get = img.getpixel
        for idx in range(tw * th):
            x, y = idx % tw, idx // tw
            px = px_get((x, y))
            white = (px > 127) if isinstance(px, int) else (px[0] > 127)
            if white ^ invert:
                buf[idx // 8] |= 0x80 >> (idx % 8)
        return bytes(buf[:FB_SIZE])

# ── Binary packet assembly ────────────────────────────────────────────────────

def make_packet(frame: bytes, enc_type: int, prev_frame: bytes | None) -> bytes:
    if enc_type == VS_DRLE:
        if prev_frame is None:
            # First frame: send as RLE since no delta available
            payload = packbits_encode(frame)
            actual_type = VS_RLE
        else:
            delta = bytes(a ^ b for a, b in zip(frame, prev_frame))
            payload = packbits_encode(delta)
            actual_type = VS_DRLE
    elif enc_type == VS_RLE:
        payload = packbits_encode(frame)
        actual_type = VS_RLE
    else:
        payload = frame
        actual_type = VS_RAW

    length = len(payload)
    header = VS_HDR + bytes([actual_type, (length >> 8) & 0xFF, length & 0xFF])
    return header + payload + VS_FLUSH

# ── Video/GIF frame loader ────────────────────────────────────────────────────

def load_frames_gif(path: str):
    img = Image.open(path)
    frames = []
    try:
        while True:
            frames.append(img.copy())
            img.seek(img.tell() + 1)
    except EOFError:
        pass
    return frames

def load_frames_cv2(path: str, max_fps: float):
    if not CV2_OK:
        print("pip install opencv-python for video files")
        sys.exit(1)
    cap = cv2.VideoCapture(path)
    src_fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    step = max(1, round(src_fps / max_fps)) if max_fps > 0 else 1
    frames = []
    idx = 0
    while True:
        ret, bgr = cap.read()
        if not ret:
            break
        if idx % step == 0:
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            frames.append(Image.fromarray(rgb))
        idx += 1
    cap.release()
    return frames

# ── BLE streaming ─────────────────────────────────────────────────────────────

class VStreamSession:
    def __init__(self, client: BleakClient, mtu: int = 244):
        self.client = client
        self.mtu = mtu
        self._ack_event = asyncio.Event()
        self._rx_buf = ""
        self.last_ack_ms: int = 0

    def _on_notify(self, _handle, data: bytearray):
        self._rx_buf += data.decode('utf-8', errors='replace')
        while '\n' in self._rx_buf:
            line, self._rx_buf = self._rx_buf.split('\n', 1)
            line = line.strip()
            if line.startswith("TELE:vs"):
                try:
                    self.last_ack_ms = int(line.split("ms=")[1].split()[0])
                except Exception:
                    pass
                self._ack_event.set()
            else:
                print(f"  dev: {line}")

    async def start(self):
        await self.client.start_notify(NUS_TX, self._on_notify)
        await self._send_text("VSTREAM:start\n")
        # Wait for "VSTREAM:ready" reply
        await asyncio.sleep(0.3)

    async def stop(self):
        await self._send_bytes(VS_STOP)
        await asyncio.sleep(0.2)
        await self.client.stop_notify(NUS_TX)

    async def _send_bytes(self, data: bytes):
        for off in range(0, len(data), self.mtu):
            chunk = data[off:off + self.mtu]
            await self.client.write_gatt_char(NUS_RX, chunk, response=False)

    async def _send_text(self, text: str):
        await self._send_bytes(text.encode())

    async def send_frame(self, packet: bytes, timeout: float = 5.0) -> int:
        self._ack_event.clear()
        await self._send_bytes(packet)
        try:
            await asyncio.wait_for(self._ack_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            print("  WARNING: ACK timeout")
            return -1
        return self.last_ack_ms

# ── Main ──────────────────────────────────────────────────────────────────────

async def run(args):
    path = Path(args.file)
    enc_map = {'raw': VS_RAW, 'rle': VS_RLE, 'drle': VS_DRLE}
    enc_type = enc_map.get(args.enc.lower(), VS_DRLE)
    landscape = args.landscape
    dither = args.dither
    invert = args.invert
    target_fps = args.fps

    print(f"Loading frames from {path}...")
    suffix = path.suffix.lower()
    if suffix == '.gif':
        raw_frames = load_frames_gif(str(path))
        print(f"  {len(raw_frames)} GIF frames")
    elif suffix in ('.mp4', '.avi', '.mov', '.mkv', '.webm'):
        raw_frames = load_frames_cv2(str(path), target_fps)
        print(f"  {len(raw_frames)} video frames (after fps filter)")
    else:
        print(f"Unknown format {suffix} — trying PIL")
        raw_frames = [Image.open(str(path))]

    print(f"Encoding to 1bpp ({'landscape' if landscape else 'portrait'}, "
          f"{'dither' if dither else 'threshold'}, "
          f"{'inverted, ' if invert else ''}"
          f"enc={args.enc})...")
    t0 = time.time()
    encoded_frames: list[bytes] = []
    for i, img in enumerate(raw_frames):
        encoded_frames.append(frame_to_1bpp(img, dither, invert, landscape))
        if (i + 1) % 50 == 0:
            print(f"  {i + 1}/{len(raw_frames)}...")
    print(f"  done in {time.time()-t0:.1f}s")

    # Compute compression stats on first few frames
    if len(encoded_frames) >= 2:
        sample_rle  = sum(len(packbits_encode(f)) for f in encoded_frames[:10]) / 10
        deltas = [bytes(a ^ b for a, b in zip(encoded_frames[i], encoded_frames[i-1]))
                  for i in range(1, min(11, len(encoded_frames)))]
        sample_drle = sum(len(packbits_encode(d)) for d in deltas) / len(deltas)
        print(f"  raw={FB_SIZE}B  rle~{int(sample_rle)}B ({100*sample_rle/FB_SIZE:.0f}%)"
              f"  drle~{int(sample_drle)}B ({100*sample_drle/FB_SIZE:.0f}%)")

    # BLE connect
    print(f"\nScanning for '{args.device}'...")
    device = None
    if args.address:
        device = await BleakScanner.find_device_by_address(args.address, timeout=10)
    else:
        device = await find_device_by_selector(args.device, timeout=10)
    if not device:
        print("Device not found"); sys.exit(1)
    print(f"Connecting to {device.name} ({device.address})...")

    async with BleakClient(device) as client:
        print("Connected. Negotiating MTU...")
        if hasattr(client, 'mtu_size'):
            mtu = max(20, client.mtu_size - 3)
        else:
            mtu = 244
        print(f"  MTU payload: {mtu}B")

        session = VStreamSession(client, mtu=mtu)
        await session.start()
        print(f"Streaming {len(encoded_frames)} frames (enc={args.enc}, loop={args.loop})...\n")

        loop_count = 0
        try:
            while True:
                prev_frame = None
                frame_times = []

                for idx, frame in enumerate(encoded_frames):
                    packet = make_packet(frame, enc_type, prev_frame)
                    t_send = time.time()
                    ack_ms = await session.send_frame(packet)
                    elapsed = (time.time() - t_send) * 1000
                    frame_times.append(elapsed)
                    prev_frame = frame

                    # Progress every 10 frames
                    if (idx + 1) % 10 == 0:
                        avg = sum(frame_times[-10:]) / 10
                        fps = 1000 / avg if avg > 0 else 0
                        pkt_sz = len(packet)
                        print(f"  frame {idx+1:4d}/{len(encoded_frames)}  "
                              f"disp={ack_ms:3d}ms  total={avg:.0f}ms  "
                              f"~{fps:.1f}fps  pkt={pkt_sz}B")

                loop_count += 1
                total_avg = sum(frame_times) / len(frame_times) if frame_times else 0
                print(f"\nLoop {loop_count} done. avg={total_avg:.0f}ms/frame "
                      f"(~{1000/total_avg:.1f}fps)\n")

                if not args.loop:
                    break

        except KeyboardInterrupt:
            print("\nInterrupted")
        finally:
            print("Stopping vstream...")
            await session.stop()
            print("Done.")

def main():
    p = argparse.ArgumentParser(description="Stream video/GIF to e-ink via BLE vstream")
    p.add_argument("file", help="Input file (GIF, MP4, AVI, etc.)")
    p.add_argument("--device",  default=DEFAULT_DEVICE_SELECTOR,
                   help="BLE device name or prefix wildcard, e.g. nrf52-E-ink-clock-*")
    p.add_argument("--address", default=None, help="BLE address (overrides --device)")
    p.add_argument("--enc",     default="drle", choices=["raw","rle","drle"],
                   help="Frame encoding: raw|rle|drle (default: drle)")
    p.add_argument("--fps",     type=float, default=0,
                   help="Max source fps to load (0=all frames)")
    p.add_argument("--dither",  action="store_true", help="Floyd-Steinberg dither")
    p.add_argument("--invert",  action="store_true", help="Invert black/white")
    p.add_argument("--landscape", action="store_true",
                   help="Scale to 296×128 (set ROT:1 on device first)")
    p.add_argument("--loop",    action="store_true", help="Loop animation")
    args = p.parse_args()
    asyncio.run(run(args))

if __name__ == "__main__":
    main()
