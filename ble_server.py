import asyncio
import logging
import os
import sys
import ctypes
from typing import Any
from bless import (
    BlessServer,
    BlessGATTCharacteristic,
    GATTCharacteristicProperties,
    GATTAttributePermissions,
)

# Setup logging
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger(__name__)

# NUS Service UUIDs
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e" # Client write, server receive
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e" # Client receive (notify), server send

BOOKS_DIR = "./books"

def is_admin() -> bool:
    try:
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except:
        return False

class BookServer:
    def __init__(self):
        self.server = None
        self.rx_char = None
        self.tx_char = None
        self.cmd_buffer = ""
        self.loop = None

    async def start(self):
        self.loop = asyncio.get_running_loop()
        
        # Windows requires Administrator privileges to rename the local BLE adapter name
        if sys.platform == "win32" and is_admin():
            logger.info("Running with Administrator privileges. Enabling BLE adapter name overwrite to 'EpdBookServer'...")
            self.server = BlessServer(name="EpdBookServer", name_overwrite=True)
        else:
            if sys.platform == "win32":
                logger.info(
                    "Running without Administrator privileges. The BLE server will advertise under your computer's default name. "
                    "To advertise explicitly as 'EpdBookServer' on Windows, run this script as Administrator (elevated shell)."
                )
            self.server = BlessServer(name="EpdBookServer", name_overwrite=False)
        self.server.read_request_func = self.read_request
        self.server.write_request_func = self.write_request

        # Add Service
        await self.server.add_new_service(NUS_SERVICE_UUID)

        # RX Characteristic (Write / Write without response)
        rx_properties = (
            GATTCharacteristicProperties.write_without_response |
            GATTCharacteristicProperties.write
        )
        rx_permissions = GATTAttributePermissions.writeable
        await self.server.add_new_characteristic(
            NUS_SERVICE_UUID, NUS_RX_UUID, rx_properties, None, rx_permissions
        )

        # TX Characteristic (Notify / Read)
        tx_properties = (
            GATTCharacteristicProperties.notify |
            GATTCharacteristicProperties.read
        )
        tx_permissions = GATTAttributePermissions.readable
        await self.server.add_new_characteristic(
            NUS_SERVICE_UUID, NUS_TX_UUID, tx_properties, None, tx_permissions
        )

        # Get instances of characteristics
        self.rx_char = self.server.get_characteristic(NUS_RX_UUID)
        self.tx_char = self.server.get_characteristic(NUS_TX_UUID)

        await self.server.start()
        logger.info("BLE Book Server started and advertising as 'EpdBookServer'...")

    async def stop(self):
        if self.server:
            await self.server.stop()
            logger.info("BLE Book Server stopped.")

    def read_request(self, characteristic: BlessGATTCharacteristic, **kwargs) -> bytearray:
        return characteristic.value

    def write_request(self, characteristic: BlessGATTCharacteristic, value: Any, **kwargs):
        characteristic.value = value
        try:
            # Decode the incoming byte array
            data = value.decode("utf-8", errors="ignore")
            self.cmd_buffer += data
            
            # Self-healing: if a new command starts while we have old, incomplete data, discard the old data.
            prefixes = ["REQ_LIST", "REQ_SIZE:", "REQ_TEXT:"]
            best_idx = -1
            for prefix in prefixes:
                idx = self.cmd_buffer.rfind(prefix)
                if idx > best_idx:
                    best_idx = idx
            if best_idx > 0:
                logger.warning(f"Discarding incomplete command buffer: {repr(self.cmd_buffer[:best_idx])}")
                self.cmd_buffer = self.cmd_buffer[best_idx:]

            if "\n" in self.cmd_buffer:
                lines = self.cmd_buffer.split("\n")
                # The last element might be incomplete (or empty if ended with \n)
                self.cmd_buffer = lines[-1]
                for line in lines[:-1]:
                    line = line.strip()
                    if line:
                        # Schedule execution on the main asyncio event loop
                        asyncio.run_coroutine_threadsafe(self.process_command(line), self.loop)
        except Exception as e:
            logger.error(f"Error handling write request: {e}")

    async def send_notification(self, data: bytes):
        self.tx_char.value = bytearray(data)
        await self.server.update_value(NUS_SERVICE_UUID, NUS_TX_UUID)
        # Small delay to allow the BLE stack to send the packet
        await asyncio.sleep(0.02)

    async def process_command(self, cmd: str):
        logger.info(f"Command received: '{cmd}'")
        
        if cmd == "REQ_LIST":
            if not os.path.exists(BOOKS_DIR):
                os.makedirs(BOOKS_DIR)
            files = [f for f in os.listdir(BOOKS_DIR) if f.endswith(".txt")]
            
            logger.info(f"Sending book list ({len(files)} files)...")
            await self.send_notification(b"LIST_START\n")
            for filename in files:
                filepath = os.path.join(BOOKS_DIR, filename)
                size = os.path.getsize(filepath)
                resp = f"BOOK:{filename}:{size}\n"
                await self.send_notification(resp.encode("utf-8"))
            await self.send_notification(b"LIST_END\n")
            logger.info("Sent LIST_END.")
            
        elif cmd.startswith("REQ_SIZE:"):
            filename = cmd.split(":", 1)[1].strip()
            filepath = os.path.join(BOOKS_DIR, filename)
            if os.path.exists(filepath):
                size = os.path.getsize(filepath)
                resp = f"SIZE:{size}\n"
                logger.info(f"Book: '{filename}', size: {size} bytes")
                await self.send_notification(resp.encode("utf-8"))
            else:
                logger.warning(f"Book '{filename}' not found for size request.")
                await self.send_notification(b"SIZE:0\n")
                
        elif cmd.startswith("REQ_TEXT:"):
            # Format: REQ_TEXT:<filename>:<offset>:<length>
            parts = cmd.split(":")
            if len(parts) >= 4:
                filename = parts[1].strip()
                offset = int(parts[2])
                length = int(parts[3])
                
                filepath = os.path.join(BOOKS_DIR, filename)
                if os.path.exists(filepath):
                    with open(filepath, "rb") as f:
                        f.seek(offset)
                        data = f.read(length)
                else:
                    data = b""
                
                logger.info(f"Streaming '{filename}' at offset {offset} (read {len(data)} bytes)...")
                header = f"TEXT_START:{len(data)}\n"
                await self.send_notification(header.encode("utf-8"))
                
                # Stream raw bytes in chunks of 20 bytes
                chunk_size = 20
                for i in range(0, len(data), chunk_size):
                    chunk = data[i:i+chunk_size]
                    await self.send_notification(chunk)
                logger.info("Stream complete.")

async def main():
    server = BookServer()
    await server.start()
    try:
        while True:
            await asyncio.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        await server.stop()

if __name__ == "__main__":
    if sys.platform == "win32":
        # Enable multi-threaded apartment apartment type if needed
        # (usually default in python, but helps WinRT)
        pass
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Program terminated.")
