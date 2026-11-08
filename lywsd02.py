"""BLE protocol core for Xiaomi LYWSD02 / LYWSD02MMC e-ink clocks."""

import asyncio
import struct
import time

from bleak import BleakClient, BleakScanner
from bleak.exc import (BleakBluetoothNotAvailableError, BleakBluetoothNotAvailableReason,
                       BleakDBusError)

SERVICE_UUID = "ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6"
TIME_CHAR_UUID = "ebe0ccb7-7a0a-4b0c-8a1a-6ff2997da3a6"
ENV_CHAR_UUID = "ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6"
BATTERY_CHAR_UUID = "ebe0ccc4-7a0a-4b0c-8a1a-6ff2997da3a6"
MODEL_CHAR_UUID = "00002a24-0000-1000-8000-00805f9b34fb"
SERIAL_CHAR_UUID = "00002a25-0000-1000-8000-00805f9b34fb"
FIRMWARE_CHAR_UUID = "00002a26-0000-1000-8000-00805f9b34fb"

XIAOMI_SERVICE_DATA = "0000fe95-0000-1000-8000-00805f9b34fb"
NAME_HINT = "LYWSD02"
KNOWN_PRODUCT_IDS = {0x045B, 0x16E4, 0x2542}

SYNC_TOLERANCE = 5
RADIO_START_TIMEOUT = 30.0


