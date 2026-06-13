import asyncio
import traceback
from uuid import UUID
from winrt.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementPublisher,
    BluetoothLEAdvertisement,
)

async def main():
    try:
        # Create custom advertisement
        advertisement = BluetoothLEAdvertisement()
        print("Created advertisement object.")
        
        # Add the NUS Service UUID
        nus_uuid = UUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        print(f"NUS UUID created: {nus_uuid}")
        advertisement.service_uuids.append(nus_uuid)
        print("Appended service UUID.")
        
        # Pass advertisement to publisher constructor
        publisher = BluetoothLEAdvertisementPublisher(advertisement)
        print("Created publisher object.")
        
        # Start publishing
        publisher.start()
        print("BluetoothLEAdvertisementPublisher started successfully!")
        print("Advertising NUS service UUID...")
        
        # Keep running for 15 seconds to check for errors
        for i in range(15):
            print(f"Status: {publisher.status}") # 0=Created, 1=Started, 2=Stopping, 3=Stopped, 4=Aborted
            if publisher.status == 4: # Aborted
                break
            await asyncio.sleep(1)
            
        publisher.stop()
        print("Publisher stopped.")
    except Exception as e:
        print("Error with publisher:")
        traceback.print_exc()

asyncio.run(main())
