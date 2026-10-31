#!/usr/bin/env python3
"""Build the release package for the platform this runs on.

    python packaging/build.py

Windows  dist/LYWSD02ClockSync-Setup-<version>-<x64|arm64>.exe
Linux    dist/lywsd02-clock-sync_<version>_<amd64|arm64|...>.deb

Dependencies and PyInstaller go into a private venv, build/venv, so nothing else
installed on the machine ends up in the package. PyInstaller cannot cross-build:
every architecture is built on a machine of that architecture.

The deb needs a glibc at least as new as the build machine's, so build it on the
oldest system it has to run on. Debian 12 (glibc 2.36) covers fnOS, Debian 12+,
Raspberry Pi OS bookworm and Ubuntu 23.04+. Needs: python3-venv python3-dev binutils.
DEB_MAINTAINER and DEB_HOMEPAGE fill the matching control fields.
"""

import os
import platform
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGING = os.path.join(ROOT, "packaging")
BUILD = os.path.join(ROOT, "build")
DIST = os.path.join(ROOT, "dist")
VENV = os.path.join(BUILD, "venv")
IS_WINDOWS = sys.platform == "win32"
APP = "LYWSD02ClockSync"
PACKAGE = "lywsd02-clock-sync"
SCRIPTS = ("postinst", "prerm", "postrm")


def run(command):
    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)


def ensure_venv():
    """Re-run this script inside build/venv, creating and filling it first."""
    python = os.path.join(VENV, "Scripts", "python.exe") if IS_WINDOWS \
        else os.path.join(VENV, "bin", "python")
    if os.path.normcase(os.path.abspath(sys.prefix)) == os.path.normcase(VENV):
        return
    if not os.path.exists(python):
        run([sys.executable, "-m", "venv", VENV])
    # PIP_INDEX_URL is honoured here like anywhere else, for a closer mirror.
    run([python, "-m", "pip", "install", "--disable-pip-version-check", "--quiet",
         "-r", os.path.join(ROOT, "requirements.txt"),
         "-r", os.path.join(PACKAGING, "requirements-build.txt")])
    sys.exit(subprocess.call([python, os.path.abspath(__file__)] + sys.argv[1:]))


def read_version():
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as handle:
        version = handle.read().strip()
    tag = os.environ.get("GITHUB_REF_NAME", "")
    if os.environ.get("GITHUB_REF_TYPE") == "tag" and tag != "v" + version:
        sys.exit(f"tag {tag} does not match VERSION {version}")
    return version


def data(source, destination):
    return f"--add-data={os.path.join(ROOT, source)}{os.pathsep}{destination}"


def pyinstaller(args):
    run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--noupx",
         "--log-level", "WARN", "--workpath", os.path.join(BUILD, "work"),
         "--specpath", os.path.join(BUILD, "spec")] + args)


def fresh(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    elif os.path.exists(path):
        os.remove(path)


def build_windows(version):
    machine = platform.machine().upper()
    arch = {"AMD64": "x64", "ARM64": "arm64"}.get(machine, machine.lower())
    icon = os.path.join(PACKAGING, "icon.ico")
    apps = os.path.join(BUILD, "app")
    fresh(apps)
    pyinstaller(["--onedir", "--windowed", "--name", APP, "--icon", icon, "--distpath", apps,
                 data("static", "static"), data("VERSION", "."),
                 data(os.path.join("packaging", "icon.ico"), "."),
                 "--collect-submodules", "uvicorn", "--collect-submodules", "bleak",
                 os.path.join(ROOT, "app.py")])

    bundle = os.path.join(apps, APP)
    payload = os.path.join(BUILD, "app.zip")
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED) as archive:
        for folder, _, files in os.walk(bundle):
            for name in files:
                path = os.path.join(folder, name)
                archive.write(path, os.path.relpath(path, bundle))

    # Elevated from the start because of the firewall rule and the scheduled task.
    pyinstaller(["--onefile", "--windowed", "--uac-admin",
                 "--name", f"{APP}-Setup-{version}-{arch}", "--icon", icon,
                 "--distpath", DIST, "--paths", ROOT,
                 data(os.path.join("build", "app.zip"), "."), data("VERSION", "."),
                 data(os.path.join("packaging", "icon.ico"), "."),
                 os.path.join(PACKAGING, "installer.py")])
    return os.path.join(DIST, f"{APP}-Setup-{version}-{arch}.exe")


