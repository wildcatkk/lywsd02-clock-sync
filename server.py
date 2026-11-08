#!/usr/bin/env python3
"""Web UI and scheduler for LYWSD02 Clock Sync.

Settings live in config.json in the data directory (see paths.py); the settings
dialog writes that same file. Runtime state goes to state.json, device labels and
readings to devices.json, next to it.
"""

import asyncio
import datetime
import json
import logging
from logging.handlers import RotatingFileHandler
import os
import socket
import sys
import time
import uuid
from contextlib import asynccontextmanager
from typing import Annotated, Literal

import paths

DATA_DIR = paths.data_dir()
os.makedirs(DATA_DIR, exist_ok=True)

class _RotatingStream:
    """A file-like stream: preserve print/traceback writes and rotate while running."""

    encoding = "utf-8"
    errors = "replace"

    def __init__(self, path):
        self.handler = RotatingFileHandler(path, maxBytes=1_000_000, backupCount=1,
                                           encoding=self.encoding, errors=self.errors)
        self.handler.terminator = ""

    def write(self, text):
        if text:
            self.handler.handle(logging.LogRecord("console", logging.INFO, "", 0,
                                                  text, (), None))
        return len(text)

    def flush(self):
        self.handler.flush()

    def isatty(self):
        return False

    def fileno(self):
        return self.handler.stream.fileno()


# Only replace absent console streams; existing stdout/stderr remain untouched.
if sys.stdout is None or sys.stderr is None:
    stream = _RotatingStream(os.path.join(DATA_DIR, "server.log"))
    if sys.stdout is None:
        sys.stdout = stream
    if sys.stderr is None:
        sys.stderr = stream

import uvicorn
from fastapi import APIRouter, Depends, FastAPI, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field, RootModel, StringConstraints, field_validator

import cluster
import lywsd02
import ntp
import store

CONFIG_PATH = os.path.join(DATA_DIR, "config.json")
DEVICES_PATH = os.path.join(DATA_DIR, "devices.json")
STATE_PATH = os.path.join(DATA_DIR, "state.json")
STATIC_DIR = os.path.join(paths.RESOURCE_DIR, "static")

LANGUAGES = ("zh-CN", "zh-TW", "en")

DEFAULT_CONFIG = {
    "language": "zh-CN",
    "host": "0.0.0.0",
    "port": 8787,
    # name: shown in the node column; empty means "use the machine's hostname".
    # peers: hand-typed nodes, for when UDP discovery is blocked.
    # cluster: off turns this install into a single machine - no peers, no node
    # endpoints. discovery: off keeps the cluster but drops the UDP beacon.
    "node": {"name": "", "peers": [], "cluster": True, "discovery": True},
    # mode: weekly | daily | off. weekday: 0=Monday .. 6=Sunday
    "schedule": {"mode": "weekly", "weekday": 6, "hour": 4, "minute": 0},
    "bluetooth": {"scan_timeout": 25.0, "connect_timeout": 20.0,
                  "env_timeout": 15.0, "retries": 3},
    # Where the clocks get their time from: an NTP host, or empty for this machine.
    "time_server": "",
}

# A server answer is reused for this long; the offset of a sane clock barely moves.
TIME_SERVER_CACHE = 600


def load_config():
    """config.json, with the nested blocks filled in one level deep."""
    config = store.load(CONFIG_PATH, DEFAULT_CONFIG)
    for key, value in DEFAULT_CONFIG.items():
        if isinstance(value, dict):
            config[key] = dict(value, **(config.get(key) or {}))
    return config


def _at(day, hour, minute):
    return time.mktime((day.year, day.month, day.day, hour, minute, 0, 0, 0, -1))


def last_scheduled(schedule, now):
    """The most recent moment the schedule should have fired, or None when off."""
    mode = schedule.get("mode", "weekly")
    if mode not in ("daily", "weekly"):
        return None
    hour, minute = int(schedule.get("hour", 4)), int(schedule.get("minute", 0))
    # Calendar days, not multiples of 86400 s: across a DST change those land an
    # hour off, which close to midnight is the wrong day.
    day = datetime.date.fromtimestamp(now)
    if mode == "weekly":
        day -= datetime.timedelta(days=(day.weekday() - int(schedule.get("weekday", 6))) % 7)
    step = datetime.timedelta(days=1 if mode == "daily" else 7)
    if _at(day, hour, minute) > now:
        day -= step
    return _at(day, hour, minute)


def next_scheduled(schedule, now):
    previous = last_scheduled(schedule, now)
    if previous is None:
        return None
    days = 1 if schedule.get("mode") == "daily" else 7
    day = datetime.date.fromtimestamp(previous) + datetime.timedelta(days=days)
    return _at(day, int(schedule.get("hour", 4)), int(schedule.get("minute", 0)))


