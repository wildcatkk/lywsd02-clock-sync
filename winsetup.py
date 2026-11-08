"""Windows setup, control window and uninstaller.

LYWSD02ClockSync-Setup.exe (packaging/installer.py) calls setup_main with the
program bundle it carries; the installed LYWSD02ClockSync.exe calls
installed_main whenever it is started without "serve". Only the standard library
and tkinter are used, so nothing has to be installed beforehand.

    program  %LOCALAPPDATA%\\Programs\\LYWSD02ClockSync   replaced on every upgrade
    data     %LOCALAPPDATA%\\LYWSD02ClockSync             config, labels, readings, log

Setup options:      --silent --dir PATH --port N --language zh-CN|zh-TW|en
                    --no-autostart --no-firewall
Uninstall options:  --silent --purge (also delete the data directory)
"""

import base64
import ctypes
import json
import math
import os
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.error
import urllib.request
import winreg
import zipfile
import zlib
from ctypes import wintypes

import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog, ttk

import paths
import store

APP_NAME = paths.APP_NAME
EXE_NAME = APP_NAME + ".exe"
TASK_NAME = APP_NAME
RULE_NAME = "LYWSD02 Clock Sync"
BEACON_PORT = 8788
UNINSTALL_KEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + APP_NAME
PUBLISHER = "LYWSD02 Clock Sync"
DEFAULT_PORT = 8787
DATA_DIR = paths.windows_data_dir()
BUNDLE = (EXE_NAME, "_internal")
INSTALL_STEPS = ("halt", "copy", "settings", "register", "firewall_step", "service")
UNINSTALL_STEPS = ("u_stop", "u_task", "u_firewall", "u_entry", "u_data", "u_files")
LANGUAGE_NAMES = {"zh-CN": "简体中文", "zh-TW": "繁體中文", "en": "English"}

CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_BREAKAWAY_FROM_JOB = 0x01000000

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
OEM = f"cp{kernel32.GetOEMCP()}"
# The system proxy would otherwise be asked for 127.0.0.1 too.
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

TEXT = {
    "zh-CN": {
        "app": "米家电子温湿度计Pro自动校时",
        "setup_title": "{app}安装程序",
        "version": "版本 {version}",
        "language": "语言",
        "dir": "安装位置",
        "browse": "浏览…",
        "port": "网页端口",
        "autostart": "登录 Windows 时自动在后台运行",
        "firewall": "允许局域网里的其它设备访问（添加防火墙规则）",
        "data_note": "设置和设备数据存放在 {path}，升级时保留，卸载时默认也保留。",
        "install": "安装",
        "upgrade": "升级",
        "open": "打开网页",
        "close": "关闭",
        "bad_port": "端口要是 1 到 65535 之间的数字",
        "bad_dir": "安装位置要填带盘符的完整路径，并且不能是数据目录 {path}",
        "other_user": "程序正以 {elevated} 的身份运行，而登录的是 {user}。请用要使用本程序的账号"
                      "登录，并在这个账号下确认管理员权限后再运行",
        "usage": "无法识别的参数：{arg}",
        "step": "[{n}/{total}] {what}",
        "halt": "停止正在运行的服务",
        "halt_ok": "端口 {port} 现在是空闲的",
        "port_busy": "端口 {port} 还有程序在响应，可能是手动启动的 server.py 或别的程序。"
                     "结束它或者换一个端口再安装",
        "removed": "已删除 {name}",
        "nothing": "没有需要处理的内容",
        "copy": "复制程序文件",
        "copied": "已解压到 {path}",
        "settings": "写入设置",
        "settings_ok": "端口 {port}、语言 {language} 已写入 {path}",
        "register": "添加开始菜单快捷方式和卸载入口",
        "shortcut_ok": "已创建 {path}",
        "entry_ok": "已登记到“设置 → 应用 → 已安装的应用”",
        "firewall_step": "添加防火墙规则",
        "firewall_ok": "已允许 {path} 接受局域网连接（网页的 TCP {port} 和节点发现的 UDP {beacon}）",
        "skipped": "已跳过",
        "service": "注册并启动后台服务",
        "task_ok": "计划任务 {name} 已注册并启动，{when}",
        "task_logon": "以后每次登录都会自动运行",
        "task_manual": "不会自动运行，要用时从开始菜单打开程序，点“启动”",
        "service_ok": "服务已在端口 {port} 上响应",
        "service_dead": "等了 {seconds} 秒端口仍没有响应，原因请看 {path}",
        "done": "安装完成，打开 http://127.0.0.1:{port}",
        "failed": "安装没有完成：{error}",
        "installing": "正在安装…",
        "upgrading": "正在升级…",
        "installed": "安装完成",
        "upgraded": "升级完成",
        "failed_title": "安装没有完成",
        "back": "返回",
        "details_show": "显示详细信息",
        "details_hide": "隐藏详细信息",
        "command_failed": "命令失败（退出码 {code}）：{output}",
        "status_checking": "正在检查…",
        "status_running": "运行中",
        "status_stopped": "未运行",
        "starting": "正在启动…",
        "stopping": "正在停止…",
        "status_dead": "启动后端口仍没有响应，原因请看 {path}",
        "start": "启动",
        "stop": "停止",
        "data": "打开数据目录",
        "uninstall": "卸载",
        "uninstall_title": "卸载{app}",
        "uninstall_intro": "将停止后台服务，并删除程序文件、计划任务、防火墙规则、快捷方式和卸载入口。",
        "purge": "同时删除设置和设备数据",
        "u_stop": "停止后台服务",
        "still_up": "端口 {port} 还有程序在响应，可能是手动启动的 server.py，需要你自己结束它",
        "u_task": "删除计划任务",
        "task_removed": "计划任务 {name} 已删除",
        "u_firewall": "删除防火墙规则",
        "rule_removed": "防火墙规则 {name} 已删除",
        "u_entry": "删除开始菜单快捷方式和卸载入口",
        "entry_removed": "已从“已安装的应用”里移除",
        "u_data": "处理数据目录",
        "data_kept": "数据保留在 {path}，重新安装会接着用",
        "u_files": "删除程序文件",
        "files_later": "{path} 会在这个窗口关闭后删除",
        "u_done": "卸载完成",
        "u_failed": "卸载没有完成：{error}",
        "uninstalling": "正在卸载…",
        "u_failed_title": "卸载没有完成",
        "elevate_denied": "卸载需要管理员权限",
    },
    "zh-TW": {
        "app": "米家電子溫濕度計Pro自動校時",
        "setup_title": "{app}安裝程式",
        "version": "版本 {version}",
        "language": "語言",
        "dir": "安裝位置",
        "browse": "瀏覽…",
        "port": "網頁通訊埠",
        "autostart": "登入 Windows 時自動在背景執行",
        "firewall": "允許區域網路中的其他裝置存取（新增防火牆規則）",
        "data_note": "設定和裝置資料存放在 {path}，升級時保留，移除時預設也保留。",
        "install": "安裝",
        "upgrade": "升級",
        "open": "開啟網頁",
        "close": "關閉",
        "bad_port": "通訊埠必須是 1 到 65535 之間的數字",
        "bad_dir": "安裝位置必須是含磁碟機代號的完整路徑，且不能是資料目錄 {path}",
        "other_user": "程式正以 {elevated} 的身分執行，而登入的是 {user}。請用要使用本程式的帳號"
                      "登入，並在該帳號下確認系統管理員權限後再執行",
        "usage": "無法辨識的參數：{arg}",
        "step": "[{n}/{total}] {what}",
        "halt": "停止正在執行的服務",
        "halt_ok": "通訊埠 {port} 現在是空閒的",
        "port_busy": "通訊埠 {port} 還有程式在回應，可能是手動啟動的 server.py 或其他程式。"
                     "結束它或者換一個通訊埠再安裝",
        "removed": "已刪除 {name}",
        "nothing": "沒有需要處理的內容",
        "copy": "複製程式檔案",
        "copied": "已解壓縮到 {path}",
        "settings": "寫入設定",
        "settings_ok": "通訊埠 {port}、語言 {language} 已寫入 {path}",
        "register": "新增開始功能表捷徑和移除項目",
        "shortcut_ok": "已建立 {path}",
        "entry_ok": "已登記到「設定 → 應用程式 → 已安裝的應用程式」",
        "firewall_step": "新增防火牆規則",
        "firewall_ok": "已允許 {path} 接受區域網路連線（網頁的 TCP {port} 和節點探索的 UDP {beacon}）",
        "skipped": "已略過",
        "service": "註冊並啟動背景服務",
        "task_ok": "排程工作 {name} 已註冊並啟動，{when}",
        "task_logon": "之後每次登入都會自動執行",
        "task_manual": "不會自動執行，要用時從開始功能表開啟程式，按「啟動」",
        "service_ok": "服務已在通訊埠 {port} 上回應",
        "service_dead": "等了 {seconds} 秒通訊埠仍沒有回應，原因請看 {path}",
        "done": "安裝完成，開啟 http://127.0.0.1:{port}",
        "failed": "安裝沒有完成：{error}",
        "installing": "正在安裝…",
        "upgrading": "正在升級…",
        "installed": "安裝完成",
        "upgraded": "升級完成",
        "failed_title": "安裝沒有完成",
        "back": "返回",
        "details_show": "顯示詳細資訊",
        "details_hide": "隱藏詳細資訊",
        "command_failed": "指令失敗（結束代碼 {code}）：{output}",
        "status_checking": "正在檢查…",
        "status_running": "執行中",
        "status_stopped": "未執行",
        "starting": "正在啟動…",
        "stopping": "正在停止…",
        "status_dead": "啟動後通訊埠仍沒有回應，原因請看 {path}",
        "start": "啟動",
        "stop": "停止",
        "data": "開啟資料目錄",
        "uninstall": "移除",
        "uninstall_title": "移除{app}",
        "uninstall_intro": "將停止背景服務，並刪除程式檔案、排程工作、防火牆規則、捷徑和移除項目。",
        "purge": "同時刪除設定和裝置資料",
        "u_stop": "停止背景服務",
        "still_up": "通訊埠 {port} 還有程式在回應，可能是手動啟動的 server.py，需要你自己結束它",
        "u_task": "刪除排程工作",
        "task_removed": "排程工作 {name} 已刪除",
        "u_firewall": "刪除防火牆規則",
        "rule_removed": "防火牆規則 {name} 已刪除",
        "u_entry": "刪除開始功能表捷徑和移除項目",
        "entry_removed": "已從「已安裝的應用程式」中移除",
        "u_data": "處理資料目錄",
        "data_kept": "資料保留在 {path}，重新安裝會接著用",
        "u_files": "刪除程式檔案",
        "files_later": "{path} 會在這個視窗關閉後刪除",
        "u_done": "移除完成",
        "u_failed": "移除沒有完成：{error}",
        "uninstalling": "正在移除…",
        "u_failed_title": "移除沒有完成",
        "elevate_denied": "移除需要系統管理員權限",
    },
    "en": {
        "app": "LYWSD02 Clock Sync",
        "setup_title": "{app} Setup",
        "version": "Version {version}",
        "language": "Language",
        "dir": "Install to",
        "browse": "Browse…",
        "port": "Web port",
        "autostart": "Run in the background when I sign in to Windows",
        "firewall": "Let other devices on the network connect (adds a firewall rule)",
        "data_note": "Settings and device data live in {path}. Upgrades keep them, and so "
                     "does uninstalling unless you ask otherwise.",
        "install": "Install",
        "upgrade": "Upgrade",
        "open": "Open web page",
        "close": "Close",
        "bad_port": "The port must be a number from 1 to 65535",
        "bad_dir": "The install location must be a full path with a drive letter, other "
                   "than the data directory {path}",
        "other_user": "This is running as {elevated} while {user} is signed in. Sign in with "
                      "the account that will use the program and approve the administrator "
                      "prompt from that account",
        "usage": "unknown argument: {arg}",
        "step": "[{n}/{total}] {what}",
        "halt": "Stopping the running service",
        "halt_ok": "port {port} is free",
        "port_busy": "something is still answering on port {port}, probably a server.py "
                     "started by hand or another program. Close it or pick another port",
        "removed": "removed {name}",
        "nothing": "nothing to do",
        "copy": "Copying the program",
        "copied": "unpacked into {path}",
        "settings": "Writing settings",
        "settings_ok": "port {port} and language {language} written to {path}",
        "register": "Adding the Start menu shortcut and the uninstall entry",
        "shortcut_ok": "created {path}",
        "entry_ok": "listed under Settings → Apps → Installed apps",
        "firewall_step": "Adding the firewall rule",
        "firewall_ok": "{path} may accept network connections (TCP {port} for the web page, "
                       "UDP {beacon} for node discovery)",
        "skipped": "skipped",
        "service": "Registering and starting the background service",
        "task_ok": "scheduled task {name} registered and started; {when}",
        "task_logon": "it starts at every sign-in from now on",
        "task_manual": "it does not start on its own, so open the program from the Start menu "
                       "and click Start when you need it",
        "service_ok": "the service answers on port {port}",
        "service_dead": "Nothing answered on the port after {seconds}s; see {path} for the reason",
        "done": "Done. Open http://127.0.0.1:{port}",
        "failed": "Setup did not finish: {error}",
        "installing": "Installing…",
        "upgrading": "Upgrading…",
        "installed": "Installed",
        "upgraded": "Upgraded",
        "failed_title": "Setup did not finish",
        "back": "Back",
        "details_show": "Show details",
        "details_hide": "Hide details",
        "command_failed": "command failed with exit code {code}: {output}",
        "status_checking": "Checking…",
        "status_running": "Running",
        "status_stopped": "Not running",
        "starting": "Starting…",
        "stopping": "Stopping…",
        "status_dead": "Started, but nothing answers on the port; see {path}",
        "start": "Start",
        "stop": "Stop",
        "data": "Open data folder",
        "uninstall": "Uninstall",
        "uninstall_title": "Uninstall {app}",
        "uninstall_intro": "This stops the background service and removes the program files, "
                           "the scheduled task, the firewall rule, the Start menu shortcut and "
                           "the uninstall entry.",
        "purge": "Also delete settings and device data",
        "u_stop": "Stopping the background service",
        "still_up": "something is still answering on port {port}, probably a server.py "
                    "started by hand; close it yourself",
        "u_task": "Removing the scheduled task",
        "task_removed": "scheduled task {name} removed",
        "u_firewall": "Removing the firewall rule",
        "rule_removed": "firewall rule {name} removed",
        "u_entry": "Removing the Start menu shortcut and the uninstall entry",
        "entry_removed": "removed from Installed apps",
        "u_data": "Handling the data directory",
        "data_kept": "Your data stays in {path}; installing again picks it up",
        "u_files": "Removing the program files",
        "files_later": "{path} is removed once this window closes",
        "u_done": "Uninstalled",
        "u_failed": "Uninstall did not finish: {error}",
        "uninstalling": "Uninstalling…",
        "u_failed_title": "Uninstall did not finish",
        "elevate_denied": "Uninstalling needs administrator rights",
    },
}


