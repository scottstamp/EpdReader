#!/usr/bin/env python3
"""
Host test: runs the Bluetooth client of tools/upload.py against the firmware's
protocol code (src/upload_proto.c) built for the PC, with a pipe in place of
the radio.  Needs gcc; no bleak or hardware.

    python tools/upload_test/test_upload.py
"""

import asyncio
import os
import random
import struct
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src")
sys.path.insert(0, os.path.join(HERE, ".."))
import upload  # noqa: E402


def build_sim(out_dir: str) -> str:
    exe = os.path.join(out_dir, "upload_sim.exe" if os.name == "nt" else "upload_sim")
    subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-O2", "-I", SRC, "-o", exe,
                    os.path.join(HERE, "upload_sim.c"), os.path.join(SRC, "upload_proto.c")],
                   check=True)
    return exe


class SimLink:
    """Device simulator process speaking [u16 len][bytes] over stdin/stdout."""

    def __init__(self, exe, store, ring, worker_every):
        self.proc = subprocess.Popen([exe, "--serve", store, "--ring", str(ring),
                                      "--worker-every", str(worker_every)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.loop = asyncio.get_running_loop()
        self.events = TickQueue(self)
        self.frames = []
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            hdr = self.proc.stdout.read(2)
            if len(hdr) < 2:
                return
            ev = self.proc.stdout.read(struct.unpack("<H", hdr)[0])
            self.loop.call_soon_threadsafe(self.events.put_nowait, ev)

    def send_raw(self, frame: bytes):
        self.proc.stdin.write(struct.pack("<H", len(frame)) + frame)
        self.proc.stdin.flush()

    async def write(self, frame: bytes, response: bool):
        self.frames.append(frame)
        self.send_raw(frame)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


class TickQueue(asyncio.Queue):
    """When the client blocks on a notification, let the simulated worker run
    (the real one runs on its own thread as soon as data arrives)."""

    def __init__(self, link):
        super().__init__()
        self.link = link

    async def get(self):
        if self.empty():
            self.link.send_raw(b"")
        return await super().get()


async def upload_via_sim(exe, store, name, data, ring=1024, worker_every=3, max_payload=244):
    link = SimLink(exe, store, ring, worker_every)
    try:
        await upload.ble_send_book(link.write, link.events, name, data, max_payload, timeout=5.0)
    finally:
        link.close()
    return link.frames


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


async def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_sim(tmp)
        store = os.path.join(tmp, "books")
        os.mkdir(store)
        rnd = random.Random(1234)

        cases = [
            ("big.epub", bytes(rnd.getrandbits(8) for _ in range(150_000)), 1024, 3, 244),
            ("tiny.txt", b"hello", 1024, 1, 244),
            ("empty.txt", b"", 1024, 1, 244),
            ("small_mtu.txt", bytes(rnd.getrandbits(8) for _ in range(5000)), 256, 7, 15),
            ("Author - Title (Book 2).epub", bytes(range(256)) * 40, 1024, 50, 244),
        ]
        for name, data, ring, every, mtu_payload in cases:
            try:
                frames = await upload_via_sim(exe, store, name, data, ring, every, mtu_payload)
                with open(os.path.join(store, name), "rb") as f:
                    check(f.read() == data, "stored content differs")
                check(not os.path.exists(os.path.join(store, name + ".part")), ".part left behind")
                data_frames = [fr for fr in frames if fr[0] == upload.BLE_OP_DATA]
                check(all(len(fr) <= min(mtu_payload, ring // 4) + 5 for fr in data_frames), "frame over MTU")
                print(f"ok   {name}: {len(data)} bytes in {len(frames)} frames")
            except Exception as e:
                failures += 1
                print(f"FAIL {name}: {e!r}")

        # Replace an existing book
        try:
            await upload_via_sim(exe, store, "tiny.txt", b"second version")
            with open(os.path.join(store, "tiny.txt"), "rb") as f:
                check(f.read() == b"second version", "not replaced")
            print("ok   replace existing")
        except Exception as e:
            failures += 1
            print(f"FAIL replace: {e!r}")

        # Device-side rejection surfaces as BleUploadError
        for bad in ("../evil.epub", "notes.pdf", "Café.epub"):
            try:
                await upload_via_sim(exe, store, bad, b"x")
                failures += 1
                print(f"FAIL {bad}: accepted")
            except upload.BleUploadError as e:
                check("invalid file name" in str(e), str(e))
                print(f"ok   rejected {bad}: {e}")
        check(not upload.ble_name_valid("../evil.epub") and upload.ble_name_valid("a.epub"),
              "client name check")

        # A client that ignores the window is caught by the device
        link = SimLink(exe, store, 256, 1000)
        try:
            await link.write(upload.ble_frame_start("flood.txt", 2000, 0), True)
            ev = await link.events.get()
            check(ev[0] == upload.BLE_EVT_READY, "no READY")
            for off in range(0, 2000, 100):
                await link.write(upload.ble_frame_data(off, b"z" * 100), False)
            ev = await asyncio.wait_for(link.events.get(), 5)
            try:
                upload.ble_check_event(ev)
                failures += 1
                print("FAIL overrun not reported")
            except upload.BleUploadError as e:
                check("overrun" in str(e), str(e))
                print(f"ok   window violation: {e}")
        finally:
            link.close()
        check(not os.path.exists(os.path.join(store, "flood.txt.part")), "flood .part left behind")

    print("all client tests passed" if not failures else f"{failures} FAILURES")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
