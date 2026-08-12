import sys
import time
import serial

def trigger_bootloader_reset(port="COM33"):
    print(f"Opening {port} at 1200 baud...")
    try:
        ser = serial.Serial(port, baudrate=1200, timeout=1)
        ser.dtr = False
        time.sleep(0.1)
        ser.close()
        ser = serial.Serial(port, baudrate=1200, timeout=1)
        ser.dtr = False
        time.sleep(0.1)
        ser.close()
        print(f"Closed {port} at 1200 baud. Device should reset to bootloader mode.")
    except Exception as e:
        print(f"Failed to touch {port}: {e}")

if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else "COM33"
    trigger_bootloader_reset(port)
