# Changelog

[简体中文](CHANGELOG.zh-CN.md) | [繁體中文](CHANGELOG.zh-TW.md) | English

## [2.0.0] - 2026-11-08

### Upgrade notes

- **Upgrade every node in a cluster to 2.0.0 at the same time.** 1.0.0 and 2.0.0 nodes cannot talk to each other. Settings and device labels are kept.
- HTTP API: `/api/state` no longer returns logs. Use `POST /api/logs` instead.
- Running from source now needs bleak 2.x and the new dependency `ifaddr`.

### Added

- **ESP32-C3 version**: flash it onto an ESP32-C3 board and plug the board into a USB charger. It keeps your clocks set on its own and can join a cluster with PCs and NAS boxes.
  - One-click flash package: `flash.bat` finds the serial port, writes the firmware and sets up Wi-Fi over USB
  - When no Wi-Fi is set up or it cannot connect, the board opens a hotspot. Open `192.168.4.1` on your phone to set up Wi-Fi
  - Device config page `/config`: view and change Wi-Fi, pick the time zone, and upload firmware updates from the browser. If new firmware cannot get online, the board goes back to the previous version
- **Time source**: set clocks from the local clock or from an NTP server (pick a preset or enter your own)
- Node cards are clickable and open that node's page

### Changed

- Logs are fetched incrementally per node, so an open page costs less
- The device table only redraws what changed and no longer flickers
- Clear messages when there is no Bluetooth adapter, BlueZ is not running, and similar cases
- Clicking a device that is already queued now says so
- Linux: reinstalling the deb after a port change removes the firewall rule for the old port

### Fixed

- Machines with several network adapters sent node beacons on only one of them, so nodes on other subnets could not find them
- Invalid characters in node data got the whole node rejected or broke the page
- Requests between nodes used the wrong timeouts and went through the system proxy, so nodes timed out and dropped off often
- The ESP32 was slow to respond with Wi-Fi power saving on. Power saving is now turned off while Bluetooth is idle
- Bluetooth could hang on startup with no timeout
- A peer with clustering turned off was reported as a problem on this machine
- Windows sometimes reported low system resources when there were none
- Extra blank space at the top of the settings window

## [1.0.0] - 2026-10-31

First release: Windows installer and Linux deb, web UI, scheduled automatic sync, multi-machine cluster.

[2.0.0]: https://github.com/wildcatkk/lywsd02-clock-sync/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/wildcatkk/lywsd02-clock-sync/releases/tag/v1.0.0