class Failure(Exception):
    pass


def translator(language):
    table = TEXT[language if language in TEXT else "en"]

    def say(key, **params):
        return table[key].format(app=table["app"], **params)
    return say


def system_language():
    code = kernel32.GetUserDefaultUILanguage()
    if code & 0x3FF != 0x04:
        return "en"
    # zh-CN, zh-SG and the neutral Simplified ids; every other Chinese is Traditional.
    return "zh-CN" if code in (0x0004, 0x0804, 0x1004, 0x7804) else "zh-TW"


def default_dir():
    local = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~\\AppData\\Local")
    return os.path.join(local, "Programs", APP_NAME)


def start_menu():
    return os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~\\AppData\\Roaming"),
                        "Microsoft", "Windows", "Start Menu", "Programs")


def same_path(first, second):
    return os.path.normcase(os.path.abspath(first)) == os.path.normcase(os.path.abspath(second))


def read_json(path):
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def data_config():
    return read_json(os.path.join(DATA_DIR, "config.json"))


def configured_port(config):
    try:
        return int(config.get("port", DEFAULT_PORT))
    except (TypeError, ValueError):
        return DEFAULT_PORT


def log_path():
    return os.path.join(DATA_DIR, "server.log")


def registered():
    """The values Setup stored under the uninstall key, or {} when not installed."""
    found = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY) as key:
            for name in ("InstallLocation", "ShortcutPath", "DisplayVersion"):
                try:
                    found[name] = winreg.QueryValueEx(key, name)[0]
                except OSError:
                    pass
    except OSError:
        pass
    return found


def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except OSError:
        return False


def other_user(say):
    """Over-the-shoulder elevation runs as a different account, whose profile,
    registry hive and scheduled task would all be the wrong ones."""
    wtsapi = ctypes.WinDLL("wtsapi32")
    buffer, size = wintypes.LPWSTR(), wintypes.DWORD()
    # WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSUserName
    if not wtsapi.WTSQuerySessionInformationW(None, wintypes.DWORD(0xFFFFFFFF), 5,
                                              ctypes.byref(buffer), ctypes.byref(size)):
        return None
    user = buffer.value or ""
    wtsapi.WTSFreeMemory(buffer)
    try:
        elevated = os.getlogin()
    except OSError:
        return None
    if user and elevated and user.lower() != elevated.lower():
        return say("other_user", elevated=elevated, user=user)
    return None


