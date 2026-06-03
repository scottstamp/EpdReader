# BLE NUS Book Uploader
# Compatible with bleak library (pip install bleak)

import asyncio
import os
import sys
from bleak import BleakScanner, BleakClient

# The NUS (Nordic UART Service) UUIDs
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Client writes to RX on server

TARGET_DEVICE_NAME = "nRF_E"

async def main():
    # 1. Resolve target book file
    book_path = "books\\The_Compound_Pt1_Ch1a.txt"
    if not os.path.exists(book_path):
        print(f"Error: {book_path} not found in the current directory.")
        sys.exit(1)
        
    print(f"Reading {book_path}...")
    with open(book_path, "r", encoding="utf-8", errors="ignore") as f:
        book_content = f.read()
        
    # Standardize first line (ensure title + newline)
    lines = book_content.splitlines()
    if not lines:
        print("Error: The book file is empty.")
        sys.exit(1)
        
    print(f"Detected book title: '{lines[0]}'")
    payload = book_content.encode("utf-8")
    payload_size = len(payload)
    print(f"Payload size: {payload_size / 1024.0:.2f} KB ({payload_size} bytes)")

    # 2. Scanning for BLE Device
    print(f"Scanning for BLE device with name '{TARGET_DEVICE_NAME}'...")
    device = None
    devices = await BleakScanner.discover(timeout=5.0)
    for d in devices:
        if d.name == TARGET_DEVICE_NAME:
            device = d
            break
            
    if not device:
        print(f"Error: Could not find target device '{TARGET_DEVICE_NAME}'.")
        print("Please make sure the board is powered on and in 'BLE Upload Mode'.")
        sys.exit(1)
        
    print(f"Found device: {device.name} [{device.address}]")
    print(f"Connecting to {device.address}...")

    # 3. Connection and streaming payload
    async with BleakClient(device) as client:
        print("Connected successfully!")
        
        # Verify NUS service is available
        nus_service = client.services.get_service(NUS_SERVICE_UUID)
        if not nus_service:
            print("Error: Nordic UART Service (NUS) not found on device.")
            sys.exit(1)
            
        rx_char = nus_service.get_characteristic(NUS_RX_CHAR_UUID)
        if not rx_char:
            print("Error: NUS RX characteristic not found.")
            sys.exit(1)

        print("Starting transmission...")
        
        # Maximum transmission unit (MTU) chunk size (typically safe limit for BLE transfers is 20-244 bytes)
        # Bleak automatically manages MTU negotiation. On Windows WinRT, writing without response
        # can throw "parameter is incorrect" if it exceeds MTU. A standard 20-byte chunk size is highly compatible.
        chunk_size = 20
        bytes_sent = 0
        
        for i in range(0, payload_size, chunk_size):
            chunk = payload[i:i+chunk_size]
            # Write without response for speed
            await client.write_gatt_char(rx_char, chunk, response=False)
            bytes_sent += len(chunk)
            
            # Simple progress reporting
            percent = (bytes_sent / payload_size) * 100
            print(f"Sent {bytes_sent / 1024.0:.1f} KB / {payload_size / 1024.0:.1f} KB ({percent:.1f}%)", end="\r")
            
            # Brief delay to allow nRF52 BLE stack to process flash writes without overflow
            await asyncio.sleep(0.005)
            
        print(f"\nTransmission complete! Sent {bytes_sent} bytes.")
        print("Waiting 4 seconds for the device to finalize the LittleFS save stream...")
        await asyncio.sleep(4.0)
        print("Done! You can now safely disconnect and read.")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nUpload cancelled by user.")
