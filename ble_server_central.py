import asyncio
import logging
import os
import sys
from bleak import BleakScanner, BleakClient

# Setup logging
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger(__name__)

# NUS Service UUIDs
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # PC writes to this characteristic
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # PC receives notifications on this characteristic

BOOKS_DIR = "./books"
cmd_buffer = ""

async def run_server(client: BleakClient):
    global cmd_buffer
    
    # Check directory
    if not os.path.exists(BOOKS_DIR):
        os.makedirs(BOOKS_DIR)

    async def notification_handler(sender, data: bytearray):
        global cmd_buffer
        try:
            msg = data.decode("utf-8", errors="ignore")
            logger.info(f"Raw notification received: {repr(msg)}")
            cmd_buffer += msg
            
            # Self-healing: if a new command starts while we have old, incomplete data, discard the old data.
            prefixes = ["REQ_LIST", "REQ_SIZE:", "REQ_TEXT:"]
            best_idx = -1
            for prefix in prefixes:
                idx = cmd_buffer.rfind(prefix)
                if idx > best_idx:
                    best_idx = idx
            if best_idx > 0:
                logger.warning(f"Discarding incomplete command buffer: {repr(cmd_buffer[:best_idx])}")
                cmd_buffer = cmd_buffer[best_idx:]

            if "\n" in cmd_buffer:
                lines = cmd_buffer.split("\n")
                cmd_buffer = lines[-1]
                for line in lines[:-1]:
                    line = line.strip()
                    if line:
                        await process_command(client, line)
        except Exception as e:
            logger.error(f"Error in notification handler: {e}")

    async def send_response(client: BleakClient, data: bytes):
        # Write to RX characteristic (split into 20-byte chunks to fit BLE MTU safely)
        chunk_size = 20
        for i in range(0, len(data), chunk_size):
            chunk = data[i:i+chunk_size]
            await client.write_gatt_char(NUS_RX_UUID, chunk, response=True)

    async def process_command(client: BleakClient, cmd: str):
        logger.info(f"Command received: '{cmd}'")
        
        if cmd == "REQ_LIST":
            files = [f for f in os.listdir(BOOKS_DIR) if f.endswith(".txt")]
            logger.info(f"Sending book list ({len(files)} files)...")
            await send_response(client, b"LIST_START\n")
            for filename in files:
                filepath = os.path.join(BOOKS_DIR, filename)
                size = os.path.getsize(filepath)
                resp = f"BOOK:{filename}:{size}\n"
                await send_response(client, resp.encode("utf-8"))
            await send_response(client, b"LIST_END\n")
            logger.info("Sent LIST_END.")
            
        elif cmd.startswith("REQ_SIZE:"):
            filename = cmd.split(":", 1)[1].strip()
            filepath = os.path.join(BOOKS_DIR, filename)
            if os.path.exists(filepath):
                size = os.path.getsize(filepath)
                resp = f"SIZE:{size}\n"
                logger.info(f"Book: '{filename}', size: {size} bytes")
                await send_response(client, resp.encode("utf-8"))
            else:
                logger.warning(f"Book '{filename}' not found for size request.")
                await send_response(client, b"SIZE:0\n")
                
        elif cmd.startswith("REQ_TEXT:"):
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
                await send_response(client, header.encode("utf-8"))
                
                # Stream raw bytes in chunks of 20 bytes
                await send_response(client, data)
                logger.info("Stream complete.")

    logger.info("Subscribing to TX notifications...")
    await client.start_notify(NUS_TX_UUID, notification_handler)
    logger.info("Ready to serve book requests. Please browse PC Books on the reader.")
    
    # Keep running as long as connected
    while client.is_connected:
        await asyncio.sleep(1)
        
    logger.info("Client disconnected.")

async def main():
    logger.info("Scanning for nice!nano reader device 'nRF_Epd_Reader'...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: ad.local_name == "nRF_Epd_Reader",
        timeout=20.0
    )
    
    if not device:
        logger.error("nice!nano reader 'nRF_Epd_Reader' not found. Make sure the reader is turned on and advertising (BLE Upload Mode or Main Menu).")
        return
        
    logger.info(f"Found reader at {device.address}. Connecting...")
    
    def handle_disconnect(_client):
        logger.info("BLE Connection closed by device.")
        
    async with BleakClient(device, disconnected_callback=handle_disconnect) as client:
        logger.info("Connected to reader!")
        await run_server(client)

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Server terminated by user.")
    except Exception as e:
        logger.exception(f"Fatal server error: {e}")