def run(command, log=None, encoding=None):
    """Run a console tool without flashing a window; returns (code, output)."""
    if log:
        log("  $ " + subprocess.list2cmdline(command))
    try:
        done = subprocess.run(command, capture_output=True, creationflags=CREATE_NO_WINDOW,
                              timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        return -1, str(error)
    output = (done.stdout + done.stderr).decode(encoding or OEM, "replace")
    return done.returncode, output.replace("\0", "").strip()


PS_WRAPPER = """$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
try {
__BODY__
} catch {
    [Console]::Out.WriteLine($_.Exception.Message)
    exit 1
}
"""


def ps_fill(template, **values):
    for name, value in values.items():
        if isinstance(value, bool):
            literal = "$true" if value else "$false"
        elif isinstance(value, int):
            literal = str(value)
        else:
            literal = "'" + str(value).replace("'", "''") + "'"
        template = template.replace(f"__{name.upper()}__", literal)
    return template


def ps_command(script):
    # An encoded command sidesteps every quoting rule between here and PowerShell.
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    return ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-EncodedCommand", encoded]


def powershell(body, log, what):
    log("  $ powershell " + what)
    return run(ps_command(PS_WRAPPER.replace("__BODY__", body)), encoding="utf-8")


def answering(port, timeout=2.0):
    try:
        OPENER.open(f"http://127.0.0.1:{port}/api/ping", timeout=timeout).close()
    except urllib.error.HTTPError:
        return True
    except (urllib.error.URLError, OSError):
        return False
    return True


def wait_until(check, timeout):
    deadline = time.monotonic() + timeout
    while not check():
        if time.monotonic() >= deadline:
            return False
        time.sleep(1.0)
    return True


def stop_service(ports, log):
    """End the task and any other copy of this program; True once the ports are quiet."""
    run(["schtasks", "/End", "/TN", TASK_NAME], log)
    # An open control window, or a "serve" started by hand, holds the files open.
    run(["taskkill", "/F", "/FI", f"IMAGENAME eq {EXE_NAME}", "/FI", f"PID ne {os.getpid()}"],
        log)
    return all(wait_until(lambda port=port: not answering(port), 10) for port in ports)


def _force(function, path, error):
    os.chmod(path, 0o700)
    function(path)


def remove(path, attempts=1):
    """Delete a file or a tree. A process that was just killed may hold it a moment longer."""
    for attempt in range(attempts):
        try:
            if os.path.isdir(path) and not os.path.islink(path):
                shutil.rmtree(path, onexc=_force)
            elif os.path.lexists(path):
                os.remove(path)
            return
        except OSError:
            if attempt == attempts - 1:
                raise
            time.sleep(1.0)


def tree_size(tops):
    total = 0
    for top in tops:
        if os.path.isfile(top):
            total += os.path.getsize(top)
        for root, _, files in os.walk(top):
            total += sum(os.path.getsize(os.path.join(root, name)) for name in files)
    return total


def has_logon_trigger():
    code, output = run(["schtasks", "/Query", "/TN", TASK_NAME, "/XML"])
    return None if code else "LogonTrigger" in output


# A logon task rather than a service: WinRT Bluetooth does not work in session 0,
# and that includes tasks registered without a password, which run as S4U.
TASK_SCRIPT = """$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute __EXE__ -Argument 'serve' -WorkingDirectory __DATA__
$principal = New-ScheduledTaskPrincipal -UserId $me -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero)
$task = @{ TaskName = __TASK__; Action = $action; Principal = $principal; Settings = $settings; Force = $true }
if (__AUTOSTART__) { $task.Trigger = New-ScheduledTaskTrigger -AtLogOn -User $me }
Register-ScheduledTask @task | Out-Null
# MultipleInstances is IgnoreNew, so a start that arrives while the previous
# instance is still winding down is dropped and nothing ends up running.
$deadline = (Get-Date).AddSeconds(20)
while ((Get-ScheduledTask -TaskName __TASK__).State -eq 'Running' -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
}
Start-ScheduledTask -TaskName __TASK__
"""

SHORTCUT_SCRIPT = """$link = (New-Object -ComObject WScript.Shell).CreateShortcut(__LINK__)
$link.TargetPath = __EXE__
$link.WorkingDirectory = __DIR__
$link.IconLocation = __ICON__
$link.Save()
"""

# Runs after this process exits, because the exe and _internal are in use until then.
REMOVAL_SCRIPT = """Wait-Process -Id __PID__ -ErrorAction SilentlyContinue
$paths = @(__EXE__, __INTERNAL__)
for ($i = 0; $i -lt 40 -and ($paths | Where-Object { Test-Path -LiteralPath $_ }); $i++) {
    Remove-Item -LiteralPath $paths -Recurse -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}
if (-not (Get-ChildItem -LiteralPath __DIR__ -Force -ErrorAction SilentlyContinue)) {
    Remove-Item -LiteralPath __DIR__ -Force -ErrorAction SilentlyContinue
}
"""


def checked(result, say):
    code, output = result
    if code:
        raise Failure(say("command_failed", code=code, output=output or "-"))
    return output


def install(payload, options, say, log, progress):
    """True when the service answers afterwards, False when it was installed but did not."""
    target, port, language = options["dir"], options["port"], options["language"]
    exe = os.path.join(target, EXE_NAME)
    steps = INSTALL_STEPS

    def step(name):
        progress(steps.index(name), len(steps))
        log("")
        log(say("step", n=steps.index(name) + 1, total=len(steps), what=say(name)))

    step("halt")
    if not stop_service({port, configured_port(data_config())}, log):
        raise Failure(say("port_busy", port=port))
    log("  " + say("halt_ok", port=port))

    step("copy")
    os.makedirs(target, exist_ok=True)
    for name in BUNDLE:
        remove(os.path.join(target, name), attempts=5)
    with zipfile.ZipFile(payload) as bundle:
        bundle.extractall(target)
    log("  " + say("copied", path=target))

    step("settings")
    os.makedirs(DATA_DIR, exist_ok=True)
    config_path = os.path.join(DATA_DIR, "config.json")
    config = read_json(config_path)
    config["port"], config["language"] = port, language
    store.save(config_path, config)
    log("  " + say("settings_ok", port=port, language=language, path=config_path))

    step("register")
    shortcut = os.path.join(start_menu(), say("app") + ".lnk")
    previous = registered().get("ShortcutPath")
    if previous and not same_path(previous, shortcut) and os.path.exists(previous):
        os.remove(previous)
    checked(powershell(ps_fill(SHORTCUT_SCRIPT, link=shortcut, exe=exe, dir=target,
                               icon=exe + ",0"), log, "CreateShortcut"), say)
    log("  " + say("shortcut_ok", path=shortcut))
    values = {
        "DisplayName": say("app"),
        "DisplayVersion": paths.version(),
        "Publisher": PUBLISHER,
        "DisplayIcon": exe,
        "InstallLocation": target,
        "UninstallString": f'"{exe}" uninstall',
        "QuietUninstallString": f'"{exe}" uninstall --silent',
        "ShortcutPath": shortcut,
        "NoModify": 1,
        "NoRepair": 1,
        "EstimatedSize": tree_size([os.path.join(target, name) for name in BUNDLE]) // 1024,
    }
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY, 0,
                            winreg.KEY_WRITE) as key:
        for name, value in values.items():
            kind = winreg.REG_DWORD if isinstance(value, int) else winreg.REG_SZ
            winreg.SetValueEx(key, name, 0, kind, value)
    log("  " + say("entry_ok"))

    step("firewall_step")
    if options["firewall"]:
        run(["netsh", "advfirewall", "firewall", "delete", "rule", f"name={RULE_NAME}"], log)
        checked(run(["netsh", "advfirewall", "firewall", "add", "rule", f"name={RULE_NAME}",
                     "dir=in", "action=allow", f"program={exe}", "enable=yes"], log), say)
        log("  " + say("firewall_ok", path=exe, port=port, beacon=BEACON_PORT))
    else:
        log("  " + say("skipped"))

    step("service")
    checked(powershell(ps_fill(TASK_SCRIPT, exe=exe, data=DATA_DIR, task=TASK_NAME,
                               autostart=options["autostart"]),
                       log, "Register-ScheduledTask " + TASK_NAME), say)
    log("  " + say("task_ok", name=TASK_NAME,
                   when=say("task_logon" if options["autostart"] else "task_manual")))
    healthy = wait_until(lambda: answering(port), 30)
    log("  " + (say("service_ok", port=port) if healthy
                else say("service_dead", seconds=30, path=log_path())))
    progress(len(steps), len(steps))
    log("")
    log(say("done", port=port))
    return healthy


def schedule_removal(target):
    script = ps_fill(REMOVAL_SCRIPT, pid=os.getpid(), exe=os.path.join(target, EXE_NAME),
                     internal=os.path.join(target, "_internal"), dir=target)
    flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
    for extra in (CREATE_BREAKAWAY_FROM_JOB, 0):
        try:
            subprocess.Popen(ps_command(script), cwd=tempfile.gettempdir(),
                             creationflags=flags | extra, close_fds=True)
            return
        except OSError:
            if not extra:
                raise


def uninstall(purge, say, log, progress):
    target = os.path.dirname(os.path.abspath(sys.executable))
    info = registered()
    steps = UNINSTALL_STEPS

    def step(name):
        progress(steps.index(name), len(steps))
        log("")
        log(say("step", n=steps.index(name) + 1, total=len(steps), what=say(name)))

    step("u_stop")
    port = configured_port(data_config())
    if stop_service({port}, log):
        log("  " + say("halt_ok", port=port))
    else:
        log("  " + say("still_up", port=port))

    step("u_task")
    code, _ = run(["schtasks", "/Delete", "/TN", TASK_NAME, "/F"], log)
    log("  " + (say("task_removed", name=TASK_NAME) if code == 0 else say("nothing")))

    step("u_firewall")
    code, _ = run(["netsh", "advfirewall", "firewall", "delete", "rule", f"name={RULE_NAME}"], log)
    log("  " + (say("rule_removed", name=RULE_NAME) if code == 0 else say("nothing")))

    step("u_entry")
    shortcut = info.get("ShortcutPath")
    if shortcut and os.path.exists(shortcut):
        os.remove(shortcut)
        log("  " + say("removed", name=shortcut))
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY)
    except FileNotFoundError:
        pass
    log("  " + say("entry_removed"))

    step("u_data")
    if purge and os.path.exists(DATA_DIR):
        remove(DATA_DIR, attempts=3)
        log("  " + say("removed", name=DATA_DIR))
    else:
        log("  " + say("data_kept", path=DATA_DIR))

    step("u_files")
    if os.path.exists(os.path.join(target, EXE_NAME)):
        schedule_removal(target)
        log("  " + say("files_later", path=target))
    else:
        log("  " + say("nothing"))
    progress(len(steps), len(steps))
    log("")
    log(say("u_done"))
    return True