def encode_time(now=None):
    if now is None:
        now = time.time()
    offset = time.localtime(now).tm_gmtoff
    # Whole hours go in the timezone byte; the remainder is folded into the timestamp.
    return struct.pack("<Ib", int(now) + offset % 3600, offset // 3600)


def decode_time(data):
    return struct.unpack("<Ib", bytes(data[:5]))


def shown_time(stamp, timezone, now):
    """The instant the face displays, on the host's clock.

    The face shows stamp + timezone hours. encode_time folds a half-hour offset
    into the stamp, so comparing stamps alone would call a correctly set clock in
    UTC+5:30 thirty minutes off - and a wrong timezone byte would go unnoticed.
    """
    return stamp + timezone * 3600 - time.localtime(now).tm_gmtoff


def decode_env(data):
    data = bytes(data)
    if len(data) < 3:
        return {}
    temperature, humidity = struct.unpack("<hB", data[:3])
    report = {"temperature": round(temperature / 100, 2), "humidity": humidity}
    if len(data) >= 5:
        report["voltage"] = round(struct.unpack("<H", data[3:5])[0] / 1000, 3)
    return report


def normalize_address(value):
    return "".join(char for char in str(value).upper() if char.isalnum())


def product_id(adv):
    for uuid, value in (adv.service_data or {}).items():
        if uuid.lower() == XIAOMI_SERVICE_DATA and len(value) >= 4:
            return int.from_bytes(bytes(value)[2:4], "little")
    return None


def radio_problem(error):
    """Why this machine's Bluetooth is unusable, or None when the error is about a device."""
    if isinstance(error, BleakBluetoothNotAvailableError):
        reason = error.reason
        if reason in (BleakBluetoothNotAvailableReason.NO_BLUETOOTH,
                      BleakBluetoothNotAvailableReason.NO_BLE_CENTRAL_ROLE):
            return "noAdapter"
        if reason is BleakBluetoothNotAvailableReason.POWERED_OFF:
            return "off"
        if reason.name.startswith("DENIED"):
            return "denied"
        return "unavailable"
    if isinstance(error, BleakDBusError):
        if error.dbus_error == "org.freedesktop.DBus.Error.ServiceUnknown":
            return "noService"  # bluetoothd is not installed
        if "activate service 'org.bluez'" in (error.dbus_error_details or ""):
            # Installed but it would not start; with no Bluetooth hardware at all,
            # systemd skips it and D-Bus gives up after 25 seconds.
            return "noStart"
        if error.dbus_error == "org.bluez.Error.NotReady":
            return "off"
    return None


def looks_like_clock(device, adv):
    name = (adv.local_name or device.name or "").upper()
    if NAME_HINT in name:
        return True
    if SERVICE_UUID in {uuid.lower() for uuid in (adv.service_uuids or [])}:
        return True
    return product_id(adv) in KNOWN_PRODUCT_IDS


async def scan(timeout, known=(), limit=200):
    """Keep clocks and previously saved devices, not unrelated advertisements."""
    seen = {}

    def on_detect(device, adv):
        key = normalize_address(device.address)
        if key not in known:
            if not looks_like_clock(device, adv):
                return
            if device.address not in seen and len(seen) >= limit:
                return
        seen[device.address] = (device, adv)

    scanner = BleakScanner(detection_callback=on_detect)
    failed = True
    try:
        # A wedged BlueZ or WinRT can leave start() waiting forever, and the job
        # queue - and every peer asking us to scan - would wait with it.
        await asyncio.wait_for(scanner.start(), timeout=RADIO_START_TIMEOUT)
        await asyncio.sleep(timeout)
        failed = False
    finally:
        try:
            await _cleanup(scanner.stop())
        except Exception:
            if not failed:
                raise
    return seen


async def resolve(address, timeout):
    """A BLEDevice connects far more reliably than a bare address string.

    It has to be a fresh one: BlueZ forgets an unpaired device about half a
    minute after discovery stops, so a BLEDevice kept from an earlier scan still
    carries a D-Bus path that is gone by the time we connect, and bleak reports
    it as "device 'dev_AA_BB_..' not found".
    """
    device = await asyncio.wait_for(BleakScanner.find_device_by_address(address, timeout=timeout),
                                    timeout=timeout + RADIO_START_TIMEOUT)
    return device or address


async def _read_text(client, uuid):
    try:
        value = bytes(await asyncio.wait_for(client.read_gatt_char(uuid), timeout=10.0))
    except Exception:
        return None
    return value.split(b"\x00")[0].decode("ascii", errors="replace").strip() or None


async def _cleanup(operation):
    # Keep cleanup bounded even when its caller is cancelled again while waiting.
    task = asyncio.create_task(asyncio.wait_for(operation, timeout=5.0))
    cancelled = None
    while not task.done():
        try:
            await asyncio.shield(task)
        except asyncio.CancelledError as error:
            cancelled = error
        except Exception:
            break
    if cancelled is not None:
        try:
            task.result()
        except BaseException:
            pass
        raise cancelled
    return task.result()


async def read_env(client, timeout):
    # LYWSD02 advertises ebe0ccc1 as [notify,read] but answers the read with an
    # empty buffer: the values only ever arrive through a notification there.
    # LYWSD02MMC answers the read properly, which saves a subscription.
    try:
        report = decode_env(await asyncio.wait_for(
            client.read_gatt_char(ENV_CHAR_UUID), timeout=10.0))
    except Exception:
        report = {}
    if report:
        return report

    queue = asyncio.Queue(maxsize=1)
    active = True

    def on_notify(_, data):
        if not active:
            return
        try:
            report = decode_env(data)
        except (TypeError, ValueError, struct.error):
            return
        if not report:
            return
        if queue.full():
            queue.get_nowait()
            queue.task_done()
        queue.put_nowait(report)

    failed = True
    try:
        await asyncio.wait_for(client.start_notify(ENV_CHAR_UUID, on_notify), timeout=10.0)
        deadline = time.monotonic() + timeout
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise asyncio.TimeoutError(f"no sensor notification within {timeout}s")
        report = await asyncio.wait_for(queue.get(), timeout=remaining)
        queue.task_done()
        failed = False
        return report
    finally:
        active = False
        if not queue.empty():
            queue.get_nowait()
            queue.task_done()
        try:
            await _cleanup(client.stop_notify(ENV_CHAR_UUID))
        except Exception:
            if not failed:
                raise


async def visit(target, write=False, env=True, connect_timeout=20.0, env_timeout=15.0,
                offset=0.0):
    """Connect once and collect everything; optionally set the clock.

    offset is added to this machine's clock, so a time server can be the reference.
    """
    report = {}
    client = BleakClient(target, timeout=connect_timeout)
    total_timeout = connect_timeout + env_timeout + 60.0
    failed = True
    try:
        # Reserve five seconds each for notification and connection cleanup.
        async with asyncio.timeout(total_timeout - 10.0):
            await asyncio.wait_for(client.connect(), timeout=connect_timeout)
            report["model"] = await _read_text(client, MODEL_CHAR_UUID)
            report["firmware"] = await _read_text(client, FIRMWARE_CHAR_UUID)
            report["serial"] = await _read_text(client, SERIAL_CHAR_UUID)
            try:
                report["battery"] = bytes(await asyncio.wait_for(
                    client.read_gatt_char(BATTERY_CHAR_UUID), timeout=10.0))[0]
            except Exception:
                report["battery"] = None

            if env:
                try:
                    report.update(await read_env(client, env_timeout))
                except Exception as error:
                    report["env_error"] = repr(error)

            current = bytes(await asyncio.wait_for(
                client.read_gatt_char(TIME_CHAR_UUID), timeout=10.0))
            report["raw_time"] = current.hex()
            report.update(_clock_report(current, offset))

            if write:
                # LYWSD02MMC reports a 7-byte value; keep whatever follows the timezone byte.
                payload = encode_time(time.time() + offset) + current[5:]
                await asyncio.wait_for(
                    client.write_gatt_char(TIME_CHAR_UUID, payload, response=True), timeout=10.0)
                report.update(_clock_report(await asyncio.wait_for(
                    client.read_gatt_char(TIME_CHAR_UUID), timeout=10.0), offset))
                report["wrote"] = payload.hex()
                report["synced"] = abs(report["drift"]) <= SYNC_TOLERANCE
        failed = False
    finally:
        try:
            await _cleanup(client.disconnect())
        except Exception:
            if not failed:
                raise
    return report


def _clock_report(data, offset):
    stamp, timezone = decode_time(data)
    now = time.time() + offset
    shown = shown_time(stamp, timezone, now)
    return {"device_time": shown, "timezone": timezone, "drift": shown - int(now)}


async def visit_with_retries(target, retries=3, delay=5.0, on_retry=None, refresh=None, **kwargs):
    last = None
    for attempt in range(1, retries + 1):
        try:
            return await visit(target, **kwargs)
        except Exception as error:
            if radio_problem(error):
                raise
            last = error
            if on_retry:
                on_retry(attempt, retries, error)
            if attempt < retries:
                await asyncio.sleep(delay)
                if refresh:
                    target = await refresh()
    raise RuntimeError(f"gave up after {retries} attempts: {last!r}")
