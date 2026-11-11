"""
Compiles the spear code in Code/ into Code/build/*.mpy for the XBee 3 Global LTE-M/NB-IoT.

Produces exactly what the Digi XBee PyCharm plugin builds: mpy-cross 1.18 (.mpy format v5), no unicode support,
and each file's on-device path (/flash/...) embedded as its source name. Checked byte-for-byte against the
plugin's build of Xbee_Code_20Mar26.

Run from this folder (needs uv):
    uv run --no-project --with mpy-cross==1.18 python build_mpy.py

The files in Code/build/ can then be copied to /flash on the XBee with XBee Studio's file system tool.
"""
import os
import subprocess
import sys

import mpy_cross

CODE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Code")
BUILD_DIR = os.path.join(CODE_DIR, "build")
MODULES = ["main", "DataTransmitter", "I2CModules", "Indicators", "lib/umqtt/__init__", "lib/umqtt/simple"]


def main():
    failed = False
    for module in MODULES:
        source = os.path.join(CODE_DIR, module + ".py")
        target = os.path.join(BUILD_DIR, module + ".mpy")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        process = mpy_cross.run("-mno-unicode", "-s", "/flash/%s.py" % module, "-o", target, source,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = process.communicate()[0].decode().strip()
        if process.returncode != 0 or output:
            failed = True
            print("FAILED  %s\n%s" % (module, output))
        else:
            with open(target, "rb") as built:  # Read back rather than os.path.getsize: Google Drive reports stale sizes
                size = len(built.read())
            print("built   %-22s %6d bytes" % (module + ".mpy", size))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
