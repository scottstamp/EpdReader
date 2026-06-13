import sys

# Import the official winrt modules first
import winrt.windows.foundation
import winrt.windows.storage.streams
import winrt.windows.devices.bluetooth.genericattributeprofile

# Map the bleak_winrt module names to the official winrt modules in sys.modules
sys.modules['bleak_winrt'] = sys.modules['winrt']
sys.modules['bleak_winrt.windows'] = sys.modules['winrt.windows']
sys.modules['bleak_winrt.windows.foundation'] = sys.modules['winrt.windows.foundation']
sys.modules['bleak_winrt.windows.storage'] = sys.modules['winrt.windows.storage']
sys.modules['bleak_winrt.windows.storage.streams'] = sys.modules['winrt.windows.storage.streams']
sys.modules['bleak_winrt.windows.devices'] = sys.modules['winrt.windows.devices']
sys.modules['bleak_winrt.windows.devices.bluetooth'] = sys.modules['winrt.windows.devices.bluetooth']
sys.modules['bleak_winrt.windows.devices.bluetooth.genericattributeprofile'] = sys.modules['winrt.windows.devices.bluetooth.genericattributeprofile']

# Now try importing bless
try:
    import bless
    print("Success! bless imported successfully using explicit sys.modules mapping!")
    print("BlessServer:", bless.BlessServer)
except Exception as e:
    import traceback
    traceback.print_exc()