def perform(job, say, failed_key, log, progress):
    """Run a job, logging any exception; returns (result, None) or (None, message)."""
    try:
        return job(say, log, progress), None
    except Failure as error:
        message = str(error)
    except Exception as error:
        log(traceback.format_exc().rstrip())
        message = str(error) or type(error).__name__
    log("")
    log(say(failed_key, error=message))
    return None, message


def run_silent(job, say, failed_key, name):
    with open(os.path.join(tempfile.gettempdir(), name), "w", encoding="utf-8") as handle:
        def log(text):
            handle.write(text + "\n")
            handle.flush()
        refused = other_user(say)
        if refused:
            log(say(failed_key, error=refused))
            return 1
        result, _ = perform(job, say, failed_key, log, lambda done, total: None)
    return 0 if result else 1


def message_box(text, title):
    ctypes.windll.user32.MessageBoxW(None, text, title, 0x10)


def parse(args, flags, options):
    found, rest = {}, list(args)
    while rest:
        arg = rest.pop(0)
        name, equals, value = arg.partition("=")
        if name in flags and not equals:
            found[name] = True
        elif name in options:
            found[name] = value if equals else (rest.pop(0) if rest else "")
        else:
            raise ValueError(arg)
    return found


def validate(fields, say):
    """(options, None) when the form makes sense, else (None, message)."""
    target = fields["dir"].strip().strip('"')
    if (not target or not os.path.isabs(target) or not os.path.splitdrive(target)[0]
            or same_path(target, DATA_DIR)):
        return None, say("bad_dir", path=DATA_DIR)
    try:
        port = int(str(fields["port"]).strip())
    except ValueError:
        port = 0
    if not 1 <= port <= 65535:
        return None, say("bad_port")
    return dict(fields, dir=os.path.abspath(target), port=port), None


def set_dpi_awareness():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass


def open_in_shell(target):
    # explorer.exe hands it to the signed-in shell, so the browser is not started elevated.
    subprocess.Popen(["explorer.exe", target], creationflags=CREATE_NO_WINDOW)


# The windows borrow the web page's palette and shapes. ttk can only stretch
# images, so the rounded pieces are painted here as PNGs at the screen's scale.
PALETTE = {
    "light": {
        "bg": "#f3f5f8", "panel": "#ffffff", "line": "#e1e7ee", "hair": "#eef2f6",
        "field": "#ffffff", "btn": "#ffffff", "btn_hover": "#f2f6fa", "btn_line": "#cbd5e1",
        "text": "#1d2733", "dim": "#64748b", "faint": "#94a3b8", "accent": "#1a73e8",
        "accent_hover": "#1765cc", "on_accent": "#ffffff", "ok": "#157f3c", "warn": "#a2620a",
        "bad": "#c23a20", "bad_hover": "#a9321b", "scroll": "#c6cfda",
        "scroll_hover": "#a9b5c3", "knob": "#ffffff", "tint": "#1d2733",
    },
    "dark": {
        "bg": "#0f1419", "panel": "#171d24", "line": "#262e38", "hair": "#1f262f",
        "field": "#0d1218", "btn": "#222b35", "btn_hover": "#2c3744", "btn_line": "#3a4654",
        "text": "#dfe6ee", "dim": "#7c8a99", "faint": "#5c6b7a", "accent": "#4da3ff",
        "accent_hover": "#67b2ff", "on_accent": "#07121d", "ok": "#3fbf7f", "warn": "#e8b23a",
        "bad": "#e8604c", "bad_hover": "#ee7a68", "scroll": "#3d4753",
        "scroll_hover": "#4e5a69", "knob": "#cfd8e2", "tint": "#ffffff",
    },
}
FAMILIES = {"zh-CN": ("Microsoft YaHei UI", "Microsoft YaHei"),
            "zh-TW": ("Microsoft JhengHei UI", "Microsoft JhengHei"),
            "en": ("Segoe UI",)}


def system_dark():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\"
                            "CurrentVersion\\Themes\\Personalize") as key:
            return winreg.QueryValueEx(key, "AppsUseLightTheme")[0] == 0
    except OSError:
        return False


def rgb(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5))


def mix(first, second, amount):
    return "#%02x%02x%02x" % tuple(round(a + (b - a) * amount)
                                   for a, b in zip(rgb(first), rgb(second)))


# Shapes are signed distance functions: negative inside, in pixels.
def box(cx, cy, half_width, half_height, radius):
    def distance(x, y):
        qx = abs(x - cx) - half_width + radius
        qy = abs(y - cy) - half_height + radius
        return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - radius
    return distance


def circle(cx, cy, radius):
    return lambda x, y: math.hypot(x - cx, y - cy) - radius


def ring(cx, cy, radius, width):
    return lambda x, y: abs(math.hypot(x - cx, y - cy) - radius) - width / 2


def stroke(points, width):
    pairs = list(zip(points, points[1:]))

    def distance(x, y):
        best = float("inf")
        for (ax, ay), (bx, by) in pairs:
            dx, dy = bx - ax, by - ay
            t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
            best = min(best, math.hypot(x - ax - t * dx, y - ay - t * dy))
        return best - width / 2
    return distance


def arc(cx, cy, radius, width, start, sweep):
    """Part of a ring with round ends; angles in radians, clockwise from twelve o'clock."""
    ends = [(cx + radius * math.sin(a), cy - radius * math.cos(a)) for a in (start, start + sweep)]

    def distance(x, y):
        if (math.atan2(x - cx, cy - y) - start) % (2 * math.pi) <= sweep:
            return abs(math.hypot(x - cx, y - cy) - radius) - width / 2
        return min(math.hypot(x - ex, y - ey) for ex, ey in ends) - width / 2
    return distance


def png(width, height, layers, opacity=1.0):
    """An anti-aliased RGBA PNG of (shape, colour, alpha) layers painted in order."""
    layers = [(shape, rgb(colour), alpha) for shape, colour, alpha in layers]
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            px, py = x + 0.5, y + 0.5
            red = green = blue = alpha = 0.0
            for shape, (r, g, b), strength in layers:
                cover = 0.5 - shape(px, py)
                if cover <= 0:
                    continue
                a = (cover if cover < 1 else 1.0) * strength
                red, green, blue = r * a + red * (1 - a), g * a + green * (1 - a), b * a + blue * (1 - a)
                alpha = a + alpha * (1 - a)
            if alpha:
                raw += bytes((round(red / alpha), round(green / alpha), round(blue / alpha),
                              round(alpha * opacity * 255)))
            else:
                raw += b"\0\0\0\0"

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6,
                                                               0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def framed(width, height, radius, fill, edge, line):
    """A rounded rectangle filling the image, with an edge line inside it."""
    w, h = width / 2, height / 2
    layers = [(box(w, h, w, h, radius), edge, 1.0)]
    if fill != edge:
        layers.append((box(w, h, w - line, h - line, max(radius - line, 0)), fill, 1.0))
    return layers


def resource(name):
    for path in (os.path.join(paths.RESOURCE_DIR, name),
                 os.path.join(paths.RESOURCE_DIR, "packaging", name)):
        if os.path.exists(path):
            return path
    return None


def icon_png(size):
    """The PNG in icon.ico that is closest to size pixels without being smaller."""
    path = resource("icon.ico")
    if not path:
        return None
    with open(path, "rb") as handle:
        data = handle.read()
    found = []
    for index in range(struct.unpack_from("<H", data, 4)[0]):
        width, _, _, _, _, _, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * index)
        blob = data[offset:offset + length]
        if blob.startswith(b"\x89PNG"):
            found.append((width or 256, blob))
    larger = [entry for entry in found if entry[0] >= size]
    return (min(larger) if larger else max(found))[1] if found else None


