#!/usr/bin/env python3
"""
upload.py  –  nRF EPD Reader: CDC Binary File Transfer Tool
------------------------------------------------------------
Transfers EPUB/TXT books to the device over USB CDC-ACM without using
the slow USB Mass Storage (MSC/SCSI) stack.

Requirements:
    pip install pyserial

Usage:
    python upload.py --file mybook.epub          # upload a file
    python upload.py --list                       # list books on device
    python upload.py --delete mybook.epub         # delete a book
    python upload.py --status                     # show free space
    python upload.py --port COM5 --file book.epub # specify port manually

The device must be in "CDC Transfer Mode" (select from main menu).
"""

import argparse
import os
import struct
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not found.  Run:  pip install pyserial")
    sys.exit(1)

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
        """,
    )
    parser.add_argument("--port", "-p", metavar="PORT",
                        help="Serial port  (auto-detected if omitted)")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--file",   "-f", metavar="FILE",     help="Upload EPUB/TXT file")
    group.add_argument("--list",   "-l", action="store_true", help="List books on device")
    group.add_argument("--delete", "-d", metavar="FILENAME",  help="Delete a book by filename")
    group.add_argument("--status", "-s", action="store_true", help="Show device storage status")
    args = parser.parse_args()

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
