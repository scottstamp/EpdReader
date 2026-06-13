import sys
import importlib.util

class BleakWinRTRedirector:
    def find_spec(self, fullname, path, target=None):
        if fullname.startswith("bleak_winrt"):
            real_name = fullname.replace("bleak_winrt", "winrt")
            try:
                spec = importlib.util.find_spec(real_name)
                if spec is not None:
                    # We need to load it under the bleak_winrt name so sys.modules is populated correctly
                    return spec
            except Exception as e:
                print(f"Error finding spec for {real_name}: {e}")
        return None

sys.meta_path.insert(0, BleakWinRTRedirector())

# Now try importing bless
try:
    import bless
    print("Imported bless successfully using redirector!")
except Exception as e:
    import traceback
    traceback.print_exc()