class Look:
    """Colours, fonts, images and the ttk theme of one window."""

    def __init__(self, root, language):
        self.root = root
        self.dark = system_dark()
        self.colors = PALETTE["dark" if self.dark else "light"]
        self.scale = root.winfo_fpixels("1i") / 96
        self.images = {}
        self.installed = set(tkfont.families(root))
        self.mono = next((name for name in ("Cascadia Mono", "Consolas")
                          if name in self.installed), "Courier New")
        self.fonts = {name: tkfont.Font(root, size=size, weight=weight)
                      for name, (size, weight) in {"body": (10, "normal"), "small": (9, "normal"),
                                                   "strong": (10, "bold"), "title": (15, "bold"),
                                                   "mono": (9, "normal")}.items()}
        self.set_language(language)
        self.theme()

    def px(self, value):
        return max(1, round(value * self.scale))

    def set_language(self, language):
        family = next((name for name in FAMILIES[language] if name in self.installed),
                      tkfont.nametofont("TkDefaultFont").actual("family"))
        for name, font in self.fonts.items():
            font.configure(family=self.mono if name == "mono" else family)

    def image(self, name, width, height, layers, opacity=1.0):
        self.images[name] = tk.PhotoImage(
            master=self.root, format="png",
            data=base64.b64encode(png(width, height, layers, opacity)).decode("ascii"))
        return self.images[name]

    def logo(self, size):
        if "logo" not in self.images:
            blob = icon_png(size)
            self.images["logo"] = blob and tk.PhotoImage(
                master=self.root, format="png", data=base64.b64encode(blob).decode("ascii"))
        return self.images["logo"]

    def theme(self):
        c, px = self.colors, self.px
        style = ttk.Style(self.root)
        if "lywsd02" not in style.theme_names():
            style.theme_create("lywsd02", parent="clam")
        style.theme_use("lywsd02")
        style.configure(".", background=c["bg"], foreground=c["text"], font=self.fonts["body"],
                        borderwidth=0, focuscolor=c["accent"])
        line = px(1)
        # ttk tiles the middle of a stretched image, so it is kept wide to keep drawing cheap.
        stretch = px(24)

        # Buttons, the same three kinds as the web page plus a red one.
        radius = px(8)
        size = 2 * radius + 2 + stretch
        kinds = {
            "TButton": ("Plain", c["btn"], c["line"], c["btn_hover"], c["btn_line"], c["text"]),
            "Primary.TButton": ("Primary", c["accent"], c["accent"], c["accent_hover"],
                                c["accent_hover"], c["on_accent"]),
            "Danger.TButton": ("Danger", c["bad"], c["bad"], c["bad_hover"], c["bad_hover"],
                               c["on_accent"]),
        }
        for name, (element, fill, edge, hover, hover_edge, fore) in kinds.items():
            element += ".border"
            pressed = mix(hover, c["text"], 0.08)
            images = [self.image(element + state, size, size, framed(size, size, radius, *colours,
                                                                     line), opacity)
                      for state, colours, opacity in (("", (fill, edge), 1.0),
                                                      ("hover", (hover, hover_edge), 1.0),
                                                      ("down", (pressed, hover_edge), 1.0),
                                                      ("off", (fill, edge), 0.45))]
            style.element_create(element, "image", images[0], ("disabled", images[3]),
                                 ("pressed", images[2]), ("active", images[1]),
                                 border=radius + 1, width=size - stretch,
                                 height=size - stretch, sticky="nsew")
            style.layout(name, [(element, {"sticky": "nsew", "children": [
                ("Button.padding", {"sticky": "nsew", "children": [
                    ("Button.label", {"sticky": "nsew"})]})]})])
            style.configure(name, padding=(px(16), px(6)), foreground=fore, anchor="center")
            style.map(name, foreground=[("disabled", mix(fore, c["panel"], 0.5))])
            style.configure("Card." + name, background=c["panel"])
        style.layout("Link.TButton", [("Button.padding", {"sticky": "nsew", "children": [
            ("Button.label", {"sticky": "nsew"})]})])
        style.configure("Link.TButton", padding=(0, px(2)), foreground=c["dim"])
        style.map("Link.TButton", foreground=[("disabled", c["faint"]), ("active", c["accent"])])

        # Text fields.
        radius = px(7)
        size = 2 * radius + 2 + stretch
        images = [self.image("field" + state, size, size,
                             framed(size, size, radius, c["field"], edge, line), opacity)
                  for state, edge, opacity in (("", c["line"], 1.0), ("hover", c["btn_line"], 1.0),
                                               ("focus", c["accent"], 1.0), ("off", c["line"], 0.4))]
        style.element_create("Field.field", "image", images[0], ("disabled", images[3]),
                             ("focus", images[2]), ("hover", images[1]),
                             border=radius + 1, width=size - stretch, height=size - stretch,
                             sticky="nsew")
        style.layout("TEntry", [("Field.field", {"sticky": "nsew", "children": [
            ("Entry.padding", {"sticky": "nsew", "children": [
                ("Entry.textarea", {"sticky": "nsew"})]})]})])
        style.configure("TEntry", padding=(px(9), px(5)), background=c["panel"],
                        fieldbackground=c["field"], foreground=c["text"], insertcolor=c["text"],
                        selectbackground=c["accent"], selectforeground=c["on_accent"])
        style.map("TEntry", foreground=[("disabled", c["faint"])])

        # Cards.
        radius = px(12)
        size = 2 * radius + 2 + stretch
        self.image("card", size, size, framed(size, size, radius, c["panel"], c["line"], line))
        style.element_create("Card.border", "image", self.images["card"], border=radius + 1,
                             width=size - stretch, height=size - stretch, sticky="nsew")
        style.layout("Card.TFrame", [("Card.border", {"sticky": "nsew"})])

        # The language picker: a row of segments in a tinted trough.
        trough = mix(c["panel"], c["tint"], 0.06)
        radius = px(8)
        size = 2 * radius + 2 + stretch
        self.image("trough", size, size, framed(size, size, radius, trough, trough, line))
        style.element_create("Seg.trough", "image", self.images["trough"], border=radius + 1,
                             width=size - stretch, height=size - stretch, sticky="nsew")
        style.layout("Seg.TFrame", [("Seg.trough", {"sticky": "nsew"})])
        style.configure("Seg.TFrame", background=c["panel"])
        radius = px(6)
        size = 2 * radius + 2 + stretch
        chosen = self.image("seg", size, size,
                            framed(size, size, radius, c["btn"], c["line"], line))
        hot = self.image("seg_hover", size, size,
                         framed(size, size, radius, mix(trough, c["tint"], 0.05),
                                mix(trough, c["tint"], 0.05), line))
        blank = self.image("seg_blank", size, size, [])
        style.element_create("Seg.border", "image", blank, ("selected", chosen), ("active", hot),
                             border=radius + 1, width=size - stretch, height=size - stretch,
                             sticky="nsew")
        style.layout("Seg.TRadiobutton", [("Seg.border", {"sticky": "nsew", "children": [
            ("Radiobutton.padding", {"sticky": "nsew", "children": [
                ("Radiobutton.label", {"sticky": "nsew"})]})]})])
        style.configure("Seg.TRadiobutton", padding=(px(12), px(3)), background=trough,
                        foreground=c["dim"], anchor="center")
        style.map("Seg.TRadiobutton", foreground=[("disabled", c["faint"]),
                                                  ("selected", c["text"]), ("active", c["text"])])

        # Progress bar.
        height = px(6)
        for name, colour in (("bar_trough", c["line"]), ("bar_fill", c["accent"])):
            self.image(name, 2 * height + stretch, height,
                       [(box(height + stretch / 2, height / 2, height + stretch / 2, height / 2,
                             height / 2), colour, 1.0)])
        style.element_create("Horizontal.Progressbar.trough", "image", self.images["bar_trough"],
                             border=(height, 0), width=2 * height, sticky="ew")
        style.element_create("Horizontal.Progressbar.pbar", "image", self.images["bar_fill"],
                             border=(height, 0), width=2 * height, sticky="nsew")
        style.configure("Horizontal.TProgressbar", background=c["bg"])

        # Scrollbar: a thin thumb, no arrows, like the page's.
        width = px(11)
        thin = px(5)
        for name, colour in (("thumb", c["scroll"]), ("thumb_hover", c["scroll_hover"])):
            self.image(name, width, 2 * width + stretch,
                       [(stroke([(width / 2, width / 2), (width / 2, 1.5 * width + stretch)], thin),
                         colour, 1.0)])
        self.image("scroll_trough", width, width, [])
        style.element_create("Vertical.Scrollbar.trough", "image", self.images["scroll_trough"],
                             border=0, sticky="ns")
        style.element_create("Vertical.Scrollbar.thumb", "image", self.images["thumb"],
                             ("pressed", self.images["thumb_hover"]),
                             ("active", self.images["thumb_hover"]),
                             border=(0, width), height=2 * width, sticky="nsew")
        style.layout("Vertical.TScrollbar", [("Vertical.Scrollbar.trough", {
            "sticky": "ns", "children": [("Vertical.Scrollbar.thumb",
                                          {"expand": "1", "sticky": "nsew"})]})])
        style.configure("Vertical.TScrollbar", background=c["panel"], troughcolor=c["panel"])

        # Switches.
        width, height, knob, gap = px(40), px(22), px(16) / 2, px(3)
        for name, on, opacity in (("switch_on", True, 1.0), ("switch_off", False, 1.0),
                                  ("switch_on_off", True, 0.4), ("switch_off_off", False, 0.4)):
            x = width - gap - knob if on else gap + knob
            self.image(name, width, height, [
                (box(width / 2, height / 2, width / 2, height / 2, height / 2),
                 c["ok"] if on else c["scroll"], 1.0),
                (circle(x, height / 2 + px(0.5), knob + px(0.5)), "#000000", 0.18),
                (circle(x, height / 2, knob), c["knob"], 1.0)], opacity)

        # Step marks, with a gap to the text drawn in.
        mark, gap = px(18), px(10)
        middle = mark / 2
        marks = {
            "step_pending": [(ring(middle, middle, middle - px(2), px(1.5)), c["faint"], 1.0)],
            "step_done": [(circle(middle, middle, middle - px(1)), c["ok"], 1.0),
                          (stroke([(mark * 0.29, mark * 0.52), (mark * 0.44, mark * 0.67),
                                   (mark * 0.71, mark * 0.37)], px(2)), c["on_accent"], 1.0)],
            "step_failed": [(circle(middle, middle, middle - px(1)), c["bad"], 1.0),
                            (stroke([(mark * 0.35, mark * 0.35), (mark * 0.65, mark * 0.65)],
                                    px(2)), c["on_accent"], 1.0),
                            (stroke([(mark * 0.65, mark * 0.35), (mark * 0.35, mark * 0.65)],
                                    px(2)), c["on_accent"], 1.0)],
        }
        for frame in range(12):
            marks[f"spin{frame}"] = [
                (ring(middle, middle, middle - px(2), px(2)), c["line"], 1.0),
                (arc(middle, middle, middle - px(2), px(2), frame * math.pi / 6, 1.7),
                 c["accent"], 1.0)]
        for name, layers in marks.items():
            self.image(name, mark + gap, mark, layers)

        size = px(10)
        for name, colour in (("dot_ok", c["ok"]), ("dot_off", c["faint"]),
                             ("dot_busy", c["accent"])):
            self.image(name, size, size, [(circle(size / 2, size / 2, size / 2), colour, 1.0)])


