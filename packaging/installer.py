"""Entry point of LYWSD02ClockSync-Setup.exe; build.py bundles the program as app.zip."""

import os
import sys

import winsetup

if __name__ == "__main__":
    bundle_dir = getattr(sys, "_MEIPASS", os.path.join(os.path.dirname(__file__), "..", "build"))
    sys.exit(winsetup.setup_main(sys.argv[1:], os.path.join(bundle_dir, "app.zip")))
