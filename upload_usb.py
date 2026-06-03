# High-Speed USB Serial Book Uploader for nice!nano e-Paper Book Reader
# Requires pyserial: pip install pyserial

import sys
import os
import time
import serial
import serial.tools.list_ports

def get_ports():
    return [p.device for p in serial.tools.list_ports.comports()]

def main():
    # 1. Resolve COM Port
    ports = get_ports()
    if not ports:
        print("Error: No active COM/Serial ports found. Make sure your board is plugged into USB.")
        sys.exit(1)

    port = None
    if len(ports) == 1:
        port = ports[0]
        print(f"Automatically selected nice!nano on port: {port}")
    else:
        print("\nAvailable serial ports:")
        for idx, p in enumerate(ports):
            print(f" [{idx + 1}] {p}")
        choice = input("Select COM port (1-{}): ".format(len(ports))).strip()
        try:
            port = ports[int(choice) - 1]
        except (ValueError, IndexError):
            print("Invalid port choice.")
            sys.exit(1)

    # 2. Resolve File Path
    if len(sys.argv) > 1:
        file_path = sys.argv[1].strip("'\"")
    else:
        file_path = input("Enter local text file path (e.g. book.txt): ").strip("'\"")

    if not os.path.exists(file_path):
        print(f"Error: Local file '{file_path}' does not exist.")
        sys.exit(1)

    # 3. Read Book Content
    print(f"Reading '{file_path}'...")
    with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()

    lines = content.splitlines()
    if not lines:
        print("Error: The book file is empty.")
        sys.exit(1)

    default_title = lines[0].strip()
    print(f"Detected book title inside file: '{default_title}'")
    custom_title = input(f"Enter custom title (or Enter for '{default_title}'): ").strip()
    final_title = custom_title if custom_title else default_title

    payload = content.encode("utf-8")
    payload_size = len(payload)

    # 4. Open connection
    print(f"\nConnecting to {port} at 115200 baud...")
    try:
        # Open with a 3-second read timeout
        ser = serial.Serial(port, 115200, timeout=3.0)
    except Exception as e:
        print(f"Error opening serial port: {e}")
        sys.exit(1)

    time.sleep(1.5) # Wait for serial bridge connection to settle
    ser.reset_input_buffer()

    print(f"Initiating upload: '{final_title}' ({payload_size} bytes) over USB...")
    handshake = f"USB_UPLOAD:{payload_size}:{final_title}\n"
    ser.write(handshake.encode("utf-8"))
    ser.flush()

    # Wait for device acknowledgement
    resp = ser.readline().decode("utf-8", errors="ignore").strip()
    if resp != "USB_READY":
        print(f"Error: Device did not respond with USB_READY. Got: '{resp}'")
        ser.close()
        sys.exit(1)

    print("Device is ready! Streaming text over USB...")
    
    start_time = time.time()
    chunk_size = 64
    bytes_sent = 0

    for i in range(0, payload_size, chunk_size):
        chunk = payload[i:i+chunk_size]
        ser.write(chunk)
        bytes_sent += len(chunk)
        
        percent = (bytes_sent / payload_size) * 100
        print(f"Progress: {bytes_sent}/{payload_size} bytes ({percent:.1f}%)", end="\r")
        time.sleep(0.005) # Yield and pace for the USB CDC endpoint buffer and flash erases
    
    ser.flush()
    elapsed = time.time() - start_time
    print(f"\nFinished sending in {elapsed:.2f} seconds ({bytes_sent / 1024.0 / elapsed:.1f} KB/s)")

    print("Waiting for save confirmation...")
    
    while True:
        finalize_resp = ser.readline().decode("utf-8", errors="ignore").strip()
        if not finalize_resp:
            print("\nError: Timeout waiting for save confirmation from device.")
            break
            
        if finalize_resp == "USB_SUCCESS":
            print("\nSuccess: Book uploaded and saved successfully over USB!")
            break
        elif finalize_resp == "USB_FAILED":
            print("\nError: Device failed to finalize book save.")
            break
        elif finalize_resp == "USB_TIMEOUT":
            print("\nError: Device timed out waiting for all bytes.")
            break
        else:
            # Print intermediate debug outputs from the device (e.g. "Started writing...", "Renaming...")
            print(f" [Device Debug] {finalize_resp}")

    ser.close()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nUpload cancelled.")
