#!/usr/bin/env python3
"""Build the Windows flash package from one or more firmware builds.

    python esp32/package.py --esptool DIR [--merge-tool CMD] [BUILD_DIR ...]

DIR holds esptool.exe and its LICENSE, as unpacked from the esptool Windows
release. CMD merges the images and defaults to that esptool.exe; off Windows
pass another esptool, e.g. "python -m esptool". BUILD_DIR defaults to
esp32/node/build; pass one per chip.

dist/LYWSD02-Clock-Sync-ESP32-<version>-windows.zip
    README.txt, flash.bat, monitor.bat, wifi.example.txt
    firmware/lywsd02-clock-sync-<version>-<chip>-full.bin   written at 0x0 by flash.bat
    firmware/lywsd02-clock-sync-<version>-<chip>-ota.bin    for the settings page upload
    tools/                                                  scripts and esptool.exe
dist/lywsd02-clock-sync-<version>-<chip>-{full,ota}.bin     the same images, for the release page
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import zipfile

ESP32 = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(ESP32)
FLASH = os.path.join(ESP32, "flash")
DIST = os.path.join(ROOT, "dist")
PACKAGE_FILES = ("README.txt", "flash.bat", "monitor.bat", "wifi.example.txt")
TOOL_FILES = ("common.ps1", "flash.ps1", "monitor.ps1")


def load(build, name):
    with open(os.path.join(build, name), encoding="utf-8") as f:
        return json.load(f)


def add_build(build, merge, firmware):
    project = load(build, "project_description.json")
    flasher = load(build, "flasher_args.json")
    chip = project["target"]
    version = project["project_version"]
    stem = f"{project['project_name']}-{version}-{chip}"

    settings = flasher["flash_settings"]
    command = merge + ["--chip", chip, "merge-bin", "-o", os.path.join(firmware, stem + "-full.bin"),
               "--flash-mode", settings["flash_mode"], "--flash-freq", settings["flash_freq"],
               "--flash-size", settings["flash_size"]]
    for offset, path in sorted(flasher["flash_files"].items(), key=lambda item: int(item[0], 16)):
        command += [offset, os.path.join(build, path)]
    print("$ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)
    shutil.copyfile(os.path.join(build, project["app_bin"]), os.path.join(firmware, stem + "-ota.bin"))
    return version


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--esptool", required=True)
    parser.add_argument("--merge-tool")
    parser.add_argument("builds", nargs="*", default=[os.path.join(ESP32, "node", "build")])
    args = parser.parse_args()
    esptool = os.path.join(args.esptool, "esptool.exe")
    merge = shlex.split(args.merge_tool) if args.merge_tool else [esptool]

    staging = os.path.join(DIST, "esp32-windows")
    shutil.rmtree(staging, ignore_errors=True)
    firmware = os.path.join(staging, "firmware")
    tools = os.path.join(staging, "tools")
    os.makedirs(firmware)
    os.makedirs(tools)

    versions = {add_build(build, merge, firmware) for build in args.builds}
    if len(versions) != 1:
        raise SystemExit(f"builds disagree on the version: {sorted(versions)}")
    version = versions.pop()

    for name in PACKAGE_FILES:
        shutil.copyfile(os.path.join(FLASH, name), os.path.join(staging, name))
    for name in TOOL_FILES:
        shutil.copyfile(os.path.join(FLASH, "tools", name), os.path.join(tools, name))
    shutil.copyfile(esptool, os.path.join(tools, "esptool.exe"))
    shutil.copyfile(os.path.join(args.esptool, "LICENSE"), os.path.join(tools, "esptool-LICENSE.txt"))

    top = f"LYWSD02-Clock-Sync-ESP32-{version}-windows"
    archive = os.path.join(DIST, top + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for folder, _, files in os.walk(staging):
            for name in sorted(files):
                path = os.path.join(folder, name)
                z.write(path, os.path.join(top, os.path.relpath(path, staging)))
    for name in sorted(os.listdir(firmware)):
        shutil.copyfile(os.path.join(firmware, name), os.path.join(DIST, name))
        print(os.path.join(DIST, name))
    shutil.rmtree(staging)
    print(archive)


if __name__ == "__main__":
    main()
