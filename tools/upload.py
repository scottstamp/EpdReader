#!/usr/bin/env python3
"""
upload.py  –  nRF EPD Reader: CDC Binary File Transfer Tool
------------------------------------------------------------
Transfers EPUB/TXT books to the device over USB CDC-ACM without using
the slow USB Mass Storage (MSC/SCSI) stack, or over Bluetooth LE.

Requirements:
    pip install pyserial        (USB CDC mode)
    pip install bleak           (Bluetooth mode, --ble)

Usage:
    python upload.py --file mybook.epub          # upload a file
    python upload.py --list                       # list books on device
    python upload.py --delete mybook.epub         # delete a book
    python upload.py --status                     # show free space
    python upload.py --port COM5 --file book.epub # specify port manually

The device must be in "CDC Transfer Mode" (select from main menu).

Bluetooth:
    python upload.py --ble --file mybook.epub     # find "nRF EPD Reader" and upload
    python upload.py --ble --address AA:BB:CC:DD:EE:FF --file mybook.epub
    python upload.py --ble --file "Long name.epub" --remote-name short.epub

On the reader, open Transfer > Bluetooth upload first.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys
import time
import zlib

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None   # only needed for USB CDC mode; reported in main()

# ── Protocol constants ───────────────────────────────────────────────────────
STX             = 0x02
ACK_BYTE        = 0xAA

CMD_FILE_START  = 0x01
CMD_FILE_DATA   = 0x02
CMD_FILE_END    = 0x03
CMD_FILE_DELETE = 0x04
CMD_LIST_FILES  = 0x05
CMD_GET_STATUS  = 0x06

STA_OK   = 0x00
STA_ERR  = 0x01
STA_DATA = 0x02

# USB VID:PID from prj.conf  (0x2FE3 = nRF default, 0x0004 = our PID)
DEVICE_VID = 0x2FE3
DEVICE_PID = 0x0004

CHUNK_SIZE  = 4096  # bytes per FILE_DATA frame – must match firmware MAX_PAYLOAD
WRITE_BATCH = 1     # 1 frame per ACK – host waits for ACK before sending next frame


# ── CRC-8/SMBUS ─────────────────────────────────────────────────────────────
def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) if (crc & 0x80) else (crc << 1)
            crc &= 0xFF
    return crc


# ── Frame builder ────────────────────────────────────────────────────────────
def make_frame(cmd: int, payload: bytes = b"") -> bytes:
    header = bytes([cmd]) + struct.pack("<I", len(payload))   # CMD + LEN(4 LE)
    body   = header + payload
    return bytes([STX]) + body + bytes([crc8(body)])


# ── Response reader ──────────────────────────────────────────────────────────
def read_response(port: serial.Serial, timeout: float = 8.0) -> tuple:
    """Returns (status: int, data: bytes).  Raises IOError on bad framing."""
    port.timeout = timeout
    hdr = port.read(6)
    if len(hdr) < 6:
        raise IOError(f"Response timeout (got {len(hdr)} bytes)")
    if hdr[0] != ACK_BYTE:
        raise IOError(f"Bad ACK byte: 0x{hdr[0]:02X}  (full: {hdr.hex()})")
    status = hdr[1]
    dlen   = struct.unpack("<I", hdr[2:6])[0]
    data   = port.read(dlen) if dlen > 0 else b""
    if len(data) != dlen:
        raise IOError(f"Response data truncated: want {dlen}, got {len(data)}")
    return status, data


# ── Port auto-detection ──────────────────────────────────────────────────────
def _probe_is_transfer_port(port_name: str) -> bool:
    """Return True if this COM port responds to our binary transfer protocol.
    The device enumerates two CDC-ACM ports: one for console/logging and one
    for our protocol.  We probe with CMD_GET_STATUS and check for ACK_BYTE."""
    try:
        with serial.Serial(port_name, baudrate=115200, timeout=2.0) as port:
            port.reset_input_buffer()
            time.sleep(0.3)
            port.reset_input_buffer()
            port.write(make_frame(CMD_GET_STATUS))
            hdr = port.read(6)
            return len(hdr) == 6 and hdr[0] == ACK_BYTE
    except Exception:
        return False


def find_port() -> str:
    candidates: list[str] = []
    for p in serial.tools.list_ports.comports():
        if p.vid == DEVICE_VID and p.pid == DEVICE_PID:
            candidates.append(p.device)
    if not candidates:
        for p in serial.tools.list_ports.comports():
            if p.description and "EPD" in p.description:
                candidates.append(p.device)
    if not candidates:
        raise RuntimeError(
            "Device not found automatically.\n"
            "  • Make sure the device is in CDC Transfer Mode (select from menu).\n"
            "  • Use  --port COMx  to specify the port manually."
        )
    if len(candidates) == 1:
        return candidates[0]

    # Device exposes two COM ports (console + transfer). Probe each one.
    for port_name in sorted(candidates):        # try lower COM number first
        if _probe_is_transfer_port(port_name):
            return port_name

    # Probing failed (device not in transfer mode yet?) – return first port
    return candidates[0]


# ── Progress bar (no external deps) ─────────────────────────────────────────
def _progress(transferred: int, total: int, speed_kbs: float):
    pct   = min(100, transferred * 100 // total)
    width = 25
    done  = pct * width // 100
    bar   = "█" * done + "░" * (width - done)
    print(f"\r  [{bar}] {pct:3d}%  {speed_kbs:6.0f} KB/s", end="", flush=True)


# ── Commands ─────────────────────────────────────────────────────────────────
def cmd_upload(port: serial.Serial, filepath: str):
    filename = os.path.basename(filepath)
    filesize = os.path.getsize(filepath)
    ext      = os.path.splitext(filename)[1].lower()

    if ext not in (".epub", ".txt"):
        print(f"WARNING: '{ext}' is not a recognised book format (.epub / .txt)")

    print(f"  File : {filename}")
    print(f"  Size : {filesize / 1024:.1f} KB")
    print(f"  Chunk: {CHUNK_SIZE} bytes  ×{WRITE_BATCH} per write")
    print()

    # FILE_START
    port.write(make_frame(CMD_FILE_START, filename.encode("utf-8")))
    status, _ = read_response(port, timeout=6.0)
    if status != STA_OK:
        print("ERROR: Device rejected FILE_START  (is it in CDC Transfer Mode?)")
        return False

    transferred = 0
    t0 = time.monotonic()

    with open(filepath, "rb") as f:
        speed = 0.0
        while True:
            # Read up to WRITE_BATCH chunks and build one combined write
            batch_chunks: list[bytes] = []
            for _ in range(WRITE_BATCH):
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                batch_chunks.append(chunk)

            if not batch_chunks:
                break

            # Single OS write call for the whole batch (4× less driver overhead)
            port.write(b"".join(make_frame(CMD_FILE_DATA, c) for c in batch_chunks))

            # Read one ACK per frame in the batch
            for chunk in batch_chunks:
                status, _ = read_response(port, timeout=30.0)
                if status != STA_OK:
                    print(f"\nERROR: Write failed at offset {transferred}")
                    return False
                transferred += len(chunk)
                elapsed = time.monotonic() - t0
                speed   = (transferred / elapsed / 1024) if elapsed > 0 else 0
                _progress(transferred, filesize, speed)

    # FILE_END
    port.write(make_frame(CMD_FILE_END))
    status, _ = read_response(port, timeout=6.0)
    elapsed = time.monotonic() - t0
    speed   = (filesize / elapsed / 1024) if elapsed > 0 else 0

    if status == STA_OK:
        _progress(filesize, filesize, speed)
        print(f"\n\n✓ Transfer complete: {elapsed:.1f}s  average {speed:.0f} KB/s")
        return True
    else:
        print("\nERROR: FILE_END rejected by device")
        return False


def cmd_list(port: serial.Serial):
    port.write(make_frame(CMD_LIST_FILES))
    status, data = read_response(port, timeout=6.0)
    if status != STA_DATA:
        print("ERROR: Could not retrieve file list")
        return
    text  = data.decode("utf-8", errors="replace").strip()
    files = [f for f in text.split("\n") if f]
    if not files:
        print("  (no books on device)")
    else:
        print(f"  Books on device ({len(files)}):")
        for name in files:
            print(f"    • {name}")


def cmd_delete(port: serial.Serial, filename: str):
    port.write(make_frame(CMD_FILE_DELETE, filename.encode("utf-8")))
    status, _ = read_response(port, timeout=6.0)
    if status == STA_OK:
        print(f"✓ Deleted: {filename}")
    else:
        print(f"ERROR: Could not delete '{filename}'  (does it exist on device?)")


def cmd_status(port: serial.Serial):
    port.write(make_frame(CMD_GET_STATUS))
    status, data = read_response(port, timeout=6.0)
    if status == STA_DATA:
        info = data.decode("utf-8", errors="replace")
        parts = dict(kv.split(":") for kv in info.split(",") if ":" in kv)
        free  = int(parts.get("free",  0))
        total = int(parts.get("total", 0))
        print(f"  Free  : {free  / 1024 / 1024:.1f} MB")
        if total > 0:
            print(f"  Total : {total / 1024 / 1024:.1f} MB")
            print(f"  Used  : {(total - free) / 1024 / 1024:.1f} MB  ({(total - free) * 100 // total}%)")
    else:
        print("ERROR: Could not get device status")


# ── Bluetooth LE upload (Nordic UART Service) ────────────────────────────────
# Protocol: see src/upload_proto.h in the firmware.
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # we write frames here
NUS_TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # device notifies here
BLE_DEVICE_NAME  = "nRF EPD Reader"

BLE_PROTO_VERSION = 1
BLE_OP_START, BLE_OP_DATA, BLE_OP_END, BLE_OP_ABORT = 0x01, 0x02, 0x03, 0x04
BLE_EVT_READY, BLE_EVT_ACK, BLE_EVT_DONE, BLE_EVT_ERROR = 0x81, 0x82, 0x83, 0x84
BLE_ATT_OVERHEAD  = 3       # ATT write header
BLE_DATA_HDR      = 5       # DATA opcode + offset
BLE_NAME_MAX      = 63      # characters, printable ASCII

BLE_ERRORS = {
    1: "protocol error",
    2: "invalid file name",
    3: "device busy",
    4: "no transfer open",
    5: "data out of order",
    6: "receive buffer overrun",
    7: "size mismatch",
    8: "checksum mismatch",
    9: "SD card write failed (card full or missing?)",
    10: "transfer cancelled on the device",
}


class BleUploadError(Exception):
    pass


def ble_name_valid(name: str) -> bool:
    """Same rules as the firmware: a bare ASCII .epub/.txt name FAT stores as-is."""
    if not 0 < len(name) <= BLE_NAME_MAX:
        return False
    if name[0] in ". " or name[-1] in ". " or ".." in name:
        return False
    if any(not 0x20 <= ord(c) < 0x7F or c in '/\\:*?"<>|' for c in name):
        return False
    lower = name.lower()
    return any(lower.endswith(ext) and len(lower) > len(ext) for ext in (".epub", ".txt"))


def ble_frame_start(name: str, size: int, crc: int) -> bytes:
    return struct.pack("<BBII", BLE_OP_START, BLE_PROTO_VERSION, size, crc) + name.encode("utf-8")


def ble_frame_data(offset: int, payload: bytes) -> bytes:
    return struct.pack("<BI", BLE_OP_DATA, offset) + payload


def ble_check_event(ev: bytes) -> int:
    """Returns the event code; raises BleUploadError for ERROR or a malformed event."""
    if not ev:
        raise BleUploadError("empty notification from device")
    if ev[0] == BLE_EVT_ERROR and len(ev) >= 6:
        code, offset = struct.unpack_from("<BI", ev, 1)
        raise BleUploadError(f"device reported: {BLE_ERRORS.get(code, f'error {code}')} (at byte {offset})")
    sizes = {BLE_EVT_READY: 8, BLE_EVT_ACK: 5, BLE_EVT_DONE: 9}
    if ev[0] not in sizes or len(ev) < sizes[ev[0]]:
        raise BleUploadError(f"unexpected notification: {ev.hex()}")
    return ev[0]


async def ble_send_book(write, events: asyncio.Queue, name: str, data: bytes,
                        max_payload: int, progress=None, timeout: float = 10.0) -> None:
    """Runs one upload.  write(frame, response) sends a frame to NUS RX, events
    receives the NUS TX notifications.  Raises BleUploadError on failure."""
    size = len(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF

    async def next_event() -> bytes:
        try:
            return await asyncio.wait_for(events.get(), timeout)
        except asyncio.TimeoutError:
            raise BleUploadError("device stopped responding") from None

    await write(ble_frame_start(name, size, crc), True)
    ev = await next_event()
    if ble_check_event(ev) != BLE_EVT_READY:
        raise BleUploadError(f"expected READY, got {ev.hex()}")
    _, version, window, dev_max = struct.unpack_from("<BBIH", ev)
    if version != BLE_PROTO_VERSION:
        raise BleUploadError(f"device speaks protocol v{version}, this tool v{BLE_PROTO_VERSION}")
    chunk = max(1, min(max_payload, dev_max))

    acked = 0

    def handle(ev: bytes) -> None:
        nonlocal acked
        if ble_check_event(ev) != BLE_EVT_ACK:
            raise BleUploadError(f"unexpected notification: {ev.hex()}")
        acked = max(acked, struct.unpack_from("<I", ev, 1)[0])

    sent = 0
    while sent < size:
        n = min(chunk, size - sent)
        # Never more than `window` unacknowledged bytes: the device buffers exactly that much
        while sent + n - acked > window:
            handle(await next_event())
        while not events.empty():
            handle(events.get_nowait())
        await write(ble_frame_data(sent, data[sent:sent + n]), False)
        sent += n
        if progress:
            progress(acked, size)

    # END waits for the final ACK so it cannot overtake data still in flight
    while acked < size:
        handle(await next_event())
    if progress:
        progress(size, size)
    await write(bytes([BLE_OP_END]), True)
    ev = await next_event()
    if ble_check_event(ev) != BLE_EVT_DONE:
        raise BleUploadError(f"expected DONE, got {ev.hex()}")
    done_size, done_crc = struct.unpack_from("<II", ev, 1)
    if done_size != size or done_crc != crc:
        raise BleUploadError("device confirmed a different size/checksum")


async def ble_upload(filepath: str, remote_name: str | None, address: str | None,
                     device_name: str, timeout: float) -> bool:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("ERROR: bleak not found.  Run:  pip install bleak")
        return False

    name = remote_name or os.path.basename(filepath)
    if not ble_name_valid(name):
        print(f"ERROR: '{name}' cannot be stored on the reader.  The name must end in .epub or .txt,")
        print(f"       be plain ASCII of at most {BLE_NAME_MAX} characters, and not contain")
        print("       / \\ : * ? \" < > | or '..'.")
        print("       Use --remote-name to choose another name.")
        return False
    with open(filepath, "rb") as f:
        data = f.read()

    print(f"  File : {name}")
    print(f"  Size : {len(data) / 1024:.1f} KB")
    if address:
        print(f"  Looking for {address} ...")
        device = await BleakScanner.find_device_by_address(address, timeout=timeout)
    else:
        print(f"  Scanning for '{device_name}' ...")
        device = await BleakScanner.find_device_by_filter(
            lambda d, adv: (adv.local_name or d.name) == device_name, timeout=timeout)
    if device is None:
        print("ERROR: Reader not found.  On the reader open Transfer > Bluetooth upload,")
        print("       then run this again (or pass --address).")
        return False

    async with BleakClient(device, timeout=timeout) as client:
        backend = getattr(client, "_backend", None)
        if hasattr(backend, "_acquire_mtu"):   # BlueZ reports 23 until asked
            try:
                await backend._acquire_mtu()
            except Exception:
                pass
        max_payload = max(1, client.mtu_size - BLE_ATT_OVERHEAD - BLE_DATA_HDR)
        print(f"  Connected: {device.address}  (MTU {client.mtu_size})")
        print()

        events: asyncio.Queue = asyncio.Queue()
        await client.start_notify(NUS_TX_UUID, lambda _char, payload: events.put_nowait(bytes(payload)))

        async def write(frame: bytes, response: bool) -> None:
            await client.write_gatt_char(NUS_RX_UUID, frame, response=response)

        async def abort() -> None:
            try:
                await write(bytes([BLE_OP_ABORT]), True)
            except Exception:
                pass

        t0 = time.monotonic()

        def progress(done: int, total: int) -> None:
            elapsed = time.monotonic() - t0
            _progress(done, max(total, 1), (done / elapsed / 1024) if elapsed > 0 else 0)

        try:
            await ble_send_book(write, events, name, data, max_payload, progress, timeout)
        except BleUploadError as e:
            print(f"\nERROR: {e}")
            await abort()
            return False
        except (KeyboardInterrupt, asyncio.CancelledError):
            await abort()
            raise

    elapsed = time.monotonic() - t0
    speed = (len(data) / elapsed / 1024) if elapsed > 0 else 0
    print(f"\n\n✓ Transfer complete: {elapsed:.1f}s  average {speed:.1f} KB/s")
    return True


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="nRF EPD Reader – CDC Book Transfer Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python upload.py --file "My Book.epub"
  python upload.py --port COM5 --file mybook.epub
  python upload.py --list
  python upload.py --delete mybook.epub
  python upload.py --status
  python upload.py --ble --file mybook.epub
        """,
    )
    parser.add_argument("--port", "-p", metavar="PORT",
                        help="Serial port  (auto-detected if omitted)")
    parser.add_argument("--ble", "-b", action="store_true",
                        help="Upload over Bluetooth LE instead of USB  (needs bleak; --file only)")
    parser.add_argument("--address", metavar="ADDR",
                        help="Bluetooth address of the reader  (scans by name if omitted)")
    parser.add_argument("--ble-name", metavar="NAME", default=BLE_DEVICE_NAME,
                        help=f"Bluetooth name to scan for  (default: {BLE_DEVICE_NAME})")
    parser.add_argument("--remote-name", metavar="NAME",
                        help="File name to store on the reader  (default: the local file name)")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="Bluetooth scan/response timeout in seconds  (default: 10)")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--file",   "-f", metavar="FILE",     help="Upload EPUB/TXT file")
    group.add_argument("--list",   "-l", action="store_true", help="List books on device")
    group.add_argument("--delete", "-d", metavar="FILENAME",  help="Delete a book by filename")
    group.add_argument("--status", "-s", action="store_true", help="Show device storage status")
    args = parser.parse_args()

    if args.ble:
        if not args.file:
            print("ERROR: Bluetooth mode only supports --file")
            sys.exit(1)
        if not os.path.isfile(args.file):
            print(f"ERROR: File not found: {args.file}")
            sys.exit(1)
        print("nRF EPD Reader – Bluetooth Transfer")
        print("─" * 48)
        try:
            ok = asyncio.run(ble_upload(args.file, args.remote_name, args.address,
                                        args.ble_name, args.timeout))
        except KeyboardInterrupt:
            print("\nAborted.")
            ok = False
        except Exception as e:      # bleak raises backend-specific errors
            print(f"\nBluetooth error: {e}")
            ok = False
        sys.exit(0 if ok else 1)

    if serial is None:
        print("ERROR: pyserial not found.  Run:  pip install pyserial")
        sys.exit(1)

    # Resolve port
    try:
        port_name = args.port or find_port()
    except RuntimeError as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    print(f"nRF EPD Reader – CDC Transfer  ({port_name})")
    print("─" * 48)

    try:
        with serial.Serial(port_name, baudrate=115200, timeout=5.0) as port:
            port.reset_input_buffer()
            time.sleep(0.3)          # allow Windows CDC line-coding handshake to complete
            port.reset_input_buffer()  # flush any stale boot banner bytes

            if args.file:
                if not os.path.isfile(args.file):
                    print(f"ERROR: File not found: {args.file}")
                    sys.exit(1)
                success = cmd_upload(port, args.file)
                sys.exit(0 if success else 1)

            elif args.list:
                cmd_list(port)

            elif args.delete:
                cmd_delete(port, args.delete)

            elif args.status:
                cmd_status(port)

    except serial.SerialException as e:
        print(f"Serial error: {e}")
        sys.exit(1)
    except IOError as e:
        print(f"Protocol error: {e}")
        print("  → Make sure the device is in CDC Transfer Mode and try again.")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nAborted.")
        sys.exit(1)


if __name__ == "__main__":
    main()