def copy_lf(source, destination, mode):
    # A checkout on Windows may have turned these into CRLF, which /bin/sh rejects.
    with open(source, "rb") as handle:
        content = handle.read().replace(b"\r\n", b"\n")
    with open(destination, "wb") as handle:
        handle.write(content)
    os.chmod(destination, mode)


def build_deb(version):
    arch = subprocess.run(["dpkg", "--print-architecture"], check=True, capture_output=True,
                          text=True).stdout.strip()
    glibc = os.confstr("CS_GNU_LIBC_VERSION").split()[-1]
    apps = os.path.join(BUILD, "app")
    fresh(apps)
    pyinstaller(["--onedir", "--name", PACKAGE, "--distpath", apps,
                 data("static", "static"), data("VERSION", "."),
                 "--collect-submodules", "uvicorn", "--collect-submodules", "bleak",
                 "--collect-submodules", "dbus_fast",
                 "--exclude-module", "winsetup", "--exclude-module", "tkinter",
                 os.path.join(ROOT, "app.py")])

    stage = os.path.join(BUILD, "deb")
    fresh(stage)
    shutil.copytree(os.path.join(apps, PACKAGE), os.path.join(stage, "opt", PACKAGE),
                    symlinks=True)
    units = os.path.join(stage, "usr", "lib", "systemd", "system")
    docs = os.path.join(stage, "usr", "share", "doc", PACKAGE)
    control = os.path.join(stage, "DEBIAN")
    for folder in (units, docs, control):
        os.makedirs(folder)
    copy_lf(os.path.join(PACKAGING, "linux", PACKAGE + ".service"),
            os.path.join(units, PACKAGE + ".service"), 0o644)
    copy_lf(os.path.join(ROOT, "LICENSE"), os.path.join(docs, "copyright"), 0o644)
    for name in SCRIPTS:
        copy_lf(os.path.join(PACKAGING, "linux", name), os.path.join(control, name), 0o755)

    size = 0
    for folder, _, files in os.walk(stage):
        if not folder.startswith(control):
            size += sum(os.path.getsize(os.path.join(folder, name)) for name in files
                        if not os.path.islink(os.path.join(folder, name)))
    fields = [
        ("Package", PACKAGE),
        ("Version", version),
        ("Architecture", arch),
        ("Maintainer", os.environ.get("DEB_MAINTAINER")
         or "LYWSD02 Clock Sync maintainers <maintainers@localhost>"),
        ("Installed-Size", str(size // 1024 + 1)),
        ("Depends", f"libc6 (>= {glibc})"),
        # Recommends rather than Depends: some NAS systems ship bluetoothd without
        # registering the bluez package, and dpkg -i would refuse to go on.
        ("Recommends", "bluez"),
        ("Section", "utils"),
        ("Priority", "optional"),
        ("Homepage", os.environ.get("DEB_HOMEPAGE", "")),
        ("Description", "keeps LYWSD02 e-ink thermometer clocks on time over Bluetooth\n"
         " A web page and a scheduler that set the time on the Mi Temperature and\n"
         " Humidity Monitor Digital Clock (LYWSD02, LYWSD02MMC). Several machines can\n"
         " share the work as a cluster without a master; each syncs the clocks it\n"
         " hears best."),
    ]
    with open(os.path.join(control, "control"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("".join(f"{name}: {value}\n" for name, value in fields if value))

    os.makedirs(DIST, exist_ok=True)
    output = os.path.join(DIST, f"{PACKAGE}_{version}_{arch}.deb")
    run(["dpkg-deb", "--build", "--root-owner-group", "-Zxz", stage, output])
    return output


def main():
    ensure_venv()
    version = read_version()
    output = build_windows(version) if IS_WINDOWS else build_deb(version)
    print("\nbuilt " + output)


if __name__ == "__main__":
    main()
