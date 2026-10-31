"""Where the program's own files are, and where it keeps yours.

The two are apart once the program is packaged: the bundle is replaced on every
upgrade, while settings, labels and readings must outlive it.
"""

import os
import sys

APP_NAME = "LYWSD02ClockSync"
PACKAGE = "lywsd02-clock-sync"
FROZEN = getattr(sys, "frozen", False)
IS_WINDOWS = sys.platform == "win32"

# Read-only: the web page and VERSION. A PyInstaller bundle unpacks them to _MEIPASS.
RESOURCE_DIR = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))


def windows_data_dir():
    local = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~\\AppData\\Local")
    return os.path.join(local, APP_NAME)


def data_dir():
    if os.environ.get("LYWSD02_DATA_DIR"):
        return os.environ["LYWSD02_DATA_DIR"]
    # systemd's StateDirectory= hands the service a colon-separated list here.
    if os.environ.get("STATE_DIRECTORY"):
        return os.environ["STATE_DIRECTORY"].split(":")[0]
    if not FROZEN:
        return RESOURCE_DIR
    if IS_WINDOWS:
        return windows_data_dir()
    return f"/var/lib/{PACKAGE}"


def version():
    try:
        with open(os.path.join(RESOURCE_DIR, "VERSION"), encoding="utf-8") as handle:
            return handle.read().strip() or "0.0.0"
    except OSError:
        return "0.0.0"
