#!/usr/bin/env python3
"""Write the GitHub release notes for the version in VERSION.

    python packaging/release_notes.py OUTPUT

GitHub releases have no language switch, so the Chinese sections of
CHANGELOG.zh-CN.md and CHANGELOG.zh-TW.md go on top as collapsed blocks and the
English section of CHANGELOG.md follows, open. Each gets its own download table.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILES = (
    "LYWSD02ClockSync-Setup-{v}-x64.exe",
    "LYWSD02ClockSync-Setup-{v}-arm64.exe",
    "lywsd02-clock-sync_{v}_amd64.deb",
    "lywsd02-clock-sync_{v}_arm64.deb",
    "LYWSD02-Clock-Sync-ESP32-{v}-windows.zip",
    "lywsd02-clock-sync-{v}-esp32c3-full.bin",
    "lywsd02-clock-sync-{v}-esp32c3-ota.bin",
)

# (changelog, block title, heading, table header, one label per entry in FILES)
LANGUAGES = (
    ("CHANGELOG.md", None, "Downloads", ("Platform", "File"), (
        "Windows x64",
        "Windows ARM64 (experimental)",
        "Linux x86_64",
        "Linux ARM64",
        "ESP32-C3 first flash (Windows one-click package)",
        "ESP32-C3 first flash (write to 0x0 with esptool)",
        "ESP32-C3 upgrade from the web page",
    )),
    ("CHANGELOG.zh-CN.md", "简体中文", "下载", ("平台", "文件"), (
        "Windows x64",
        "Windows ARM64（实验性）",
        "Linux x86_64",
        "Linux ARM64",
        "ESP32-C3 首次烧录（Windows 一键包）",
        "ESP32-C3 首次烧录（用 esptool 写到 0x0）",
        "ESP32-C3 网页升级",
    )),
    ("CHANGELOG.zh-TW.md", "繁體中文", "下載", ("平台", "檔案"), (
        "Windows x64",
        "Windows ARM64（實驗性）",
        "Linux x86_64",
        "Linux ARM64",
        "ESP32-C3 首次燒錄（Windows 一鍵包）",
        "ESP32-C3 首次燒錄（用 esptool 寫到 0x0）",
        "ESP32-C3 網頁升級",
    )),
)


def section(name, version):
    lines = None
    with open(os.path.join(ROOT, name), encoding="utf-8") as f:
        for line in f.read().splitlines():
            if line.startswith("## ["):
                if lines is not None:
                    break
                if line.startswith(f"## [{version}]"):
                    lines = []
            elif lines is not None:
                if re.match(r"\[[^\]]+\]: ", line):
                    break
                lines.append(line)
    text = "\n".join(lines or []).strip()
    if not text:
        print(f"::error::{name} has no section for {version}")
        sys.exit(1)
    return text


def notes(version, name, heading, header, labels):
    rows = [f"| {header[0]} | {header[1]} |", "| --- | --- |"]
    rows += [f"| {label} | `{file.format(v=version)}` |" for label, file in zip(labels, FILES)]
    return f"{section(name, version)}\n\n### {heading}\n\n" + "\n".join(rows)


def main():
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
        version = f.read().strip()
    english, blocks = None, []
    for name, title, heading, header, labels in LANGUAGES:
        body = notes(version, name, heading, header, labels)
        if title is None:
            english = body
        else:
            blocks.append(f"<details>\n<summary>{title}</summary>\n\n{body}\n\n</details>")
    with open(sys.argv[1], "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(blocks) + "\n\n" + english + "\n")


if __name__ == "__main__":
    main()