class Switch(tk.Label):
    """An on/off switch drawn like the web page's, bound to a BooleanVar."""

    def __init__(self, parent, look, variable):
        c = look.colors
        super().__init__(parent, bd=0, bg=c["panel"], cursor="hand2", takefocus=1,
                         highlightthickness=look.px(2), highlightbackground=c["panel"],
                         highlightcolor=c["accent"])
        self.look, self.variable, self.enabled = look, variable, True
        trace = variable.trace_add("write", lambda *_: self.paint())
        self.bind("<Destroy>", lambda _: variable.trace_remove("write", trace))
        self.bind("<Button-1>", self.toggle)
        self.bind("<space>", self.toggle)
        self.paint()

    def toggle(self, _event=None):
        if self.enabled:
            self.variable.set(not self.variable.get())

    def set_enabled(self, enabled):
        self.enabled = enabled
        self.configure(cursor="hand2" if enabled else "")
        self.paint()

    def paint(self):
        name = "switch_" + ("on" if self.variable.get() else "off") + ("" if self.enabled else "_off")
        self.configure(image=self.look.images[name])


def dark_title_bar(root):
    try:
        hwnd = ctypes.windll.user32.GetParent(root.winfo_id())
        value = ctypes.c_int(1)
        # 20 since Windows 10 20H1, 19 before it.
        for attribute in (20, 19):
            if not ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attribute, ctypes.byref(value),
                                                              ctypes.sizeof(value)):
                break
        # SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER repaints the caption.
        ctypes.windll.user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x27)
    except (AttributeError, OSError):
        pass


