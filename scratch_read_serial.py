import serial
import time
import sys

def main():
    port = "COM33"
    baud = 115200
    print(f"Connecting to {port} at {baud} baud...")
    try:
        ser = serial.Serial(port, baud, timeout=2)
        print("Connected! Listening for output (10 seconds)...")
        # Try to trigger a reboot by sending something or toggling DTR/RTS
        # Nice!nano bootloader/USB stack reboots when serial DTR/RTS is toggled
        ser.dtr = False
        time.sleep(0.1)
        ser.dtr = True
        
        start_time = time.time()
        while time.time() - start_time < 30:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore')
                print(f"DEVICE: {line.strip()}")
            time.sleep(0.01)
            
        ser.close()
        print("Done listening.")
    except Exception as e:
        print("Error reading serial:", e)

if __name__ == "__main__":
    main()
