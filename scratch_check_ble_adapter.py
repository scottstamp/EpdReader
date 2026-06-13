import asyncio
from winrt.windows.devices.bluetooth import BluetoothAdapter

async def main():
    try:
        adapter = await BluetoothAdapter.get_default_async()
        if adapter is None:
            print("No Bluetooth adapter found on this PC!")
            return
        
        print(f"Bluetooth Adapter Found: {adapter.device_id}")
        print(f"Bluetooth Address: {hex(adapter.bluetooth_address)}")
        print(f"Peripheral Role Supported: {adapter.is_peripheral_role_supported}")
        
        # Check if the radio is on
        radio = await adapter.get_radio_async()
        if radio is not None:
            state_names = {0: "Unknown", 1: "On", 2: "Off", 3: "Disabled"}
            print(f"Radio State: {state_names.get(radio.state, str(radio.state))}")
        else:
            print("Could not query radio state (get_radio_async returned None).")
            
    except Exception as e:
        print("Error querying Bluetooth adapter:", e)

asyncio.run(main())