class Window:
    """A Tk root with the header, body and footer every window here shares, plus a
    queue that worker threads use to reach the widgets."""

    def __init__(self, language):
        set_dpi_awareness()
        self.language = language
        self.root = tk.Tk()
        self.root.withdraw()
        self.look = Look(self.root, language)
        c, px = self.colors, self.px = self.look.colors, self.look.px
        self.root.configure(bg=c["bg"])
        icon = resource("icon.ico")
        if icon:
            self.root.iconbitmap(default=icon)
        self.events = queue.Queue()
        self.busy = False
        self.rows = []
        self.current = 0
        self.spin = 0
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        outer = tk.Frame(self.root, bg=c["bg"], padx=px(24), pady=px(20))
        outer.pack(fill="both", expand=True)
        head = tk.Frame(outer, bg=c["bg"])
        head.pack(fill="x")
        logo = self.look.logo(px(44))
        if logo:
            tk.Label(head, image=logo, bg=c["bg"], bd=0).pack(side="left", padx=(0, px(14)))
        names = tk.Frame(head, bg=c["bg"])
        names.pack(side="left", fill="x")
        self.heading = self.label(names, font="title", bg="bg")
        self.heading.pack(anchor="w")
        self.subheading = self.label(names, font="small", color="dim", bg="bg")
        self.subheading.pack(anchor="w")
        self.footer = tk.Frame(outer, bg=c["bg"])
        self.footer.pack(side="bottom", fill="x", pady=(px(18), 0))
        self.body = tk.Frame(outer, bg=c["bg"])
        self.body.pack(fill="both", expand=True, pady=(px(20), 0))
        self.root.after(100, self.drain)

    def say(self, key, **params):
        return translator(self.language)(key, **params)

    def label(self, parent, text="", font="body", color="text", bg="panel", fit=False,
              **options):
        """A plain text label; fit=True wraps it at whatever width it is given."""
        label = tk.Label(parent, text=text, font=self.look.fonts[font], fg=self.colors[color],
                         bg=self.colors[bg], bd=0, padx=0, pady=0, anchor="w", justify="left",
                         **options)
        if fit:
            label.bind("<Configure>", lambda event: label.configure(wraplength=event.width))
        return label

    def card(self, parent, padding=None):
        return ttk.Frame(parent, style="Card.TFrame",
                         padding=padding or (self.px(18), self.px(16)))

    def separator(self, parent):
        return tk.Frame(parent, height=self.px(1), bg=self.colors["hair"])

    def clear(self, frame):
        for child in frame.winfo_children():
            child.destroy()

    def actions(self, buttons, link=None):
        """Fill the footer: (key, command, style) buttons on the right, an optional link left."""
        self.clear(self.footer)
        made = {}
        for key, command, style in reversed(buttons):
            made[key] = ttk.Button(self.footer, text=self.say(key), command=command, style=style,
                                   cursor="hand2", takefocus=1)
            made[key].pack(side="right", padx=(self.px(8), 0))
        if link:
            made["link"] = ttk.Button(self.footer, text=self.say(link[0]), command=link[1],
                                      style="Link.TButton", cursor="hand2")
            made["link"].pack(side="left")
        return made

    def show(self):
        root = self.root
        root.update_idletasks()
        width, height = root.winfo_reqwidth(), root.winfo_reqheight()
        root.geometry(f"+{(root.winfo_screenwidth() - width) // 2}"
                      f"+{max((root.winfo_screenheight() - height) // 3, 0)}")
        root.attributes("-alpha", 0.0)
        root.deiconify()
        root.update_idletasks()
        if self.look.dark:
            dark_title_bar(root)
        root.attributes("-alpha", 1.0)
        # Later pages may grow the window, never shrink it below the first one.
        root.minsize(width, height)
        root.lift()
        root.focus_force()

    def progress_page(self, steps, headline):
        """Swap the body for a step list, a progress bar and a hidden log."""
        c, px = self.colors, self.px
        self.clear(self.body)
        self.headline = self.label(self.body, self.say(headline), font="strong", bg="bg")
        self.headline.pack(fill="x")
        self.subline = self.label(self.body, font="small", color="dim", bg="bg", fit=True,
                                  wraplength=px(520))
        self.subline.pack(fill="x", pady=(px(2), 0))
        self.bar = ttk.Progressbar(self.body, maximum=len(steps), mode="determinate")
        self.bar.pack(fill="x", pady=(px(12), px(14)))
        card = self.card(self.body)
        card.pack(fill="x")
        self.rows = []
        for index, step in enumerate(steps):
            row = self.label(card, self.say(step), color="dim", compound="left",
                             image=self.look.images["step_pending"])
            row.pack(fill="x", pady=(px(9) if index else 0, 0))
            self.rows.append(row)
        self.current = 0
        self.log_card = self.card(self.body, padding=(px(14), px(10), px(6), px(10)))
        self.text = tk.Text(self.log_card, height=9, width=1, wrap="word", state="disabled",
                            bd=0, highlightthickness=0, padx=0, pady=0, bg=c["panel"], fg=c["dim"],
                            font=self.look.fonts["mono"], selectbackground=c["accent"],
                            selectforeground=c["on_accent"], insertbackground=c["text"])
        scroll = ttk.Scrollbar(self.log_card, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y", padx=(px(6), 0))
        self.text.pack(side="left", fill="both", expand=True)
        self.details_open = False

    def toggle_details(self, show=None):
        self.details_open = (not self.details_open) if show is None else show
        if self.details_open:
            self.log_card.pack(fill="both", expand=True, pady=(self.px(14), 0))
            self.text.see("end")
        else:
            self.log_card.pack_forget()
        if "link" in self.buttons:
            self.buttons["link"].configure(
                text=self.say("details_hide" if self.details_open else "details_show"))

    def details_link(self):
        return ("details_hide" if self.details_open else "details_show", self.toggle_details)

    def advance(self, done, total):
        self.bar["value"] = done
        self.current = done
        images, c = self.look.images, self.colors
        for index, row in enumerate(self.rows):
            if index < done:
                row.configure(image=images["step_done"], fg=c["text"])
            elif index == done:
                row.configure(image=images[f"spin{self.spin % 12}"], fg=c["text"])
            else:
                row.configure(image=images["step_pending"], fg=c["dim"])

    def conclude(self, title, detail="", failed=False, colour="dim"):
        c = self.colors
        self.headline.configure(text=title, fg=c["bad"] if failed else c["text"])
        self.subline.configure(text=detail, fg=c["bad"] if failed else c[colour])
        if failed and self.current < len(self.rows):
            self.rows[self.current].configure(image=self.look.images["step_failed"], fg=c["bad"])

    def write(self, line):
        self.text.configure(state="normal")
        self.text.insert("end", line + "\n")
        self.text.see("end")
        self.text.configure(state="disabled")

    def start(self, job, failed_key):
        self.busy = True
        say = translator(self.language)

        def body():
            outcome = perform(job, say, failed_key,
                              lambda line: self.events.put(("log", line)),
                              lambda done, total: self.events.put(("progress", (done, total))))
            self.events.put(("done", outcome))
        threading.Thread(target=body, daemon=True).start()

    def drain(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.write(value)
                elif kind == "progress":
                    self.advance(*value)
                elif kind == "done":
                    self.busy = False
                    self.finished(*value)
                else:
                    self.handle(kind, value)
        except queue.Empty:
            pass
        if self.busy and self.current < len(self.rows):
            self.spin += 1
            self.rows[self.current].configure(image=self.look.images[f"spin{self.spin % 12}"])
        # The spinner needs the fast tick; a window that is only showing a status does not.
        self.root.after(90 if self.busy else 250, self.drain)

    def handle(self, kind, value):
        pass

    def finished(self, result, message):
        pass

    def close(self):
        if not self.busy:
            self.root.destroy()

    def run(self):
        self.root.mainloop()


class SetupWindow(Window):
    def __init__(self, payload, options, upgrade):
        super().__init__(options["language"])
        self.payload, self.upgrade, self.port = payload, upgrade, options["port"]
        self.choice = tk.StringVar(value=self.language)
        self.dir = tk.StringVar(value=options["dir"])
        self.port_var = tk.StringVar(value=str(options["port"]))
        self.autostart = tk.BooleanVar(value=options["autostart"])
        self.firewall = tk.BooleanVar(value=options["firewall"])
        self.form()
        self.show()

    def form(self):
        px = self.px
        self.clear(self.body)
        self.rows = []
        card = self.card(self.body)
        card.pack(fill="x")
        card.columnconfigure(0, weight=1)

        self.language_label = self.label(card, color="dim")
        self.language_label.grid(row=0, column=0, sticky="w")
        picker = ttk.Frame(card, style="Seg.TFrame", padding=px(3))
        picker.grid(row=0, column=1, sticky="e")
        for code, name in LANGUAGE_NAMES.items():
            ttk.Radiobutton(picker, text=name, value=code, variable=self.choice,
                            style="Seg.TRadiobutton", command=self.pick_language,
                            cursor="hand2").pack(side="left")

        self.dir_label = self.label(card, color="dim")
        self.dir_label.grid(row=1, column=0, columnspan=2, sticky="w", pady=(px(16), px(6)))
        line = tk.Frame(card, bg=self.colors["panel"])
        line.grid(row=2, column=0, columnspan=2, sticky="ew")
        self.dir_entry = ttk.Entry(line, textvariable=self.dir, width=46,
                                   font=self.look.fonts["body"])
        self.dir_entry.pack(side="left", fill="x", expand=True)
        self.browse_button = ttk.Button(line, command=self.browse, style="Card.TButton",
                                        cursor="hand2")
        self.browse_button.pack(side="left", padx=(px(8), 0))

        self.port_label = self.label(card, color="dim")
        self.port_label.grid(row=3, column=0, sticky="w", pady=(px(16), 0))
        self.port_entry = ttk.Entry(card, textvariable=self.port_var, width=7, justify="center",
                                    font=self.look.fonts["body"])
        self.port_entry.grid(row=3, column=1, sticky="e", pady=(px(16), 0))

        self.separator(card).grid(row=4, column=0, columnspan=2, sticky="ew", pady=px(16))
        self.switches = []
        for row, variable in ((5, self.autostart), (6, self.firewall)):
            text = self.label(card, wraplength=px(430), cursor="hand2")
            text.grid(row=row, column=0, sticky="w", pady=(px(10) if row == 6 else 0, 0))
            switch = Switch(card, self.look, variable)
            switch.grid(row=row, column=1, sticky="e", pady=(px(10) if row == 6 else 0, 0))
            text.bind("<Button-1>", switch.toggle)
            self.switches.append((text, switch))

        self.note = self.label(self.body, font="small", color="dim", bg="bg", fit=True,
                               wraplength=px(540))
        self.note.pack(fill="x", pady=(px(12), 0))
        self.error = self.label(self.body, color="bad", bg="bg", fit=True,
                                wraplength=px(540))
        self.buttons = self.actions([("close", self.close, "TButton"),
                                     ("upgrade" if self.upgrade else "install", self.install,
                                      "Primary.TButton")])
        self.render()

    def render(self):
        say = self.say
        self.root.title(say("setup_title"))
        self.heading.configure(text=say("app"))
        self.subheading.configure(text=say("version", version=paths.version()))
        self.language_label.configure(text=say("language"))
        self.dir_label.configure(text=say("dir"))
        self.browse_button.configure(text=say("browse"))
        self.port_label.configure(text=say("port"))
        for (text, _), key in zip(self.switches, ("autostart", "firewall")):
            text.configure(text=say(key))
        self.note.configure(text=say("data_note", path=DATA_DIR))
        for key, button in self.buttons.items():
            button.configure(text=say(key))

    def pick_language(self):
        self.language = self.choice.get()
        self.look.set_language(self.language)
        self.render()

    def browse(self):
        start = self.dir.get().strip()
        chosen = filedialog.askdirectory(
            parent=self.root, mustexist=False,
            initialdir=os.path.dirname(start) if start else os.path.dirname(default_dir()))
        if chosen:
            chosen = os.path.normpath(chosen)
            if os.path.basename(chosen).lower() != APP_NAME.lower():
                chosen = os.path.join(chosen, APP_NAME)
            self.dir.set(chosen)

    def install(self):
        refused = other_user(self.say)
        options, problem = validate({"dir": self.dir.get(), "port": self.port_var.get(),
                                     "language": self.language,
                                     "autostart": self.autostart.get(),
                                     "firewall": self.firewall.get()}, self.say)
        if refused or problem:
            self.error.configure(text=refused or problem)
            self.error.pack(fill="x", pady=(self.px(8), 0))
            return
        self.port = options["port"]
        self.progress_page(INSTALL_STEPS, "upgrading" if self.upgrade else "installing")
        self.buttons = self.actions([("close", self.close, "TButton")], self.details_link())
        self.buttons["close"].state(["disabled"])
        self.start(lambda say, log, progress: install(self.payload, options, say, log,
                                                      progress), "failed")

    def finished(self, result, message):
        if result is None:
            self.conclude(self.say("failed_title"), message, failed=True)
            self.buttons = self.actions([("back", self.form, "TButton"),
                                         ("close", self.close, "Primary.TButton")],
                                        self.details_link())
            self.toggle_details(True)
            return
        self.advance(len(self.rows), len(self.rows))
        url = f"http://127.0.0.1:{self.port}"
        if result:
            self.conclude(self.say("upgraded" if self.upgrade else "installed"), url)
        else:
            self.conclude(self.say("upgraded" if self.upgrade else "installed"),
                          self.say("service_dead", seconds=30, path=log_path()), colour="warn")
        self.buttons = self.actions([("close", self.close, "TButton"),
                                     ("open", lambda: open_in_shell(url + "/"),
                                      "Primary.TButton")], self.details_link())
        self.buttons["open"].focus_set()


class ManagerWindow(Window):
    def __init__(self, language):
        super().__init__(language)
        px = self.px
        self.root.title(self.say("app"))
        self.root.resizable(False, False)
        self.heading.configure(text=self.say("app"))
        self.subheading.configure(text=self.say("version", version=paths.version()))
        card = self.card(self.body)
        card.pack(fill="x")
        card.columnconfigure(1, weight=1, minsize=px(250))
        self.dot = tk.Label(card, image=self.look.images["dot_off"], bg=self.colors["panel"], bd=0)
        self.dot.grid(row=0, column=0, sticky="w", padx=(0, px(10)))
        self.state_label = self.label(card, self.say("status_checking"), font="strong")
        self.state_label.grid(row=0, column=1, sticky="w")
        self.detail = self.label(card, font="small", color="dim", wraplength=px(260))
        self.detail.grid(row=1, column=1, sticky="w", pady=(px(2), 0))
        buttons = tk.Frame(card, bg=self.colors["panel"])
        buttons.grid(row=0, column=2, rowspan=2, sticky="e", padx=(px(16), 0))
        self.toggle_button = ttk.Button(buttons, text=self.say("start"), style="Card.TButton",
                                        command=self.toggle_service, cursor="hand2")
        self.toggle_button.pack(side="left")
        self.open_button = ttk.Button(buttons, text=self.say("open"),
                                      style="Card.Primary.TButton", command=self.open_page,
                                      cursor="hand2")
        self.open_button.pack(side="left", padx=(px(8), 0))
        for widget in (self.toggle_button, self.open_button):
            widget.state(["disabled"])

        for key, command in (("data", lambda: open_in_shell(DATA_DIR)),
                             ("uninstall", self.uninstall)):
            ttk.Button(self.footer, text=self.say(key), command=command, style="Link.TButton",
                       cursor="hand2").pack(side="left", padx=(0, px(20)))
        self.running = None
        self.note = None
        threading.Thread(target=self.watch, daemon=True).start()
        self.show()

    def port(self):
        return configured_port(data_config())

    def watch(self):
        while True:
            self.events.put(("status", answering(self.port(), 1.5)))
            time.sleep(2.0)

    def handle(self, kind, value):
        if kind == "note":
            self.note = value
        elif kind in ("status", "idle"):
            self.running = value
            if kind == "idle":
                self.busy = False
        if self.running is None or self.busy:
            return
        images = self.look.images
        if self.running:
            self.dot.configure(image=images["dot_ok"])
            self.state_label.configure(text=self.say("status_running"))
            self.detail.configure(text=f"http://127.0.0.1:{self.port()}", fg=self.colors["dim"])
        else:
            self.dot.configure(image=images["dot_off"])
            self.state_label.configure(text=self.say("status_stopped"))
            self.detail.configure(text=self.note or "",
                                  fg=self.colors["warn" if self.note else "dim"])
        self.toggle_button.configure(text=self.say("stop" if self.running else "start"))
        self.toggle_button.state(["!disabled"])
        self.open_button.state(["!disabled" if self.running else "disabled"])

    def toggle_service(self):
        stopping = bool(self.running)
        self.busy = True
        self.note = None
        self.dot.configure(image=self.look.images["dot_busy"])
        self.state_label.configure(text=self.say("stopping" if stopping else "starting"))
        self.toggle_button.state(["disabled"])
        self.open_button.state(["disabled"])

        def body():
            try:
                if stopping:
                    stop_service({self.port()}, None)
                else:
                    self.start_service()
            finally:
                self.events.put(("idle", answering(self.port(), 1.5)))
        threading.Thread(target=body, daemon=True).start()

    def start_service(self):
        port = self.port()
        code, _ = run(["schtasks", "/Run", "/TN", TASK_NAME])
        if code:
            # Without a task (or the right to run it) start it directly; it outlives this window.
            subprocess.Popen([sys.executable, "serve"], cwd=DATA_DIR,
                             creationflags=CREATE_NEW_PROCESS_GROUP, close_fds=True)
        if not wait_until(lambda: answering(port), 25):
            self.events.put(("note", self.say("status_dead", path=log_path())))

    def open_page(self):
        open_in_shell(f"http://127.0.0.1:{self.port()}/")

    def uninstall(self):
        subprocess.Popen([sys.executable, "uninstall"], close_fds=True)
        self.root.destroy()

    def close(self):
        self.root.destroy()


class UninstallWindow(Window):
    def __init__(self, language):
        super().__init__(language)
        self.root.title(self.say("uninstall_title"))
        self.heading.configure(text=self.say("uninstall_title"))
        self.subheading.configure(text=self.say("version", version=paths.version()))
        self.purge = tk.BooleanVar(value=False)
        self.form()
        self.show()

    def form(self):
        px = self.px
        self.clear(self.body)
        self.rows = []
        card = self.card(self.body)
        card.pack(fill="x")
        card.columnconfigure(0, weight=1)
        self.label(card, self.say("uninstall_intro"), fit=True, wraplength=px(500)).grid(
            row=0, column=0, columnspan=2, sticky="ew")
        self.separator(card).grid(row=1, column=0, columnspan=2, sticky="ew", pady=px(16))
        text = self.label(card, self.say("purge"), cursor="hand2")
        text.grid(row=2, column=0, sticky="w")
        where = self.label(card, DATA_DIR, font="small", color="dim", cursor="hand2")
        where.grid(row=3, column=0, sticky="w", pady=(px(2), 0))
        switch = Switch(card, self.look, self.purge)
        switch.grid(row=2, column=1, rowspan=2, sticky="e", padx=(px(16), 0))
        for widget in (text, where):
            widget.bind("<Button-1>", switch.toggle)
        self.error = self.label(self.body, color="bad", bg="bg", fit=True,
                                wraplength=px(520))
        self.buttons = self.actions([("close", self.close, "TButton"),
                                     ("uninstall", self.go, "Danger.TButton")])

    def go(self):
        refused = other_user(self.say)
        if refused:
            self.error.configure(text=refused)
            self.error.pack(fill="x", pady=(self.px(8), 0))
            return
        purge = self.purge.get()
        self.progress_page(UNINSTALL_STEPS, "uninstalling")
        self.buttons = self.actions([("close", self.close, "TButton")], self.details_link())
        self.buttons["close"].state(["disabled"])
        self.start(lambda say, log, progress: uninstall(purge, say, log, progress), "u_failed")

    def finished(self, result, message):
        if result is None:
            self.conclude(self.say("u_failed_title"), message, failed=True)
            self.buttons = self.actions([("back", self.form, "TButton"),
                                         ("close", self.close, "Primary.TButton")],
                                        self.details_link())
            self.toggle_details(True)
            return
        self.advance(len(self.rows), len(self.rows))
        self.conclude(self.say("u_done"),
                      "" if self.purge.get() else self.say("data_kept", path=DATA_DIR))
        self.buttons = self.actions([("close", self.close, "Primary.TButton")],
                                    self.details_link())
        self.buttons["close"].focus_set()


class _ShellExecuteInfo(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG),
                ("hwnd", wintypes.HWND), ("lpVerb", wintypes.LPCWSTR),
                ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
                ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int),
                ("hInstApp", wintypes.HINSTANCE), ("lpIDList", ctypes.c_void_p),
                ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
                ("dwHotKey", wintypes.DWORD), ("hIcon", wintypes.HANDLE),
                ("hProcess", wintypes.HANDLE)]


