#!/usr/bin/env python3
"""
OTA DFU upload tool for nRF52 via BLE (mcumgr SMP protocol).

Usage:
  python dfu_upload.py                         # scan & select device interactively
  python dfu_upload.py app_update.bin          # upload specific file
  python dfu_upload.py -d "nrf52" fw.bin       # filter by device name
  python dfu_upload.py --confirm               # confirm current image
  python dfu_upload.py --reset                 # reset device

Requirements:
  pip install bleak cbor2
"""

import argparse
import asyncio
import hashlib
import struct
import sys
import time
from pathlib import Path

try:
    import cbor2
except ImportError:
    print("ERROR: cbor2 not installed. Run: pip install cbor2")
    sys.exit(1)

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("ERROR: bleak not installed. Run: pip install bleak")
    sys.exit(1)


# SMP BLE UUIDs
SMP_SVC_UUID = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHAR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"

# SMP protocol constants
SMP_OP_READ = 0
SMP_OP_READ_RSP = 1
SMP_OP_WRITE = 2
SMP_OP_WRITE_RSP = 3

SMP_GRP_OS = 0
SMP_GRP_IMG = 1

IMG_CMD_STATE = 0
IMG_CMD_UPLOAD = 1

OS_CMD_RESET = 5


class SMPClient:
    """SMP over BLE client for mcumgr firmware management."""

    def __init__(self, client: BleakClient):
        self.client = client
        self.seq = 0
        self.response_event = asyncio.Event()
        self.response_data = None
        self._reassembly_buf = bytearray()
        self._expected_len = 0

    async def start(self):
        """Start notifications on SMP characteristic."""
        await self.client.start_notify(SMP_CHAR_UUID, self._on_notify)

    def _on_notify(self, sender, data: bytearray):
        """Handle incoming SMP notification fragments."""
        if not self._reassembly_buf:
            # First fragment — has 8-byte SMP header
            if len(data) < 8:
                return
            payload_len = struct.unpack(">H", data[2:4])[0]
            self._expected_len = 8 + payload_len
            self._reassembly_buf = bytearray(data)
        else:
            self._reassembly_buf.extend(data)

        if len(self._reassembly_buf) >= self._expected_len:
            # Complete response received
            payload = bytes(self._reassembly_buf[8:self._expected_len])
            self._reassembly_buf = bytearray()
            self._expected_len = 0
            try:
                self.response_data = cbor2.loads(payload)
            except Exception as e:
                self.response_data = {"rc": -1, "error": str(e)}
            self.response_event.set()

    def _build_header(self, op: int, group: int, cmd_id: int, payload: bytes) -> bytes:
        """Build 8-byte SMP header."""
        hdr = struct.pack(
            ">BBHHBB",
            op,            # op
            0,             # flags
            len(payload),  # payload length
            group,         # group
            self.seq,      # seq
            cmd_id,        # command id
        )
        self.seq = (self.seq + 1) & 0xFF
        return hdr

    async def request(self, op: int, group: int, cmd_id: int, payload_obj: dict, timeout: float = 30.0) -> dict:
        """Send SMP request and wait for response."""
        payload = cbor2.dumps(payload_obj)
        hdr = self._build_header(op, group, cmd_id, payload)
        msg = hdr + payload

        self.response_event.clear()
        self.response_data = None

        # Send in BLE MTU-sized chunks
        mtu = self.client.mtu_size - 3 if hasattr(self.client, 'mtu_size') else 240
        chunk_size = min(mtu, 240)
        for i in range(0, len(msg), chunk_size):
            chunk = msg[i:i + chunk_size]
            await self.client.write_gatt_char(SMP_CHAR_UUID, chunk, response=False)

        # Wait for response
        try:
            await asyncio.wait_for(self.response_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            raise TimeoutError(f"SMP response timeout ({timeout}s)")

        return self.response_data or {}

    async def image_upload(self, firmware: bytes, progress_cb=None) -> None:
        """Upload firmware image to slot 1."""
        total = len(firmware)
        sha = hashlib.sha256(firmware).digest()
        offset = 0
        chunk_size = 192  # Conservative data payload per SMP packet

        while offset < total:
            end = min(offset + chunk_size, total)
            chunk = firmware[offset:end]

            payload = {"data": chunk, "off": offset}
            if offset == 0:
                payload["len"] = total
                payload["sha"] = sha

            resp = await self.request(SMP_OP_WRITE, SMP_GRP_IMG, IMG_CMD_UPLOAD, payload, timeout=30.0)

            if resp.get("rc", 0) != 0:
                raise RuntimeError(f"Upload error at offset {offset}: rc={resp.get('rc')}")

            # Device may report the next expected offset
            offset = resp.get("off", end)

            if progress_cb:
                progress_cb(offset, total)

    async def image_state(self) -> dict:
        """Query image state (list slots)."""
        return await self.request(SMP_OP_READ, SMP_GRP_IMG, IMG_CMD_STATE, {})

    async def image_test(self, img_hash: bytes) -> dict:
        """Mark image for test boot."""
        return await self.request(SMP_OP_WRITE, SMP_GRP_IMG, IMG_CMD_STATE, {"hash": img_hash, "confirm": False})

    async def image_confirm(self) -> dict:
        """Confirm current running image as permanent."""
        return await self.request(SMP_OP_WRITE, SMP_GRP_IMG, IMG_CMD_STATE, {"confirm": True})

    async def os_reset(self) -> None:
        """Reset the device."""
        try:
            await self.request(SMP_OP_WRITE, SMP_GRP_OS, OS_CMD_RESET, {}, timeout=3.0)
        except (TimeoutError, Exception):
            pass  # Device disconnects before responding — expected


async def scan_devices(name_filter: str = None, timeout: float = 5.0):
    """Scan for BLE devices advertising SMP service."""
    print(f"Сканирование BLE устройств ({timeout}s)…")
    devices = await BleakScanner.discover(
        timeout=timeout,
        service_uuids=[SMP_SVC_UUID],
    )

    if name_filter:
        devices = [d for d in devices if d.name and name_filter.lower() in d.name.lower()]

    return devices


async def select_device(name_filter: str = None) -> str:
    """Interactive device selection. Returns BLE address."""
    devices = await scan_devices(name_filter)

    if not devices:
        print("Устройства не найдены. Убедитесь, что прошивка с mcumgr запущена.")
        sys.exit(1)

    if len(devices) == 1:
        d = devices[0]
        print(f"Найдено: {d.name} [{d.address}] RSSI={d.rssi}")
        return d.address

    print(f"\nНайдено {len(devices)} устройств:")
    for i, d in enumerate(devices):
        print(f"  [{i}] {d.name or '(unknown)'} — {d.address} (RSSI {d.rssi})")

    while True:
        try:
            idx = int(input("\nВыбери номер устройства: "))
            if 0 <= idx < len(devices):
                return devices[idx].address
        except (ValueError, EOFError):
            pass
        print("Неверный выбор.")


def progress_bar(offset: int, total: int):
    """Print progress bar to terminal."""
    pct = offset / total * 100
    bar_len = 40
    filled = int(bar_len * offset / total)
    bar = "█" * filled + "░" * (bar_len - filled)
    speed = ""  # Could add speed calc
    sys.stdout.write(f"\r  [{bar}] {pct:5.1f}% ({offset}/{total})")
    sys.stdout.flush()
    if offset >= total:
        print()


async def main():
    parser = argparse.ArgumentParser(description="nRF52 OTA DFU via BLE (mcumgr SMP)")
    parser.add_argument("firmware", nargs="?", help="Path to app_update.bin")
    parser.add_argument("-d", "--device", help="Filter devices by name substring")
    parser.add_argument("-a", "--address", help="BLE address (skip scan)")
    parser.add_argument("--confirm", action="store_true", help="Confirm current image")
    parser.add_argument("--reset", action="store_true", help="Reset device")
    parser.add_argument("--test", action="store_true", help="Mark slot 1 image for test boot")
    parser.add_argument("--state", action="store_true", help="Query image state")
    parser.add_argument("--timeout", type=float, default=5.0, help="Scan timeout (seconds)")
    args = parser.parse_args()

    # Determine BLE address
    address = args.address
    if not address:
        address = await select_device(args.device)

    print(f"\nПодключение к {address}…")
    async with BleakClient(address) as client:
        if not client.is_connected:
            print("Не удалось подключиться!")
            sys.exit(1)
        print(f"Подключено. MTU={client.mtu_size}")

        smp = SMPClient(client)
        await smp.start()
        await asyncio.sleep(0.3)  # Let notifications settle

        # State query
        if args.state or (not args.firmware and not args.confirm and not args.reset and not args.test):
            if not args.firmware and not args.confirm and not args.reset and not args.test:
                # Just show state if no action requested
                print("\nСостояние образов:")
                state = await smp.image_state()
                for img in state.get("images", []):
                    flags = []
                    if img.get("active"): flags.append("ACTIVE")
                    if img.get("confirmed"): flags.append("CONFIRMED")
                    if img.get("pending"): flags.append("PENDING")
                    if img.get("permanent"): flags.append("PERMANENT")
                    ver = img.get("version", "?")
                    slot = img.get("slot", "?")
                    print(f"  Slot {slot}: v{ver} [{', '.join(flags) or 'none'}]")
                if not args.firmware:
                    return

        # Upload firmware
        if args.firmware:
            fw_path = Path(args.firmware)
            if not fw_path.exists():
                print(f"Файл не найден: {fw_path}")
                sys.exit(1)

            fw_data = fw_path.read_bytes()
            print(f"\nФайл: {fw_path.name} ({len(fw_data)} байт, {len(fw_data)/1024:.1f} KB)")
            print(f"SHA-256: {hashlib.sha256(fw_data).hexdigest()[:16]}…")
            print()

            t0 = time.time()
            await smp.image_upload(fw_data, progress_cb=progress_bar)
            elapsed = time.time() - t0
            speed = len(fw_data) / elapsed / 1024
            print(f"\n✓ Загрузка завершена за {elapsed:.1f}s ({speed:.1f} KB/s)")

            # Query state after upload
            state = await smp.image_state()
            for img in state.get("images", []):
                ver = img.get("version", "?")
                slot = img.get("slot", "?")
                print(f"  Slot {slot}: v{ver}")

            # Auto-test the new image
            if len(state.get("images", [])) > 1:
                img1 = state["images"][1]
                print(f"\nПомечаю slot 1 для тестовой загрузки…")
                resp = await smp.image_test(img1["hash"])
                if resp.get("rc", 0) == 0:
                    print("✓ Образ помечен для test boot")
                else:
                    print(f"⚠ Ошибка: rc={resp.get('rc')}")

                ans = input("\nПерезагрузить устройство сейчас? [y/N]: ").strip().lower()
                if ans in ("y", "yes", "д", "да"):
                    await smp.os_reset()
                    print("⟳ Reset отправлен. Устройство перезагрузится на новый образ.")

        # Confirm
        if args.confirm:
            print("\nПодтверждение текущего образа…")
            resp = await smp.image_confirm()
            if resp.get("rc", 0) == 0:
                print("✓ Образ подтверждён как постоянный.")
            else:
                print(f"⚠ Ошибка: rc={resp.get('rc')}")

        # Test
        if args.test:
            state = await smp.image_state()
            if len(state.get("images", [])) > 1:
                img1 = state["images"][1]
                resp = await smp.image_test(img1["hash"])
                if resp.get("rc", 0) == 0:
                    print("✓ Slot 1 помечен для test boot.")
                else:
                    print(f"⚠ Ошибка: rc={resp.get('rc')}")
            else:
                print("Нет образа в slot 1.")

        # Reset
        if args.reset:
            print("\nОтправка reset…")
            await smp.os_reset()
            print("⟳ Reset отправлен.")


if __name__ == "__main__":
    asyncio.run(main())