class Hub:
    """Holds every piece of mutable state, so the job functions stay plain."""

    def __init__(self):
        self.config = load_config()
        self.devices = store.load(DEVICES_PATH, {})
        self.state = store.load(STATE_PATH, {"last_auto_run": 0})
        if not self.state.get("last_auto_run"):
            # A fresh install has no history, and the scheduler reads "no history"
            # as "the last slot was missed" and syncs on the spot. Start the clock
            # now instead, so catch-up only covers slots this machine really slept
            # through.
            self.state["last_auto_run"] = time.time()
            self.save_state()
        self.queue = asyncio.Queue(maxsize=32)
        self.pending = set()
        self.busy = None
        self.logs = []
        self.log_boot = uuid.uuid4().hex[:16]
        self.log_cursor = 0
        self.watched_at = float("-inf")
        self.time_offset = None  # (server, offset, monotonic measurement time)

    @property
    def watched(self):
        """True while a browser is sitting on this node's page.

        Only /api/state counts, never a peer's probe: two idle nodes watching each
        other would otherwise keep each other at the fast cadence forever.
        """
        return time.monotonic() - self.watched_at < cluster.WATCH_WINDOW

    def log(self, event, **params):
        # Text from adapters, peer errors and labels must not balloon a log batch.
        safe = {}
        budget = 768
        for key, value in list(params.items())[:16]:
            key = str(key)[:64]
            if value is not None and type(value) not in (bool, int, float):
                value = str(value).replace("\x00", "")[:min(512, budget)]
            elif type(value) in (int, float):
                try:
                    cluster.number_value(value)
                except cluster.LocalFailure:
                    value = str(value)[:64]
            cost = len(json.dumps({key: value}, ensure_ascii=False).encode("utf-8"))
            if cost > budget:
                break
            safe[key] = value
            budget -= cost
        if self.log_cursor >= cluster.MAX_CURSOR:
            self.clear_logs()
        self.log_cursor += 1
        event = str(event)[:128]
        while len(json.dumps(event, ensure_ascii=False).encode("utf-8")) > 256:
            event = event[:len(event) // 2]
        self.logs.append({"id": self.log_cursor, "at": time.time(),
                          "event": event, "params": safe})
        del self.logs[:-200]
        detail = " ".join(f"{key}={value}" for key, value in safe.items())
        print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {event} {detail}".rstrip(), flush=True)

    def clear_logs(self):
        self.logs.clear()
        self.log_boot = uuid.uuid4().hex[:16]
        self.log_cursor = 0

    def log_source(self, boot="", after=0):
        return cluster.log_batch(grid.node_id, grid.name, self.log_boot,
                                 self.log_cursor, self.logs, boot, after)

    def save_devices(self):
        store.save(DEVICES_PATH, self.devices)

    def save_config(self):
        store.save(CONFIG_PATH, self.config)

    def save_state(self):
        store.save(STATE_PATH, self.state)

    def record(self, key):
        existing = self.devices.get(key)
        if existing is None:
            if len(self.devices) >= cluster.MAX_DEVICES:
                raise cluster.LocalFailure("Device record limit")
            # No label is stored, so the UI can show a default in the chosen language.
            existing = {"auto": True}
            self.devices[key] = existing
        return existing

    def name_of(self, key):
        record = self.devices.get(key) or {}
        return record.get("label") or record.get("address") or key

    def enqueue(self, key, event, run, token=None):
        if key in self.pending:
            self.log("queue.duplicate", target=self.name_of(key))
            return False
        try:
            self.pending.add(key)
            self.queue.put_nowait({"key": key, "event": event, "run": run, "token": token})
        except (asyncio.QueueFull, MemoryError):
            self.pending.discard(key)
            try:
                self.log("queue.full", target=self.name_of(key))
            except MemoryError:
                pass
            return False
        except BaseException:
            self.pending.discard(key)
            raise
        return True


hub = Hub()
grid = cluster.Cluster(hub)


async def job_scan():
    timeout = hub.config["bluetooth"]["scan_timeout"]
    hub.log("scan.start", seconds=round(timeout))
    found = 0
    heard = set()
    for address, (device, adv) in (await lywsd02.scan(
            timeout, known=hub.devices, limit=cluster.MAX_DEVICES)).items():
        key = lywsd02.normalize_address(address)
        if not lywsd02.looks_like_clock(device, adv) and key not in hub.devices:
            continue
        if key not in hub.devices and len(hub.devices) >= cluster.MAX_DEVICES:
            continue
        record = hub.record(key)
        record["address"] = address
        # Some firmware pads the name with NULs, which the ESP32 nodes cannot store.
        record["name"] = (adv.local_name or device.name or "").split("\x00")[0] or None
        record["rssi"] = adv.rssi
        record["product_id"] = lywsd02.product_id(adv)
        record["seen_at"] = time.time()
        heard.add(key)
        found += 1
    # A clock this radio no longer hears must stop counting as near it, or a moved
    # device stays owned by, and pinnable to, a node that cannot reach it.
    for key, record in hub.devices.items():
        if key not in heard:
            record["rssi"] = None
    hub.save_devices()
    hub.log("scan.done", count=found)


async def time_offset():
    """Seconds to add to this machine's clock to get the configured reference."""
    server = hub.config["time_server"]
    if not server:
        return 0.0
    cached = hub.time_offset
    if cached and cached[0] == server and time.monotonic() - cached[2] < TIME_SERVER_CACHE:
        return cached[1]
    offset = await asyncio.to_thread(ntp.query, server)
    hub.time_offset = (server, offset, time.monotonic())
    hub.log("time.offset", server=server, offset=round(offset, 3) + 0.0)  # no "-0.0"
    return offset


async def job_visit(key, write):
    record = hub.record(key)
    name = hub.name_of(key)
    address = record.get("address", key)
    radio = hub.config["bluetooth"]

    async def locate():
        target = await lywsd02.resolve(address, radio["scan_timeout"])
        if target is address:
            hub.log("device.notAdvertising", target=name, address=address)
        return target

    hub.log("device.syncing" if write else "device.reading", target=name)
    try:
        offset = await time_offset()
    except Exception as error:
        server = hub.config["time_server"]
        if write:
            # Writing the local time when somebody asked for a server would be
            # quietly doing the wrong thing; a read only reports, so it carries on.
            record["error"] = f"{server}: {error!r}"
            record["error_at"] = time.time()
            hub.save_devices()
            hub.log("time.serverFailed", target=name, server=server, error=repr(error))
            return
        hub.log("time.serverFallback", server=server, error=repr(error))
        offset = 0.0
    try:
        report = await lywsd02.visit_with_retries(
            await locate(),
            retries=radio["retries"],
            refresh=locate,
            on_retry=lambda attempt, total, error: hub.log(
                "device.retry", target=name, attempt=attempt, total=total, error=repr(error)),
            write=write,
            env=True,
            connect_timeout=radio["connect_timeout"],
            env_timeout=radio["env_timeout"],
            offset=offset,
        )
    except Exception as error:
        if lywsd02.radio_problem(error):
            raise  # Not this clock's fault, and every other clock would fail the same way.
        record["error"] = str(error)
        record["error_at"] = time.time()
        hub.save_devices()
        hub.log("device.failed", target=name, error=str(error))
        return

    record["error"] = None
    record["error_at"] = None
    record["last"] = dict(report, at=time.time())
    hub.save_devices()

    if report.get("env_error"):
        hub.log("device.envFailed", target=name, error=report["env_error"])
    if write:
        hub.log("device.synced" if report.get("synced") else "device.syncedOff",
                target=name, payload=report["wrote"], drift=report["drift"])
    else:
        hub.log("device.read", target=name, drift=report["drift"],
                temperature=report.get("temperature"), humidity=report.get("humidity"))


async def wait_for_peers(peers, deadline):
    """Unknown is not idle; only thirty continuous seconds of network failure count."""
    if not peers:
        return True
    down_since = {}
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        states = await asyncio.gather(*(grid.get(peer["url"], "/api/node/status",
                                                timeout=min(cluster.STATE_TIMEOUT, remaining))
                                       for peer in peers), return_exceptions=True)
        now = time.monotonic()
        if now >= deadline:
            return False
        finished = True
        for peer, state in zip(peers, states):
            url = peer["url"]
            if isinstance(state, BaseException) and cluster.network_failure(state):
                since = down_since.setdefault(url, now)
                if now - since < cluster.DOWN_GRACE:
                    finished = False
                continue
            down_since.pop(url, None)  # Unknown/local/5xx breaks the network streak.
            try:
                if isinstance(state, BaseException):
                    raise cluster.LocalFailure(str(state))
                cluster.validate_status(state)
                if (peer.get("node_id") and state["node"]["id"] != peer["node_id"]):
                    raise cluster.LocalFailure("Status identity changed")
                if state["busy"] is not None or state["queued"]:
                    finished = False
            except Exception as error:
                grid.local_failure(error)
                finished = False
        if finished:
            return True
        await asyncio.sleep(min(2, max(0, deadline - time.monotonic())))
    return False


async def start_on_peers(path, token, event):
    """Return accepted or unacknowledged jobs; an unknown reply may have queued work."""
    peers = grid.online()
    if not peers:
        return []
    hub.log(event, count=len(peers))
    answers = await asyncio.gather(*(grid.post(peer["url"], path, {"token": token})
                                     for peer in peers), return_exceptions=True)
    started = []
    for peer, answer in zip(peers, answers):
        acknowledged = (isinstance(answer, dict) and set(answer) == {"queued"}
                        and type(answer["queued"]) is bool)
        if acknowledged and answer["queued"]:
            started.append(peer)
            continue
        refused = acknowledged or isinstance(answer, cluster.Rejected) and 400 <= answer.status < 500
        if not refused:
            started.append(peer)  # Do not release the lock on an unknown job result.
        if acknowledged:
            hub.log("cluster.peerFailed", node=peer["name"], error="error.alreadyQueued")
            continue
        if not isinstance(answer, BaseException):
            answer = cluster.LocalFailure("Job was not acknowledged as queued")
        said = getattr(answer, "detail", None)
        hub.log("cluster.peerFailed", node=peer["name"],
                error=said.get("event") or str(answer) if isinstance(said, dict) else str(answer))
    return started


async def job_scan_cluster(token):
    """Scan on every node at once, and hold the lock until the last one is done."""
    # Peers first and in parallel: each has its own radio, and a local failure -
    # no adapter, BlueZ busy - must not stop the rest of the house from scanning.
    started = time.monotonic()
    peers = await start_on_peers("/api/node/scan", token, "cluster.scanPeers")
    try:
        await job_scan()
    finally:
        # A peer may use a longer scan window than ours, so wait as long as the
        # lock allows; this returns as soon as every peer has gone idle.
        if (peers and not asyncio.current_task().cancelling()
                and not await wait_for_peers(peers, started + cluster.LOCK_TTL - 60)):
            hub.log("cluster.scanTimeout")


async def job_read_owned():
    """Read every device this node owns, whatever its auto-sync switch says."""
    # Ownership is worked out from the peers' signal reports, and an idle node's
    # copy of those can be a minute old.
    await grid.ensure_fresh()
    keys = [device["key"] for device in grid.merge(local_views())
            if device["node_id"] == grid.node_id and device["key"] in hub.devices]
    if not keys:
        hub.log("read.nothingHere")
    for key in keys:
        await job_visit(key, write=False)


async def job_read_cluster(token):
    """Every node reads its own devices at once; the lock is held until all are done."""
    started = time.monotonic()
    peers = await start_on_peers("/api/node/read", token, "cluster.readPeers")
    try:
        await job_read_owned()
    finally:
        # A slow device can take minutes with retries, so wait for as long as the
        # peers will keep our lock, not for a fixed scan window.
        if (peers and not asyncio.current_task().cancelling()
                and not await wait_for_peers(peers, started + cluster.LOCK_TTL - 60)):
            hub.log("cluster.readTimeout")


async def job_auto():
    await job_scan()
    mine, taken = grid.my_share(grid.merge(local_views()))
    for key in taken:
        hub.log("cluster.takeover", target=hub.name_of(key))
    for key in mine + taken:
        await job_visit(key, write=True)
    if not mine and not taken:
        hub.log("schedule.nothingHere")
    hub.state["last_auto_run"] = time.time()
    hub.save_state()


async def worker():
    while True:
        job = await hub.queue.get()
        hub.busy = job["event"]
        keeper = None
        try:
            keeper = asyncio.create_task(grid.keep(job["token"])) if job["token"] else None
            await job["run"]()
        except Exception as error:
            reason = lywsd02.radio_problem(error)
            if reason:
                hub.log(f"bluetooth.{reason}", error=str(error.args[0] if error.args else error))
            else:
                hub.log("job.crashed", error=repr(error))
        finally:
            async def cleanup():
                if keeper:
                    keeper.cancel()
                    await asyncio.gather(keeper, return_exceptions=True)
                if job["token"]:
                    try:
                        await grid.release(job["token"])
                    except Exception as error:
                        hub.log("job.crashed", error=repr(error))

            try:
                await cluster.finish_cleanup(cleanup())
            finally:
                hub.busy = None
                hub.pending.discard(job["key"])
                hub.queue.task_done()


async def scheduler():
    while True:
        await asyncio.sleep(60)
        token = None
        try:
            due = last_scheduled(hub.config.get("schedule", {}), time.time())
            if due is None or hub.state.get("last_auto_run", 0) >= due:
                continue
            if not grid.my_work(grid.merge(local_views())):
                hub.state["last_auto_run"] = time.time()
                hub.save_state()
                hub.log("schedule.nothingHere")
                continue
            event = {"event": "busy.scheduled", "params": {}}
            token = await grid.acquire(event)
            hub.log("schedule.triggered", due=time.strftime("%Y-%m-%d %H:%M", time.localtime(due)))
            if hub.enqueue("auto", event, job_auto, token):
                token = None
        except cluster.Busy:
            continue
        except (MemoryError, cluster.LocalFailure) as error:
            grid.local_failure(error)
        finally:
            if token:
                await cluster.finish_cleanup(grid.release(token))


@asynccontextmanager
async def lifespan(_app):
    hub.log("service.start", port=hub.config["port"])
    hub.log("cluster.node", node=grid.name)
    tasks = [asyncio.create_task(worker()), asyncio.create_task(scheduler()),
             asyncio.create_task(grid.beacon()), asyncio.create_task(grid.poll())]
    try:
        yield
    finally:
        async def cleanup():
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            tokens = set()
            while not hub.queue.empty():
                job = hub.queue.get_nowait()
                try:
                    hub.pending.discard(job["key"])
                    if job["token"]:
                        grid.drop(job["token"])
                        tokens.add(job["token"])
                finally:
                    hub.queue.task_done()
            await asyncio.gather(*(grid.release(token) for token in tokens),
                                 return_exceptions=True)
            await grid.close()  # Also reclaim a shielded refresh left by a browser.

        await cluster.finish_cleanup(cleanup())


class BoundedJSONBody:
    """Bound ASGI reads before FastAPI can call request.json()/body()."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        path = scope.get("path", "")
        limit = 32 * 1024
        if path == "/api/logs" or path.startswith("/api/node/lock") or path.startswith("/api/node/unlock"):
            limit = 4096
        elif path == "/api/settings":
            limit = 8192
        elif path.startswith("/api/devices/"):
            limit = 4096
        body = bytearray()
        try:
            lengths = [value for key, value in scope.get("headers", []) if key.lower() == b"content-length"]
            if len(lengths) > 1:
                raise ValueError("Duplicate Content-Length")
            if lengths:
                length = int(lengths[0])
                if length < 0:
                    raise ValueError("Invalid Content-Length")
                if length > limit:
                    return await JSONResponse({"detail": "Request body too large"}, 413)(scope, receive, send)
            while True:
                message = await receive()
                if message["type"] == "http.disconnect":
                    return
                if message["type"] != "http.request":
                    raise ValueError("Invalid request message")
                chunk = message.get("body", b"")
                if len(body) + len(chunk) > limit:
                    return await JSONResponse({"detail": "Request body too large"}, 413)(scope, receive, send)
                body.extend(chunk)
                if not message.get("more_body", False):
                    break
            if body:
                cluster.bounded_json(body)
        except (ValueError, cluster.LocalFailure, MemoryError):
            return await JSONResponse({"detail": "Invalid or excessively nested JSON"}, 400)(scope, receive, send)
        delivered = False

        async def bounded_receive():
            nonlocal delivered
            if not delivered:
                delivered = True
                return {"type": "http.request", "body": bytes(body), "more_body": False}
            return await receive()

        await self.app(scope, bounded_receive, send)


app = FastAPI(lifespan=lifespan)
app.add_middleware(BoundedJSONBody)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


def fit(text, size):
    """At most `size` UTF-8 bytes and no NUL, as the ESP32 nodes require."""
    if text is None:
        return None
    text = text.replace("\x00", "")
    data = text.encode("utf-8")
    return text if len(data) <= size else data[:size].decode("utf-8", "ignore")


def device_view(key, record):
    last = record.get("last") or {}
    return {
        "key": key,
        "address": record.get("address", key),
        "label": fit(record.get("label"), 160),
        "label_at": record.get("label_at"),
        "auto": record.get("auto", True),
        "auto_at": record.get("auto_at"),
        "owner_pin": record.get("owner_pin"),
        "pin_at": record.get("pin_at"),
        "name": fit(record.get("name"), 96),
        "rssi": record.get("rssi"),
        "seen_at": record.get("seen_at"),
        "model": fit(last.get("model"), 128),
        "firmware": fit(last.get("firmware"), 128),
        "serial": fit(last.get("serial"), 128),
        "battery": last.get("battery"),
        "voltage": last.get("voltage"),
        "temperature": last.get("temperature"),
        "humidity": last.get("humidity"),
        "device_time": last.get("device_time"),
        "timezone": last.get("timezone"),
        "drift": last.get("drift"),
        "updated_at": last.get("at"),
        "error": fit(record.get("error"), 512),
        "error_at": record.get("error_at"),
        "pending": key in hub.pending,
    }


def local_views(limit=None):
    items = sorted(hub.devices.items(), key=lambda item: item[0])
    if limit is not None:
        items = items[:limit]
    return [device_view(key, record) for key, record in items]


def local_status():
    return {"node": grid.self_view, "busy": hub.busy, "queued": hub.queue.qsize()}


def local_state():
    """Node snapshots carry only the log version, never the log history."""
    return dict(local_status(), host_time=time.time(),
                log_boot=hub.log_boot, log_cursor=hub.log_cursor,
                devices=local_views(cluster.MAX_DEVICES))


def detail(event, **params):
    """An error the page can translate, instead of a sentence baked into the server."""
    return {"event": event, "params": params}


class DevicePatch(BaseModel):
    label: str | None = Field(default=None, max_length=40)
    auto: bool | None = None
    owner_pin: str | None = Field(default=None, max_length=64)


class NodeSettings(BaseModel):
    name: str = Field(min_length=1, max_length=24)
    peers: list[Annotated[str, StringConstraints(max_length=256)]] = Field(default_factory=list, max_length=10)
    cluster: bool = True
    discovery: bool = True


class ScheduleSettings(BaseModel):
    mode: Literal["weekly", "daily", "off"]
    weekday: int = Field(ge=0, le=6)
    hour: int = Field(ge=0, le=23)
    minute: int = Field(ge=0, le=59)


class RadioSettings(BaseModel):
    scan_timeout: float = Field(ge=5, le=120)
    connect_timeout: float = Field(ge=5, le=120)
    env_timeout: float = Field(ge=1, le=60)
    retries: int = Field(ge=1, le=10)


class Settings(BaseModel):
    host: str = Field(min_length=1, max_length=64)
    port: int = Field(ge=1, le=65535)
    node: NodeSettings
    schedule: ScheduleSettings
    bluetooth: RadioSettings
    time_server: str = Field(default="", max_length=64, pattern=r"^[A-Za-z0-9.-]*$")


class LanguageChoice(BaseModel):
    language: Literal["zh-CN", "zh-TW", "en"]


class LockClaim(BaseModel):
    token: str = Field(min_length=8, max_length=64)
    holder_id: str = Field(min_length=1, max_length=64)
    holder_name: str = Field(min_length=1, max_length=64)
    op: dict | None = None

    @field_validator("op")
    @classmethod
    def valid_operation(cls, value):
        try:
            cluster.validate_busy(value)
        except cluster.LocalFailure as error:
            raise ValueError(str(error)) from error
        return value


class LockToken(BaseModel):
    token: str | None = Field(default=None, min_length=8, max_length=64)


BootQuery = Annotated[str, Query(pattern=r"^(?:[0-9a-fA-F]{16})?$")]
AfterQuery = Annotated[int, Query(ge=0, le=cluster.MAX_CURSOR)]


class LogPosition(BaseModel):
    boot: str = Field(default="", max_length=16, pattern=r"^(?:[0-9a-fA-F]{16})?$")
    after: int = Field(default=0, ge=0, le=cluster.MAX_CURSOR, strict=True)


class LogPositions(RootModel):
    root: dict[Annotated[str, StringConstraints(min_length=1, max_length=64)], LogPosition] = Field(max_length=11)


@app.get("/")
async def index():
    return FileResponse(os.path.join(STATIC_DIR, "index.html"))


# ---------------------------------------------------------------- node to node
#
# These are what a peer calls. They act on this node's own radio and never forward
# anywhere else, so a request can only ever travel one hop.


def cluster_enabled():
    if not grid.enabled:
        raise HTTPException(status_code=503, detail=detail("error.clusterOff"))


node = APIRouter(prefix="/api/node", dependencies=[Depends(cluster_enabled)])


@node.get("/state")
async def node_state(request: Request, port: Annotated[int | None, Query(ge=1, le=65535)] = None):
    # A probe is also an announcement: on a network where our broadcast never
    # arrives, this is the only thing that tells us the caller is out there.
    if port and request.client:
        grid.hear(request.client.host, port)
    return local_state()


@node.get("/status")
async def node_status():
    return local_status()


@node.get("/logs")
async def node_logs(boot: BootQuery = "", after: AfterQuery = 0):
    return hub.log_source(boot, after)


@node.post("/lock")
async def node_lock(claim: LockClaim):
    return grid.grant(claim.model_dump())


@node.post("/unlock")
async def node_unlock(body: LockToken):
    grid.drop(body.token)
    return {"released": True}


@node.post("/scan")
async def node_scan(body: LockToken):
    # A cluster scan is one operation, so the coordinator holds the lock for all of
    # us and this endpoint must not try to take it again.
    if not grid.holds(body.token):
        raise HTTPException(status_code=409, detail=detail("error.staleToken"))
    return {"queued": hub.enqueue("scan", {"event": "busy.scan", "params": {}}, job_scan)}


@node.post("/read")
async def node_read(body: LockToken):
    # Same as /scan: the coordinator already holds the lock on our behalf.
    if not grid.holds(body.token):
        raise HTTPException(status_code=409, detail=detail("error.staleToken"))
    return {"queued": hub.enqueue("readOwned", {"event": "busy.readOwned", "params": {}},
                                  job_read_owned)}


@node.post("/logs/clear")
async def node_clear_logs():
    hub.clear_logs()
    grid.clear_log_cache()
    return {"logs": []}


async def start_visit(key, write):
    """Take the cluster lock and queue the job on this node's radio."""
    if key not in hub.devices:
        raise HTTPException(status_code=404, detail=detail("error.unknownDevice"))
    event = {"event": "busy.sync" if write else "busy.read",
             "params": {"target": hub.name_of(key)}}
    try:
        token = await grid.acquire(event)
    except cluster.Busy as error:
        raise HTTPException(status_code=409,
                            detail=detail("error.clusterBusy", node=error.holder)) from None
    if not hub.enqueue(key, event, lambda: job_visit(key, write=write), token):
        await grid.release(token)
        return {"queued": False}
    return {"queued": True}


@node.post("/devices/{key}/refresh")
async def node_refresh(key: str):
    return await start_visit(key, write=False)


@node.post("/devices/{key}/sync")
async def node_sync(key: str):
    return await start_visit(key, write=True)


app.include_router(node)


# -------------------------------------------------------------------- browser
#
# These serve the page. Anything that needs a radio runs here when this node owns
# the device, and is forwarded to the owning node when it does not.

@app.get("/api/state")
async def get_state():
    # The page is open, so the cluster view should be current and stay current.
    # Only the first request wakes the loops; doing it on every one would send a
    # beacon every two seconds and race the poll loop to the same probes.
    if not hub.watched:
        grid.wake()
    hub.watched_at = time.monotonic()
    await grid.ensure_fresh()
    local = local_status()
    merged = grid.merge(local_views())
    schedule = hub.config.get("schedule", {})
    language = hub.config.get("language", "zh-CN")

    return {
        "language": language if language in LANGUAGES else "zh-CN",
        "cluster": grid.enabled,
        "node": local["node"],
        "nodes": grid.nodes_view(local, merged),
        "lock": grid.lock_view(),
        "busy": local["busy"],
        "queued": local["queued"],
        "host_time": time.time(),
        "schedule": schedule,
        "next_run": next_scheduled(schedule, time.time()),
        "devices": merged,
    }


@app.get("/api/ping")
async def ping():
    # For the Windows control window, which only asks "is it up?": /api/state would
    # count as somebody watching and put the whole cluster on its fast cadence.
    return {"ok": True}


@app.post("/api/scan")
async def post_scan():
    event = {"event": "busy.clusterScan", "params": {}}
    try:
        token = await grid.acquire(event)
    except cluster.Busy as error:
        raise HTTPException(status_code=409,
                            detail=detail("error.clusterBusy", node=error.holder)) from None
    if not hub.enqueue("scan", event, lambda: job_scan_cluster(token), token):
        await grid.release(token)
        return {"queued": False}
    return {"queued": True}


@app.post("/api/read")
async def post_read():
    event = {"event": "busy.clusterRead", "params": {}}
    try:
        token = await grid.acquire(event)
    except cluster.Busy as error:
        raise HTTPException(status_code=409,
                            detail=detail("error.clusterBusy", node=error.holder)) from None
    if not hub.enqueue("readAll", event, lambda: job_read_cluster(token), token):
        await grid.release(token)
        return {"queued": False}
    return {"queued": True}


@app.post("/api/logs")
async def post_logs(positions: LogPositions):
    if not hub.watched:
        grid.wake()
    hub.watched_at = time.monotonic()
    await grid.ensure_fresh()
    requested = positions.root
    position = requested.get(grid.node_id) or LogPosition()
    sources = [hub.log_source(position.boot, position.after)]
    seen = {grid.node_id}
    for peer in grid.peers.values():
        node_id = peer["node_id"]
        if not node_id or node_id in seen or peer["log_boot"] is None:
            continue
        position = requested.get(node_id) or LogPosition()
        sources.append(cluster.log_batch(node_id, peer["name"], peer["log_boot"],
                       peer["log_cursor"], peer["logs"], position.boot, position.after))
        seen.add(node_id)
    return {"sources": sources}


@app.post("/api/logs/clear")
async def post_clear_logs():
    hub.clear_logs()
    grid.clear_log_cache()
    try:
        await asyncio.gather(*(grid.post(peer["url"], "/api/node/logs/clear")
                               for peer in grid.online()), return_exceptions=True)
    finally:
        grid.clear_log_cache()  # Discard any round that raced remote clearing.
    return {"logs": []}


SETTING_KEYS = ("node", "host", "port", "schedule", "bluetooth", "time_server")


def settings_view():
    view = {key: hub.config[key] for key in SETTING_KEYS}
    # The stored name may be blank, which means "use the hostname" - show that.
    view["node"] = dict(view["node"], name=grid.name)
    return view


@app.get("/api/settings")
async def get_settings():
    return settings_view()


@app.put("/api/settings")
async def put_settings(settings: Settings):
    incoming = settings.model_dump()
    incoming["host"] = incoming["host"].strip()
    if not incoming["host"]:
        raise HTTPException(status_code=400, detail=detail("error.emptyHost"))

    name = incoming["node"]["name"].strip()
    if not name:
        raise HTTPException(status_code=400, detail=detail("error.emptyNodeName"))
    # The field is pre-filled with the machine name, so writing it back verbatim
    # would quietly freeze today's hostname into the file and stop the name
    # following a rename. Storing the blank keeps the default a default.
    incoming["node"]["name"] = "" if name == socket.gethostname() else name

    peers = []
    for entry in incoming["node"]["peers"]:
        if not entry.strip():
            continue
        url = cluster.normalize_peer(entry, incoming["port"])
        if url is None:
            raise HTTPException(status_code=400, detail=detail("error.badPeer", peer=entry))
        if url not in peers:
            peers.append(url)
    incoming["node"]["peers"] = peers

    # The listening socket is already bound, so those two only take effect on restart.
    restart = (incoming["host"], incoming["port"]) != (hub.config["host"],
                                                       int(hub.config["port"]))
    # Updating key by key leaves anything hand-added alone.
    hub.config.update(incoming)
    hub.save_config()
    grid.remember(grid.node_id, grid.name)
    # Either switch may just have been flipped, and both loops are asleep.
    grid.wake()
    hub.log("settings.saved")
    return {"settings": settings_view(), "restart_required": restart}


# The picker sits outside the dialog, so the language has an endpoint of its own.
@app.put("/api/language")
async def put_language(choice: LanguageChoice):
    hub.config["language"] = choice.language
    hub.save_config()
    hub.log("settings.saved")
    return {"language": choice.language}


def find(key):
    for device in grid.merge(local_views()):
        if device["key"] == key:
            return device
    raise HTTPException(status_code=404, detail=detail("error.unknownDevice"))


async def dispatch(key, write):
    """Nearest node does the work: run it here, or hand it to whoever owns the device."""
    # Who owns the device, and whether that node is up, is decided from the peer
    # states - so make sure they are not minutes old before acting on them.
    await grid.ensure_fresh()
    device = find(key)
    if device["node_id"] == grid.node_id:
        return await start_visit(key, write)
    peer = next((item for item in grid.online()
                 if item["node_id"] == device["node_id"]), None)
    if peer is None:
        raise HTTPException(status_code=503,
                            detail=detail("error.nodeOffline", node=device["node"]))
    action = "sync" if write else "refresh"
    try:
        return await grid.post(peer["url"], f"/api/node/devices/{key}/{action}")
    except cluster.Rejected as error:
        raise HTTPException(status_code=error.status, detail=error.detail) from None
    except Exception as error:
        raise HTTPException(status_code=503, detail=detail(
            "error.nodeFailed", node=peer["name"], error=str(error))) from None


@app.post("/api/devices/{key}/refresh")
async def post_refresh(key: str):
    return await dispatch(key, write=False)


@app.post("/api/devices/{key}/sync")
async def post_sync(key: str):
    return await dispatch(key, write=True)


@app.patch("/api/devices/{key}")
async def patch_device(key: str, patch: DevicePatch):
    device = find(key)
    # Labels, the auto flag and the pin are edited on whichever page is open and then
    # spread to the other nodes by timestamp, so they survive an offline peer.
    record = dict(hub.devices.get(key) or {"auto": True})
    record.setdefault("address", device["address"])
    now = time.time()

    if patch.label is not None:
        label = patch.label.strip()[:40]
        if not label:
            raise HTTPException(status_code=400, detail=detail("error.emptyLabel"))
        record["label"], record["label_at"] = label, now
    if patch.auto is not None:
        record["auto"], record["auto_at"] = patch.auto, now
    if "owner_pin" in patch.model_fields_set:
        pin = (patch.owner_pin or "").strip() or None
        if pin and pin != device["owner_pin"] and not any(
                heard["node_id"] == pin and heard["fresh"] for heard in device["sightings"]):
            raise HTTPException(status_code=400, detail=detail("error.nodeNotHeard"))
        record["owner_pin"], record["pin_at"] = pin, now

    if key not in hub.devices and len(hub.devices) >= cluster.MAX_DEVICES:
        raise HTTPException(status_code=503, detail="Device record limit")
    hub.devices[key] = record
    hub.save_devices()
    return {"ok": True}


def main():
    uvicorn.run(app, host=hub.config["host"], port=int(hub.config["port"]), log_level="warning")


if __name__ == "__main__":
    main()