def run_elevated(args):
    """Run this program again with administrator rights and wait; None when refused."""
    info = _ShellExecuteInfo(cbSize=ctypes.sizeof(_ShellExecuteInfo), fMask=0x40,
                             lpVerb="runas", lpFile=sys.executable,
                             lpParameters=subprocess.list2cmdline(args), nShow=1)
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(_ShellExecuteInfo)]
    if not shell32.ShellExecuteExW(ctypes.byref(info)) or not info.hProcess:
        return None
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.WaitForSingleObject(info.hProcess, 0xFFFFFFFF)
    code = wintypes.DWORD()
    kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(code))
    kernel32.CloseHandle(info.hProcess)
    return code.value


def setup_main(args, payload):
    try:
        flags = parse(args, {"--silent", "--no-autostart", "--no-firewall"},
                      {"--dir", "--port", "--language"})
    except ValueError as error:
        say = translator(system_language())
        message_box(say("usage", arg=error), say("setup_title"))
        return 2
    info = registered()
    target = flags.get("--dir") or info.get("InstallLocation") or default_dir()
    saved = data_config()
    language = next(code for code in (flags.get("--language"), saved.get("language"),
                                      system_language()) if code in TEXT)
    say = translator(language)
    logon = has_logon_trigger()
    fields = {"dir": target, "port": flags.get("--port") or configured_port(saved),
              "language": language,
              "autostart": not flags.get("--no-autostart") and logon is not False,
              "firewall": not flags.get("--no-firewall")}

    if flags.get("--silent"):
        options, problem = validate(fields, say)
        if problem:
            with open(os.path.join(tempfile.gettempdir(), APP_NAME + "-setup.log"), "w",
                      encoding="utf-8") as handle:
                handle.write(problem + "\n")
            return 2
        return run_silent(lambda say_, log, progress: install(payload, options, say_, log,
                                                              progress),
                          say, "failed", APP_NAME + "-setup.log")

    upgrade = bool(info) or os.path.exists(os.path.join(target, EXE_NAME))
    SetupWindow(payload, fields, upgrade).run()
    return 0


def installed_main(args):
    language = data_config().get("language")
    say = translator(language if language in TEXT else system_language())
    verb = args[0] if args else ""
    if not verb:
        ManagerWindow(language if language in TEXT else system_language()).run()
        return 0
    if verb != "uninstall":
        message_box(say("usage", arg=verb), say("app"))
        return 2
    try:
        flags = parse(args[1:], {"--silent", "--purge"}, set())
    except ValueError as error:
        message_box(say("usage", arg=error), say("app"))
        return 2
    if not is_admin():
        code = run_elevated(args)
        if code is None:
            if not flags.get("--silent"):
                message_box(say("elevate_denied"), say("uninstall_title"))
            return 1
        return code
    if flags.get("--silent"):
        purge = bool(flags.get("--purge"))
        return run_silent(lambda say_, log, progress: uninstall(purge, say_, log, progress),
                          say, "u_failed", APP_NAME + "-uninstall.log")
    UninstallWindow(language if language in TEXT else system_language()).run()
    return 0
