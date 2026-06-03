# Interactive BLE Control Panel for nice!nano e-Paper Book Reader
# Requires bleak: pip install bleak

import asyncio
import os
import sys
from bleak import BleakScanner, BleakClient

# Nordic UART Service (NUS) UUIDs
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Client writes to RX
NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # Client subscribes to TX (notify)

TARGET_DEVICE_PREFIX = "nRF_E"

class BleConsoleApp:
    def __init__(self):
        self.client = None
        self.rx_char = None
        self.tx_char = None
        
        # Buffer and queue for handling notifications
        self.line_buffer = ""
        self.response_future = None
        self.list_mode = False
        self.list_accumulator = []

    def notification_handler(self, sender, data):
        # Decode and append to line buffer
        chunk = data.decode("utf-8", errors="ignore")
        self.line_buffer += chunk
        
        while "\n" in self.line_buffer:
            line, self.line_buffer = self.line_buffer.split("\n", 1)
            line = line.strip()
            if not line:
                continue
                
            self.handle_line(line)

    def handle_line(self, line):
        # Handle list aggregation
        if line == "LIST_START":
            self.list_mode = True
            self.list_accumulator = []
            return
        elif line == "LIST_END":
            self.list_mode = False
            if self.response_future and not self.response_future.done():
                self.response_future.set_result(self.list_accumulator)
            return
            
        if self.list_mode:
            self.list_accumulator.append(line)
            return
            
        # Signal regular response completions
        if self.response_future and not self.response_future.done():
            self.response_future.set_result(line)

    async def send_command(self, cmd_str, timeout=5.0, wait_response=True):
        # Reset future
        self.response_future = asyncio.get_running_loop().create_future()
        
        # Send line in 20-byte chunks to avoid Windows MTU/WinRT parameter errors
        payload = (cmd_str + "\n").encode("utf-8")
        chunk_size = 20
        for i in range(0, len(payload), chunk_size):
            chunk = payload[i:i+chunk_size]
            await self.client.write_gatt_char(self.rx_char, chunk, response=False)
            await asyncio.sleep(0.005)
            
        if not wait_response:
            return "BTN_SUCCESS"

        # Wait for reply
        try:
            return await asyncio.wait_for(self.response_future, timeout)
        except asyncio.TimeoutError:
            print(f"Warning: Timeout waiting for response to command: {cmd_str}")
            return None

    async def show_stats(self):
        print("\n=== Fetching Device Statistics ===")
        res = await self.send_command("CMD_STATS")
        if not res or not res.startswith("STATS:"):
            print("Error: Could not retrieve statistics from device.")
            return

        # Format: STATS:vbat=3.85|percent=72|rssi=-65|used=45056|total=235520
        stats_raw = res[6:]
        stats = {}
        for part in stats_raw.split("|"):
            if "=" in part:
                k, v = part.split("=", 1)
                stats[k] = v

        vbat = stats.get("vbat", "0.0")
        percent = stats.get("percent", "0")
        rssi = stats.get("rssi", "0")
        used = int(stats.get("used", "0"))
        total = int(stats.get("total", "235520"))
        free = total - used

        print(f"Battery Voltage : {vbat} V")
        print(f"Battery Charge  : {percent}%")
        print(f"Signal Strength : {rssi} dBm")
        print(f"Used Storage    : {used / 1024.0:.1f} KB")
        print(f"Free Storage    : {free / 1024.0:.1f} KB")
        print(f"Total Storage   : {total / 1024.0:.1f} KB")

        # Visual progress bar for storage
        bar_len = 20
        filled = int((used / total) * bar_len)
        bar = "#" * filled + "-" * (bar_len - filled)
        percent_used = (used / total) * 100
        print(f"Storage Usage   : [{bar}] {percent_used:.1f}%")

    async def list_books(self):
        print("\n=== Book List ===")
        res = await self.send_command("CMD_LIST")
        if res is None:
            print("Error: Could not retrieve book list.")
            return []
            
        if not res:
            print("No books found on device storage.")
            return []

        books = []
        for line in res:
            if ":" in line:
                name, size = line.split(":", 1)
                books.append((name, int(size)))
                
        for idx, (name, size) in enumerate(books):
            clean_name = name.replace(".txt", "").replace("_", " ")
            print(f" [{idx + 1}] {clean_name:<30} ({size / 1024.0:.1f} KB)")
            
        return books

    async def delete_book_ui(self):
        books = await self.list_books()
        if not books:
            return

        choice = input("\nEnter the index of the book to delete (or Enter to cancel): ").strip()
        if not choice:
            return
            
        try:
            idx = int(choice) - 1
            if idx < 0 or idx >= len(books):
                print("Invalid index choice.")
                return
        except ValueError:
            print("Invalid input.")
            return

        target_book = books[idx][0]
        confirm = input(f"Are you sure you want to delete '{target_book}'? (y/N): ").strip().lower()
        if confirm != 'y':
            print("Deletion cancelled.")
            return

        print(f"Deleting '{target_book}'...")
        res = await self.send_command(f"CMD_DELETE:{target_book}")
        if res and res.startswith("DELETE_SUCCESS:"):
            print("Book deleted successfully!")
        else:
            print("Error deleting book from device.")

    async def upload_book_ui(self):
        path = input("\nEnter local text file path: ").strip()
        # Strip quotes if dragged-and-dropped in terminal
        path = path.strip("'\"")
        if not os.path.exists(path):
            print("Error: Local file does not exist.")
            return

        # Read content
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()

        lines = content.splitlines()
        if not lines:
            print("Error: The book file is empty.")
            return

        default_title = lines[0].strip()
        print(f"Detected book title inside file: '{default_title}'")
        
        custom_title = input(f"Enter custom title (or Enter for '{default_title}'): ").strip()
        final_title = custom_title if custom_title else default_title

        payload = content.encode("utf-8")
        payload_size = len(payload)

        print(f"\nInitiating upload: '{final_title}' ({payload_size} bytes)...")
        
        # 1. Send upload initialization command
        res = await self.send_command(f"CMD_UPLOAD:{payload_size}:{final_title}")
        if res != "UPLOAD_READY":
            print(f"Error: Device rejected upload command. Response: {res}")
            return

        print("Device is ready! Streaming text chunks...")

        # 2. Stream chunk payload
        chunk_size = 20
        bytes_sent = 0
        
        # Configure future to wait for complete status
        self.response_future = asyncio.get_running_loop().create_future()

        for i in range(0, payload_size, chunk_size):
            chunk = payload[i:i+chunk_size]
            await self.client.write_gatt_char(self.rx_char, chunk, response=False)
            bytes_sent += len(chunk)
            
            percent = (bytes_sent / payload_size) * 100
            print(f"Progress: {bytes_sent}/{payload_size} bytes ({percent:.1f}%)", end="\r")
            
            # Brief delay to allow nRF52 flash task processing
            await asyncio.sleep(0.01)

        print("\nStreaming finished! Waiting for finalize response...")
        
        # 3. Wait for final save response
        try:
            finalize_res = await asyncio.wait_for(self.response_future, 5.0)
            if finalize_res == "UPLOAD_SUCCESS":
                print("Book uploaded and saved successfully!")
            else:
                print(f"Error saving book. Response: {finalize_res}")
        except asyncio.TimeoutError:
            print("Warning: Timeout waiting for upload completion callback.")

    async def remote_page_control_ui(self):
        print("\n=== Remote Page Control ===")
        print(" (Press 'n' for Next page, 'p' for Previous page, 'q' to Return to main menu)")
        while True:
            action = input("Page action (n/p/q): ").strip().lower()
            if action == 'n':
                print("Sending Next Page command...")
                await self.send_command("CMD_PAGE_NEXT", wait_response=False)
                print("Next Page command sent.")
            elif action == 'p':
                print("Sending Previous Page command...")
                await self.send_command("CMD_PAGE_PREV", wait_response=False)
                print("Previous Page command sent.")
            elif action == 'q':
                break
            else:
                print("Invalid choice. Enter 'n', 'p', or 'q'.")

    async def simulate_buttons_ui(self):
        print("\n=== Simulate Onboard Buttons ===")
        print(" (Press 'p' for Prev click, 'n' for Next click, 's' for Select click, 'l' for Select Long Press, 'q' to Return)")
        while True:
            action = input("Simulate input (p/n/s/l/q): ").strip().lower()
            if action == 'p':
                print("Sending Prev button click...")
                await self.send_command("CMD_BTN_PREV", wait_response=False)
                print("Prev button click command sent.")
            elif action == 'n':
                print("Sending Next button click...")
                await self.send_command("CMD_BTN_NEXT", wait_response=False)
                print("Next button click command sent.")
            elif action == 's':
                print("Sending Select button click...")
                await self.send_command("CMD_BTN_SELECT", wait_response=False)
                print("Select button click command sent.")
            elif action == 'l':
                print("Sending Select button Long Press...")
                await self.send_command("CMD_BTN_SELECT_LONG", wait_response=False)
                print("Select button Long Press command sent.")
            elif action == 'q':
                break
            else:
                print("Invalid choice. Enter 'p', 'n', 's', 'l', or 'q'.")

    async def run(self):
        # Discover target device
        print(f"Scanning for BLE device with prefix '{TARGET_DEVICE_PREFIX}'...")
        device = None
        devices = await BleakScanner.discover(timeout=5.0)
        for d in devices:
            if d.name and d.name.startswith(TARGET_DEVICE_PREFIX):
                device = d
                break
                
        if not device:
            print(f"Error: Could not find BLE device starting with '{TARGET_DEVICE_PREFIX}'.")
            print("Please make sure the board is powered on and advertising BLE.")
            return

        print(f"Found device: {device.name} [{device.address}]")
        print("Connecting to device...")
        
        async with BleakClient(device) as self.client:
            print("Connected successfully!")
            
            # Retrieve characteristics
            nus_service = self.client.services.get_service(NUS_SERVICE_UUID)
            if not nus_service:
                print("Error: Nordic UART Service (NUS) not found.")
                return
                
            self.rx_char = nus_service.get_characteristic(NUS_RX_CHAR_UUID)
            self.tx_char = nus_service.get_characteristic(NUS_TX_CHAR_UUID)
            
            if not self.rx_char or not self.tx_char:
                print("Error: NUS RX or TX characteristic is missing.")
                return

            # Subscribe to notifications
            await self.client.start_notify(self.tx_char, self.notification_handler)
            print("Subscribed to BLE notifications.")

            # Main interactive prompt loop
            while True:
                print("\n==============================")
                print("  BLE e-Paper Book Controller  ")
                print("==============================")
                print(" [1] Show Device Statistics")
                print(" [2] List Books on Device")
                print(" [3] Delete a Book")
                print(" [4] Upload a New Book")
                print(" [5] Remote Page Control (Prev/Next)")
                print(" [6] Simulate Onboard Buttons (Prev/Next/Select)")
                print(" [7] Exit")
                print("==============================")
                
                choice = input("Enter choice (1-7): ").strip()
                if choice == "1":
                    await self.show_stats()
                elif choice == "2":
                    await self.list_books()
                elif choice == "3":
                    await self.delete_book_ui()
                elif choice == "4":
                    await self.upload_book_ui()
                elif choice == "5":
                    await self.remote_page_control_ui()
                elif choice == "6":
                    await self.simulate_buttons_ui()
                elif choice == "7":
                    print("Disconnecting...")
                    break
                else:
                    print("Invalid option. Please choose 1-7.")

                await asyncio.sleep(0.1)

            # Unsubscribe upon exit
            await self.client.stop_notify(self.tx_char)

if __name__ == "__main__":
    try:
        app = BleConsoleApp()
        asyncio.run(app.run())
    except KeyboardInterrupt:
        print("\nApplication closed by user.")
    except Exception as e:
        print(f"\nError: {e}")
