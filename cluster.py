"""Everything that makes one installation part of a cluster.

Nodes are equals: no master, no shared database. Each node owns the devices its own
radio hears best and learns about the rest from its peers over HTTP, so any page can
drive any device. One lock, granted by every node at once, keeps two people - or two
schedulers - from driving two radios at the same moment.
"""

import asyncio
import errno
import http.client
import ipaddress
import json
import math
import random
import re
import socket
import time
import urllib.error
import urllib.request
import uuid

import ifaddr

MAX_RESPONSE = 32 * 1024
MAX_PEERS = 10
MAX_KNOWN = 64
MAX_DEVICES = 200
MAX_CURSOR = (1 << 32) - 1
LOG_BATCH = 20
LOG_CACHE = 60
BOOT = re.compile(r"^[0-9a-fA-F]{16}$")


class LocalFailure(Exception):
    """Local resource exhaustion or an untrustworthy/unknown response, not downtime."""


class NetworkFailure(Exception):
    """A positively identified transport failure."""


def network_failure(error):
    if isinstance(error, NetworkFailure):
        return True
    if isinstance(error, urllib.error.HTTPError):
        return False
    if isinstance(error, urllib.error.URLError):
        return network_failure(error.reason)
    return (isinstance(error, (TimeoutError, ConnectionError, socket.gaierror))
            or isinstance(error, OSError) and error.errno in {
                errno.ENETDOWN, errno.ENETUNREACH, errno.EHOSTUNREACH,
                errno.ECONNRESET, errno.ECONNREFUSED, errno.ECONNABORTED,
                errno.ETIMEDOUT, errno.EPIPE})


class _DeadlineConnection(http.client.HTTPConnection):
    """urllib arms the socket timeout once per operation, so a slow connect followed
    by a slow reply could take twice the deadline; this one re-arms what is left."""

    def __init__(self, *args, deadline, **kwargs):
        super().__init__(*args, **kwargs)
        self._deadline = deadline

    def connect(self):
        super().connect()
        left = self._deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("timed out")
        self.sock.settimeout(left)


class _DeadlineHandler(urllib.request.HTTPHandler):
    def __init__(self, deadline):
        super().__init__()
        self._deadline = deadline

    def http_open(self, req):
        return self.do_open(_DeadlineConnection, req, deadline=self._deadline)


def _opener(deadline):
    # Peers are on the LAN; a desktop proxy setting must not route them elsewhere.
    return urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                       _DeadlineHandler(deadline))


def bounded_json(data, max_depth=8):
    """Check nesting before allocating a JSON tree (braces inside strings do not count)."""
    depth = 0
    quoted = escaped = False
    try:
        text = data.decode("utf-8")
        for char in text:
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char in "[{":
                depth += 1
                if depth > max_depth:
                    raise LocalFailure("JSON nesting limit")
            elif char in "]}":
                depth -= 1
        value = json.loads(text, parse_constant=lambda value: (_ for _ in ()).throw(
            ValueError("non-finite JSON number")))
        # A "\ud800" escape decodes to a lone surrogate, which nothing can encode
        # back to UTF-8 - storing one would break every later answer of ours.
        json.dumps(value, ensure_ascii=False).encode("utf-8")
        # cJSON on the ESP32 nodes cannot hold a NUL; one stored here would make
        # every later answer of ours unreadable to them.
        if _has_nul(value):
            raise ValueError("NUL in JSON string")
        return value
    except (ValueError, UnicodeError, RecursionError, MemoryError) as error:
        raise LocalFailure("Invalid JSON or local JSON resource failure") from error


def _has_nul(value):
    if isinstance(value, str):
        return "\x00" in value
    if isinstance(value, dict):
        return any(_has_nul(key) or _has_nul(item) for key, item in value.items())
    if isinstance(value, list):
        return any(_has_nul(item) for item in value)
    return False


async def finish_cleanup(awaitable):
    """Finish bounded async cleanup even if the owner receives another cancellation."""
    task = asyncio.ensure_future(awaitable)
    cancelled = False
    while not task.done():
        try:
            await asyncio.shield(task)
        except asyncio.CancelledError:
            cancelled = True
    result = task.result()
    if cancelled:
        raise asyncio.CancelledError
    return result


def text_value(value, maximum, nullable=False, nonempty=False):
    if nullable and value is None:
        return
    if not isinstance(value, str) or len(value) > maximum or nonempty and not value:
        raise LocalFailure("Invalid string field")


def number_value(value, low=-1e15, high=1e15, nullable=False, integer=False):
    if nullable and value is None:
        return
    if (type(value) not in ((int,) if integer else (int, float))
            or not low <= value <= high or not math.isfinite(value)):
        raise LocalFailure("Invalid numeric field")


def validate_params(params):
    if not isinstance(params, dict) or len(params) > 16:
        raise LocalFailure("Invalid log parameters")
    # Bound the whole entry too: twenty entries must fit inside the HTTP limit.
    size = 0
    for key, value in params.items():
        text_value(key, 64, nonempty=True)
        if isinstance(value, str):
            text_value(value, 512)
        elif value is not None and type(value) is not bool:
            number_value(value)
        size += len(key.encode("utf-8")) + len(str(value).encode("utf-8"))
    if size > 1024:
        raise LocalFailure("Log parameter size limit")


def validate_busy(value):
    if value is None:
        return
    if not isinstance(value, dict) or set(value) != {"event", "params"}:
        raise LocalFailure("Invalid busy field")
    text_value(value["event"], 128, nonempty=True)
    validate_params(value["params"])


def validate_status(state, full=False):
    if not isinstance(state, dict) or not {"node", "busy", "queued"} <= state.keys():
        raise LocalFailure("Missing status fields")
    if not full and set(state) != {"node", "busy", "queued"}:
        raise LocalFailure("Invalid status fields")
    node = state["node"]
    if not isinstance(node, dict) or set(node) != {"id", "name"}:
        raise LocalFailure("Invalid node identity")
    text_value(node["id"], 64, nonempty=True)
    text_value(node["name"], 64, nonempty=True)
    validate_busy(state["busy"])
    number_value(state["queued"], 0, 32, integer=True)
    return state


def validate_state(state):
    validate_status(state, full=True)
    if set(state) != {"node", "busy", "queued", "host_time", "devices",
                      "log_boot", "log_cursor"}:
        raise LocalFailure("Invalid state fields")
    number_value(state["host_time"], 0)
    if not isinstance(state["log_boot"], str) or not BOOT.fullmatch(state["log_boot"]):
        raise LocalFailure("Invalid log epoch")
    number_value(state["log_cursor"], 0, MAX_CURSOR, integer=True)
    devices = state["devices"]
    if not isinstance(devices, list) or len(devices) > MAX_DEVICES:
        raise LocalFailure("Device limit")
    strings = {"key": 128, "address": 128, "label": 40, "owner_pin": 64,
               "name": 128, "model": 128, "firmware": 128, "serial": 128,
               "error": 512}
    numeric = {"label_at", "auto_at", "pin_at", "seen_at", "updated_at", "error_at",
               "rssi", "battery", "voltage", "temperature", "humidity",
               "device_time", "timezone", "drift"}
    ranges = {"rssi": (-200, 100), "battery": (0, 100), "voltage": (0, 1000),
              "temperature": (-273.15, 1000), "humidity": (0, 100), "timezone": (-24, 24)}
    keys = set()
    for view in devices:
        if (not isinstance(view, dict) or not {"key", "address"} <= view.keys()
                or set(view) - (strings.keys() | numeric | {"auto", "pending"})):
            raise LocalFailure("Invalid device fields")
        for field, maximum in strings.items():
            if field in view:
                text_value(view[field], maximum, nullable=field not in ("key", "address"),
                           nonempty=field in ("key", "address"))
        if view["key"] in keys:
            raise LocalFailure("Duplicate device key")
        keys.add(view["key"])
        for field in numeric & view.keys():
            low, high = ranges.get(field, (0, 1e15) if field.endswith("_at") else (-1e15, 1e15))
            number_value(view[field], low, high, nullable=True)
        for field in ("auto", "pending"):
            if field in view and type(view[field]) is not bool:
                raise LocalFailure("Invalid device boolean")
    return state


def log_batch(node_id, name, boot, cursor, logs, requested_boot="", after=0):
    """The cursor is the last delivered ID, not the head of a not-yet-drained batch."""
    reset = (requested_boot != boot or after > cursor
             or logs and after < logs[0]["id"] - 1)
    entries = logs[-LOG_BATCH:] if reset else [entry for entry in logs if entry["id"] > after][:LOG_BATCH]
    return {"id": node_id, "name": name, "boot": boot,
            "cursor": entries[-1]["id"] if entries else cursor, "logs": entries}


def validate_logs(source, node_id, name, boot, after):
    if not isinstance(source, dict) or set(source) != {"id", "name", "boot", "cursor", "logs"}:
        raise LocalFailure("Invalid log source")
    if (source["id"] != node_id or source["name"] != name or source["boot"] != boot):
        raise LocalFailure("Log identity/epoch changed during probe")
    number_value(source["cursor"], 0, MAX_CURSOR, integer=True)
    # Past the probed head is fine: the node may have logged since it answered.
    if not after <= source["cursor"]:
        raise LocalFailure("Invalid log cursor")
    entries = source["logs"]
    if not isinstance(entries, list) or len(entries) > LOG_BATCH:
        raise LocalFailure("Log batch limit")
    previous = after
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"id", "at", "event", "params"}:
            raise LocalFailure("Invalid log entry")
        number_value(entry["id"], previous + 1, source["cursor"], integer=True)
        number_value(entry["at"], 0)
        text_value(entry["event"], 128, nonempty=True)
        validate_params(entry["params"])
        previous = entry["id"]
    if (entries and previous != source["cursor"]
            or not entries and source["cursor"] != after):
        raise LocalFailure("Undelivered log cursor")
    return source

BEACON_PORT = 8788
BEACON_FAST = 10.0           # while a page is open, so a new node shows up quickly
BEACON_SLOW = 60.0           # nobody is watching; one packet a minute is plenty
BEACON_TTL = 150.0           # must outlast BEACON_SLOW several times over
# The cadence is a freshness knob for the page, never a correctness one: every
# action calls ensure_fresh() first, so a slow loop can only make the table a few
# seconds stale, never make a node act on a stale view.
POLL_WATCHED = 2.0           # a browser is on this node right now
POLL_IDLE = 60.0             # peers exist, nobody is looking
POLL_ALONE = 300.0           # no peer has ever answered; just keep listening
WATCH_WINDOW = 15.0          # how long one /api/state counts as "being watched"
FRESH_ENOUGH = 3.0           # a probe this recent needs no repeat
STATE_TIMEOUT = 1.5
ACTION_TIMEOUT = 8.0
# The socket times out on the same deadline the caller waits for, so without a
# little slack a plain network timeout loses that race and reads as a local fault.
HTTP_DIAGNOSIS_GRACE = 1.0
LOCK_TTL = 900.0             # safety valve: a crashed holder must not wedge the cluster
DOWN_GRACE = 30.0            # how long a holder may be unreachable before we drop its lock
# A node that is simply switched off must not be chased at the watched cadence: every
# attempt blocks for the full STATE_TIMEOUT, and with a page open that is most of
# every two seconds, for nothing. Matching POLL_IDLE means a machine that is away
# costs the same whether or not somebody is looking at a page, and still comes back
# within a minute - sooner, because its own beacon wakes the loop.
DOWN_BACKOFF = 60.0
BUCKET = 6                   # dBm per ownership step; noise below this must not move a device
SIGHTING_TTL = 7 * 86400
TAKEOVER_AFTER = 26 * 3600

HOSTNAME = re.compile(r"^[A-Za-z0-9._-]+$")


class Busy(Exception):
    """Another node is already doing something, and only one of us may at a time."""

    def __init__(self, holder):
        super().__init__(holder)
        self.holder = holder


class Rejected(Exception):
    """A peer answered with an HTTP error that the browser should see verbatim."""

    def __init__(self, status, detail):
        super().__init__(detail)
        self.status = status
        self.detail = detail


def normalize_peer(entry, default_port):
    """"10.0.0.5", "10.0.0.5:8787" or "http://10.0.0.5:8787" -> "http://10.0.0.5:8787"."""
    text = str(entry or "").strip()
    if "://" in text:
        text = text.split("://", 1)[1]
    text = text.strip("/")
    if not text:
        return None
    host, _, port = text.partition(":")
    if not HOSTNAME.match(host):
        return None
    try:
        number = int(port) if port else int(default_port)
    except ValueError:
        return None
    return f"http://{host}:{number}" if 1 <= number <= 65535 else None


def broadcast_addresses():
    """The limited broadcast plus every attached LAN's own broadcast address.

    255.255.255.255 leaves through one interface of the system's choosing, which on
    a machine with VPN or virtual-machine adapters is often not the one the LAN is on.
    """
    found = ["255.255.255.255"]
    try:
        adapters = ifaddr.get_adapters()
    except OSError:
        return found
    for adapter in adapters:
        for ip in adapter.ips:
            if not isinstance(ip.ip, str) or ip.network_prefix >= 31:
                continue
            network = ipaddress.IPv4Network(f"{ip.ip}/{ip.network_prefix}", strict=False)
            address = str(network.broadcast_address)
            if not (network.is_loopback or network.is_link_local) and address not in found:
                found.append(address)
    return found


class _Beacon(asyncio.DatagramProtocol):
    def __init__(self, cluster):
        self.cluster = cluster

    def datagram_received(self, data, addr):
        self.cluster.on_beacon(data, addr)

    def error_received(self, _error):
        pass            # a broadcast into a dead interface is not worth a log line


class Cluster:
    def __init__(self, hub):
        self.hub = hub
        self.peers = {}             # url -> peer dict
        self.lock = None
        self.refreshed = 0.0        # monotonic time of the last completed round
        self._refreshing = None     # the round in flight, shared by everyone who asks
        self._http_slots = asyncio.Semaphore(4)
        self._http_tasks = set()     # slots stay occupied until the actual thread returns
        self._closing = False
        self._local_logged = float("-inf")
        self._log_generation = 0
        # One event per loop: something that wants them to run now rather than at
        # their own pace - a page opening, a beacon from a node we never saw -
        # sets both, and whichever loop wakes first must not swallow the other's.
        self.nudges = {"poll": asyncio.Event(), "beacon": asyncio.Event()}
        # Node names are remembered so a pinned node still has a name while it is down.
        self.known = hub.state.setdefault("nodes", {})
        self.node_id = hub.state.get("node_id")
        if not self.node_id:
            # config.json gets copied between machines; the identity must not.
            self.node_id = uuid.uuid4().hex[:12]
            hub.state["node_id"] = self.node_id
        self.known[self.node_id] = self.name
        for key in list(self.known):
            if len(self.known) <= MAX_KNOWN:
                break
            if key != self.node_id:
                del self.known[key]
        hub.save_state()

    # ---------------------------------------------------------------- identity

    @property
    def settings(self):
        return self.hub.config.get("node") or {}

    @property
    def name(self):
        return self.settings.get("name") or socket.gethostname()

    @property
    def enabled(self):
        """Off means one lonely machine: no peers, no beacon, no node endpoints."""
        return bool(self.settings.get("cluster", True))

    @property
    def discovering(self):
        return self.enabled and bool(self.settings.get("discovery", True))

    @property
    def self_view(self):
        return {"id": self.node_id, "name": self.name}

    def wake(self):
        """Tell the loops that waiting is over."""
        for event in self.nudges.values():
            event.set()

    async def _nap(self, who, seconds):
        """Sleep, unless somebody wakes us."""
        event = self.nudges[who]
        try:
            await asyncio.wait_for(event.wait(), seconds)
        except asyncio.TimeoutError:
            return
        event.clear()

    def manual_urls(self):
        port = self.hub.config["port"]
        found = []
        for entry in self.settings.get("peers") or []:
            url = normalize_peer(entry, port)
            if url and url not in found:
                found.append(url)
                if len(found) == MAX_PEERS:
                    break
        return found

    def remember(self, node_id, name):
        if (node_id and name and self.known.get(node_id) != name
                and (node_id in self.known or len(self.known) < MAX_KNOWN)):
            self.known[node_id] = name
            self.hub.save_state()

    def local_failure(self, error, url=None):
        now = time.monotonic()
        if now - self._local_logged >= 30:
            self._local_logged = now
            try:
                text = f"{url}: {error}" if url else str(error)
                self.hub.log("cluster.localResource", error=text[:512])
            except Exception:
                pass  # Reporting exhausted memory must not kill the poll loop.

    def clear_log_cache(self):
        self._log_generation += 1
        for peer in self.peers.values():
            peer.update(logs=[], log_boot=None, log_cursor=0)

    def node_names(self):
        return dict(self.known, **{self.node_id: self.name})

    def online(self):
        return [peer for peer in self.peers.values() if peer["online"]]

    def online_ids(self):
        return {self.node_id} | {peer["node_id"] for peer in self.online() if peer["node_id"]}

    # --------------------------------------------------------------- discovery

    def touch(self, url, origin):
        peer = self.peers.get(url)
        if peer is None:
            if len(self.peers) >= MAX_PEERS:
                candidates = [p for p in self.peers.values() if p["origin"] == "beacon"]
                if origin != "manual" or not candidates:
                    return None
                oldest = min(candidates, key=lambda p: p["last_beacon"])
                del self.peers[oldest["url"]]
            peer = {"url": url, "origin": origin, "node_id": None, "name": None,
                    "last_beacon": 0.0, "probed": 0.0, "attempted": 0.0,
                    "down_since": time.monotonic(), "online": False, "state": None,
                    "error": None, "logs": [], "log_boot": None, "log_cursor": 0}
            self.peers[url] = peer
        elif origin == "manual":
            # A hand-typed peer outlives its beacons, so the stricter origin wins.
            peer["origin"] = "manual"
        return peer

    def hear(self, host, port):
        """Register whoever just probed us, the same way a beacon would.

        Our broadcast does not always reach the other side - a NAS sends a limited
        broadcast out of one interface only - and a peer typed in by hand is easy to
        enter on one page and forget on the other. Either way the pair would see
        different owners, and the node left in the dark would not be asked for the
        lock before its radio was used. One probe in either direction is enough to
        close that gap.
        """
        try:
            number = int(port)
        except (TypeError, ValueError):
            return
        url = normalize_peer(host, number)
        if not url:
            return
        peer = self.touch(url, "beacon")
        if peer is None:
            return
        # Counts as a beacon for expiry too, or the next refresh would drop it as
        # noise before it had been probed once.
        peer["last_beacon"] = time.monotonic()
        if not peer["online"]:
            self._heard(peer)

    def _heard(self, peer):
        """It just spoke to us, so it is up: whatever backoff it was in is over,
        and the loop should go and talk to it now rather than at its next turn."""
        peer["attempted"] = 0.0
        self.wake()

    def on_beacon(self, data, addr):
        if len(data) > 1024:
            return
        try:
            payload = bounded_json(data)
            if not isinstance(payload, dict) or payload.get("id") == self.node_id:
                return
            text_value(payload.get("id"), 64, nonempty=True)
            text_value(payload.get("name"), 64, nonempty=True)
        except LocalFailure:
            return
        port = payload.get("port")
        if type(port) is not int or not 1 <= port <= 65535:
            return
        url = f"http://{addr[0]}:{port}"
        peer = self.peers.get(url)
        # A node we have never seen, and one we had given up on, are both worth
        # probing right now: the second is how a machine that was switched off gets
        # back on the page before its backoff would have noticed.
        news = peer is None or not peer["online"]
        peer = self.touch(url, "beacon")
        if peer is None:
            return
        peer["last_beacon"] = time.monotonic()
        if not peer["online"]:
            peer["name"] = payload["name"]
        if news:
            # Both sides may be announcing once a minute, which would make a new
            # node take that long to appear. Answering a stranger immediately
            # keeps discovery quick without a fast beacon.
            self._heard(peer)

    def _announce(self, transport):
        payload = json.dumps({"id": self.node_id, "name": self.name,
                              "port": int(self.hub.config["port"])}).encode("utf-8")
        for address in broadcast_addresses():
            try:
                transport.sendto(payload, (address, BEACON_PORT))
            except OSError:
                pass

    async def beacon(self):
        """Announce ourselves, and listen for the others, on one broadcast socket."""
        loop = asyncio.get_running_loop()
        transport = None
        blocked = False
        try:
            while True:
                if not self.discovering:
                    if transport is not None:
                        # Give the port back, so the switch really means "off".
                        transport.close()
                        transport = None
                    await self._nap("beacon", POLL_IDLE)
                    continue
                if transport is None:
                    try:
                        transport, _protocol = await loop.create_datagram_endpoint(
                            lambda: _Beacon(self), local_addr=("0.0.0.0", BEACON_PORT),
                            allow_broadcast=True)
                        blocked = False
                    except OSError as error:
                        # Discovery is a convenience; the hand-typed list still works.
                        # Say so once: a port somebody else holds will still be theirs
                        # next time, and a plain sleep here means an opening page does
                        # not drag us through the same failing bind every two seconds.
                        if not blocked:
                            self.hub.log("cluster.beaconFailed", port=BEACON_PORT,
                                         error=str(error))
                            blocked = True
                        await asyncio.sleep(BEACON_SLOW)
                        continue
                self._announce(transport)
                await self._nap("beacon", BEACON_FAST if self.hub.watched else BEACON_SLOW)
        finally:
            if transport is not None:
                transport.close()

    async def poll(self):
        while True:
            try:
                await self.refresh()
            except Exception as error:
                self.local_failure(error)
            await self._nap("poll", self.cadence())

    async def close(self):
        self._closing = True
        if self._refreshing is not None:
            self._refreshing.cancel()
            await asyncio.gather(self._refreshing, return_exceptions=True)
        # urllib/DNS threads cannot be killed. Their bounded slots are released by
        # completion callbacks, never by cancelling their asyncio waiter.

    def cadence(self):
        if not self.enabled:
            return POLL_IDLE
        if self.hub.watched:
            return POLL_WATCHED
        # Nothing to ask and nobody asking: the beacon socket is what finds a peer,
        # and that wakes this loop, so there is no reason to keep knocking.
        return POLL_IDLE if self.peers else POLL_ALONE

    def _due(self, peer, now):
        """Is this peer worth the wait right now?

        A node that has just gone quiet is chased at full speed, because a restart
        is the usual reason and it will be back in seconds. One that has been gone
        longer is not: the attempt blocks for STATE_TIMEOUT and tells us what we
        already know.
        """
        if peer["online"] or now - peer["down_since"] <= DOWN_GRACE:
            return True
        return now - peer["attempted"] >= DOWN_BACKOFF

    def refresh(self):
        """Probe the peers, joining a round already under way instead of starting a
        second one: the poll loop, a page and the scheduler can all ask at once."""
        if self._refreshing is None or self._refreshing.done():
            self._refreshing = asyncio.ensure_future(self._refresh())
        # A browser that gives up must not cancel the round the others are waiting on.
        return asyncio.shield(self._refreshing)

    async def _refresh(self):
        now = time.monotonic()
        if not self.hub.watched:
            self.clear_log_cache()
        if not self.enabled:
            self.peers.clear()
            self.refreshed = now
            return
        manual = set(self.manual_urls())
        for peer in self.peers.values():
            if peer["origin"] == "manual" and peer["url"] not in manual:
                peer["origin"] = "beacon"
        for url in manual:
            self.touch(url, "manual")
        for url, peer in list(self.peers.items()):
            # Existing sightings keep ownership during repair; removed manual
            # entries now obey exactly the same TTL as discovered entries.
            if (peer["origin"] == "beacon" and now - peer["last_beacon"] > BEACON_TTL
                    and (not peer["probed"] or now - peer["probed"] > SIGHTING_TTL)):
                del self.peers[url]
        await asyncio.gather(*(self._probe(peer) for peer in list(self.peers.values())
                               if self._due(peer, now)))
        self.refreshed = time.monotonic()

    async def ensure_fresh(self):
        """Probe now unless we just did. Every action goes through here, so the
        poll cadence is free to be as slow as it likes."""
        if time.monotonic() - self.refreshed > FRESH_ENOUGH:
            await self.refresh()

    async def _probe(self, peer):
        peer["attempted"] = time.monotonic()
        path = f"/api/node/state?port={int(self.hub.config['port'])}"
        try:
            try:
                answer = await self.get(peer["url"], path)
            except Exception as error:
                # One reply lost on a busy radio - an ESP32 shares its antenna with
                # its BLE scan - is not a node going away; ask once more first.
                if not (peer["online"] and network_failure(error)):
                    raise
                answer = await self.get(peer["url"], path)
            state = validate_state(answer)
            previous = peer["state"]
            if (previous and state["node"]["id"] == previous["node"]["id"]
                    and state["log_boot"] == previous["log_boot"]
                    and state["log_cursor"] < previous["log_cursor"]):
                raise LocalFailure("Log cursor regressed without a new epoch")
        except Exception as error:
            # A node whose cluster switch is off has left; it is not misbehaving.
            left = (isinstance(error, Rejected) and isinstance(error.detail, dict)
                    and error.detail.get("event") == "error.clusterOff")
            if left or network_failure(error):
                self._lost(peer, error)
            else:
                self.local_failure(error, peer["url"])
            return

        node = state["node"]
        if node["id"] == self.node_id:
            self.peers.pop(peer["url"], None)
            return
        # Never commit even the first device from a malformed list. Validation
        # above covers every retained field before any cache or record is changed.
        try:
            self.adopt(state)
            self.remember(node["id"], node["name"])
        except Exception as error:
            self.local_failure(error, peer["url"])
            return
        was_online = peer["online"]
        identity_changed = peer["node_id"] != node["id"]
        peer.update(node_id=node["id"], name=node["name"], online=True, error=None,
                    state=state, probed=time.monotonic())
        if identity_changed:
            peer.update(logs=[], log_boot=None, log_cursor=0)
        for other in list(self.peers.values()):
            if (other is not peer and other["node_id"] == node["id"] and not other["online"]
                    and other["origin"] != "manual"):
                self.peers.pop(other["url"], None)  # its old address; see _lost
        if not was_online:
            self.hub.log("cluster.peerUp", node=peer["name"], url=peer["url"])
        if not self.hub.watched:
            self.clear_log_cache()
            return
        boot, head = state["log_boot"], state["log_cursor"]
        # Multiple addresses may point at one node: keep one shared sixty-entry
        # history, not another cache and download for every address.
        aliases = [other for other in self.peers.values()
                   if other["node_id"] == node["id"] and other["log_boot"] == boot]
        if aliases:
            cached = max(aliases, key=lambda other: other["log_cursor"])
            peer.update(logs=cached["logs"], log_boot=boot, log_cursor=cached["log_cursor"])
        if (boot, head) == (peer["log_boot"], peer["log_cursor"]):
            return
        generation = self._log_generation
        after = peer["log_cursor"] if peer["log_boot"] == boot else 0
        requested_boot = peer["log_boot"] or ""
        try:
            source = validate_logs(await self.get(peer["url"],
                f"/api/node/logs?boot={requested_boot}&after={after}"),
                node["id"], node["name"], boot, after if after <= head else 0)
            if generation != self._log_generation or not self.hub.watched:
                return
            previous = peer["logs"] if peer["log_boot"] == boot else []
            by_id = {entry["id"]: entry for entry in previous}
            by_id.update((entry["id"], entry) for entry in source["logs"])
            history = [by_id[key] for key in sorted(by_id)[-LOG_CACHE:]]
            cursor = max(source["cursor"], peer["log_cursor"] if peer["log_boot"] == boot else 0)
            for other in self.peers.values():
                if other["node_id"] == node["id"]:
                    other.update(logs=history, log_boot=boot, log_cursor=cursor)
        except Exception as error:
            # A log download failing cannot undo a successful state probe.
            self.local_failure(error, peer["url"])

    def _reachable_elsewhere(self, peer):
        return bool(peer["node_id"]) and any(
            other is not peer and other["online"] and other["node_id"] == peer["node_id"]
            for other in self.peers.values())

    def _lost(self, peer, error):
        elsewhere = self._reachable_elsewhere(peer)
        if elsewhere and peer["origin"] != "manual":
            # The node moved to another address (a port or IP change), so this
            # record is stale rather than down; keeping it would list the node twice.
            self.peers.pop(peer["url"], None)
            return
        # A node that refuses because its cluster switch is off is not broken, so
        # pass its own words on and let the page say so in the reader's language.
        said = error.detail if isinstance(error, Rejected) and isinstance(
            error.detail, dict) else None
        if peer["online"]:
            peer["down_since"] = time.monotonic()
            self.hub.log("cluster.peerDown", node=peer["name"] or peer["url"],
                         error=said["event"] if said else str(error))
        # The state is kept: a node that is down still has the devices it is nearest
        # to, and forgetting that would hand them to a radio that can barely hear them.
        peer.update(online=False, error=said or str(error))
        # A node that holds our lock and then disappears would otherwise block
        # everyone until the TTL expires.
        if (not elsewhere and self.lock and self.lock["holder_id"] == peer["node_id"]
                and time.monotonic() - peer["down_since"] > DOWN_GRACE):
            self.hub.log("cluster.lockDropped", node=self.lock["holder_name"])
            self.lock = None

    def adopt(self, state):
        """Take over the fields a peer may have edited more recently than we did."""
        changed = False
        for view in state.get("devices") or []:
            key = view.get("key")
            if not key:
                continue
            existing = self.hub.devices.get(key)
            if existing is None and len(self.hub.devices) >= MAX_DEVICES:
                continue  # Never prune the user's existing persistent records.
            record = dict(existing) if existing is not None else {
                "auto": True, "address": view.get("address") or key}
            updated = existing is None
            for field, stamp in (("label", "label_at"), ("auto", "auto_at"),
                                 ("owner_pin", "pin_at")):
                theirs = view.get(stamp) or 0
                if theirs > (record.get(stamp) or 0):
                    record[field] = view.get(field)
                    record[stamp] = theirs
                    updated = True
            if updated:
                self.hub.devices[key] = record
                changed = True
        if changed:
            self.hub.save_devices()

    # ------------------------------------------------------------------- HTTP

    async def get(self, url, path, timeout=STATE_TIMEOUT):
        return await self._http(url + path, None, timeout)

    async def post(self, url, path, payload=None, timeout=ACTION_TIMEOUT):
        return await self._http(url + path, payload or {}, timeout)

    async def _http(self, target, payload, timeout):
        if self._closing:
            raise LocalFailure("HTTP client shutting down")
        deadline = time.monotonic() + timeout
        try:
            await asyncio.wait_for(self._http_slots.acquire(), timeout)
        except TimeoutError as error:
            raise LocalFailure("HTTP inflight limit") from error
        task = None
        try:
            if self._closing or time.monotonic() >= deadline:
                raise LocalFailure("HTTP admission deadline")
            task = asyncio.create_task(asyncio.to_thread(
                self._request, target, payload, timeout, deadline=deadline))
            task.add_done_callback(self._http_done)
            self._http_tasks.add(task)
            done, _ = await asyncio.wait({task}, timeout=max(0.001, deadline - time.monotonic())
                                         + HTTP_DIAGNOSIS_GRACE)
            if not done:
                # Executor/DNS/OS scheduling may still own the thread. Without a
                # completed transport diagnosis this timeout is unknown, not lost.
                raise LocalFailure("HTTP deadline without transport diagnosis")
            return task.result()
        except MemoryError as error:
            raise LocalFailure("HTTP local memory failure") from error
        finally:
            if task is None:
                self._http_slots.release()

    def _http_done(self, task):
        self._http_tasks.discard(task)
        self._http_slots.release()
        if not task.cancelled():
            task.exception()  # Retrieve errors even when its caller was cancelled.

    @staticmethod
    def _read_response(response, deadline):
        length = response.headers.get("Content-Length")
        if length is not None:
            try:
                if not 0 <= int(length) <= MAX_RESPONSE:
                    raise LocalFailure("HTTP response size limit")
            except ValueError as error:
                raise LocalFailure("Invalid HTTP Content-Length") from error
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise NetworkFailure("HTTP read deadline")
            # urllib's default timeout applies per read. Re-arm the actual socket
            # so a slow drip cannot restart the total deadline on each chunk.
            fp = getattr(response, "fp", None)
            raw = getattr(fp, "raw", fp)
            sock = getattr(raw, "_sock", None)
            if sock is not None:
                sock.settimeout(remaining)
            read = getattr(response, "read1", response.read)
            chunk = read(min(4096, MAX_RESPONSE + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > MAX_RESPONSE:
                raise LocalFailure("HTTP response size limit")
        if time.monotonic() > deadline:
            raise NetworkFailure("HTTP total deadline")
        result = bounded_json(data)
        if time.monotonic() > deadline:
            raise LocalFailure("HTTP decode deadline")
        return result

    @staticmethod
    def _request(target, payload, timeout, deadline=None):
        deadline = time.monotonic() + timeout if deadline is None else deadline
        try:
            if time.monotonic() >= deadline:
                raise LocalFailure("HTTP executor admission deadline")
            body = None if payload is None else json.dumps(payload, allow_nan=False).encode("utf-8")
            if body is not None and len(body) > MAX_RESPONSE:
                raise LocalFailure("HTTP request size limit")
            request = urllib.request.Request(
                target, data=body,
                headers={"Content-Type": "application/json"} if body is not None else {})
            try:
                with _opener(deadline).open(request, timeout=max(0.001, deadline - time.monotonic())) as response:
                    return Cluster._read_response(response, deadline)
            except urllib.error.HTTPError as error:
                # HTTPError owns a response stream too, including on parse failure.
                try:
                    parsed = Cluster._read_response(error, deadline)
                    if not isinstance(parsed, dict):
                        raise LocalFailure("Invalid HTTP error object")
                    detail = parsed.get("detail")
                    raise Rejected(error.code, detail or f"HTTP {error.code}") from None
                except Rejected:
                    raise
                except Exception as failure:
                    raise LocalFailure("HTTP error body unavailable") from failure
                finally:
                    try:
                        error.close()
                    except Exception as failure:
                        raise LocalFailure("HTTP error close failed") from failure
        except (LocalFailure, Rejected, NetworkFailure):
            raise
        except Exception as error:
            if network_failure(error):
                raise NetworkFailure(str(error)) from error
            raise LocalFailure("HTTP local or unknown failure: " + type(error).__name__) from error

    # ------------------------------------------------------------------- lock

    def _held(self):
        if self.lock and time.monotonic() - self.lock["at"] > LOCK_TTL:
            self.hub.log("cluster.lockExpired", node=self.lock["holder_name"])
            self.lock = None
        return self.lock

    def _take(self, token, holder_id, holder_name, op):
        if self._held() or self.hub.pending:
            return False
        self.lock = {"token": token, "holder_id": holder_id, "holder_name": holder_name,
                     "op": op, "at": time.monotonic()}
        return True

    def drop(self, token):
        if self.lock and self.lock["token"] == token:
            self.lock = None

    def holds(self, token):
        held = self._held()
        return bool(token and held and held["token"] == token)

    def grant(self, body):
        """Answer a peer's claim. Never raises: a refusal is data, not an error."""
        held = self._held()
        if held and body.get("token") and held["token"] == body["token"]:
            held["at"] = time.monotonic()        # the holder renewing a long job
            return {"granted": True, "holder": None}
        taken = self._take(body.get("token"), body.get("holder_id"),
                           body.get("holder_name"), body.get("op"))
        if taken:
            return {"granted": True, "holder": None}
        held = self._held()
        return {"granted": False, "holder": held["holder_name"] if held else self.name}

    async def acquire(self, op):
        """Hold the whole cluster still. Returns a token to pass to release()."""
        # The peer list may be a few minutes old when nobody is watching, and a
        # node missing from it would not be asked for the lock.
        await self.ensure_fresh()
        for attempt in (0, 1):
            token = uuid.uuid4().hex
            blocker = None
            if self._take(token, self.node_id, self.name, op):
                succeeded = False
                claims = None
                targets = []
                try:
                    targets = list(self.online())
                    claims = asyncio.create_task(self._claim_peers(token, op, targets))
                    blocker = await asyncio.shield(claims)
                    if blocker is None:
                        succeeded = True
                        return token
                finally:
                    if not succeeded:
                        self.drop(token)

                        async def rollback():
                            # Do not race unlock ahead of a still-running claim.
                            if claims is not None:
                                await asyncio.gather(claims, return_exceptions=True)
                            await self._release_peers(token, targets)

                        await finish_cleanup(rollback())
            else:
                held = self._held()
                blocker = held["holder_name"] if held else self.name
            if not attempt:
                # Two nodes can refuse each other at the same instant; let one win.
                await asyncio.sleep(random.uniform(0.1, 0.4))
                continue
            self.hub.log("cluster.lockRefused", node=blocker)
            raise Busy(blocker)

    async def _claim_peers(self, token, op, targets=None):
        targets = self.online() if targets is None else targets
        if not targets:
            return None
        payload = {"token": token, "holder_id": self.node_id,
                   "holder_name": self.name, "op": op}
        answers = await asyncio.gather(
            *(self.post(peer["url"], "/api/node/lock", payload, STATE_TIMEOUT)
              for peer in targets), return_exceptions=True)
        for peer, answer in zip(targets, answers):
            if isinstance(answer, BaseException):
                if network_failure(answer):
                    continue
                self.local_failure(answer, peer["url"])
                return peer["name"] or peer["url"]
            try:
                if (not isinstance(answer, dict) or set(answer) != {"granted", "holder"}
                        or type(answer["granted"]) is not bool):
                    raise LocalFailure("Invalid lock answer")
                text_value(answer["holder"], 64, nullable=True)
            except LocalFailure as error:
                self.local_failure(error, peer["url"])
                return peer["name"] or peer["url"]
            if not answer["granted"]:
                return answer["holder"] or peer["name"] or peer["url"]
        return None

    async def _release_peers(self, token, targets=None):
        await asyncio.gather(
            *(self.post(peer["url"], "/api/node/unlock", {"token": token}, STATE_TIMEOUT)
              for peer in (self.online() if targets is None else targets)), return_exceptions=True)

    async def release(self, token):
        self.drop(token)
        await self._release_peers(token)

    async def keep(self, token):
        """Renew the lock until cancelled.

        LOCK_TTL is there for a holder that crashed, not for one that is merely
        slow: a scheduled run over many devices with retries easily outlasts it,
        and a peer whose copy expired would start its own radio in the middle.
        """
        while True:
            await asyncio.sleep(LOCK_TTL / 3)
            if not self.holds(token):
                return
            self.lock["at"] = time.monotonic()
            # The same claim again: a peer still holding it renews, one that came
            # online after we started takes it now.
            await self._claim_peers(token, self.lock["op"])

    def lock_view(self):
        held = self._held()
        if not held:
            return None
        return {"holder": held["holder_name"], "holder_id": held["holder_id"],
                "op": held["op"], "mine": held["holder_id"] == self.node_id}

    # ------------------------------------------------------- merge and ownership

    def nodes_view(self, local, merged):
        owned = {}
        for device in merged:
            owned[device["node_id"]] = owned.get(device["node_id"], 0) + 1
        rows = [dict(self.self_view, url=None, origin="self", own=True, online=True,
                     busy=local.get("busy"), queued=local.get("queued", 0), error=None,
                     devices=owned.get(self.node_id, 0))]
        for peer in sorted(self.peers.values(), key=lambda item: item["url"]):
            state = peer["state"] or {}
            rows.append({"id": peer["node_id"], "name": peer["name"] or peer["url"],
                         "url": peer["url"], "origin": peer["origin"], "own": False,
                         "online": peer["online"],
                         # A queue read off a node that is down is just an old number.
                         "busy": state.get("busy") if peer["online"] else None,
                         "queued": state.get("queued", 0) if peer["online"] else 0,
                         "error": peer["error"],
                         "devices": owned.get(peer["node_id"], 0) if peer["node_id"] else 0})
        return rows

    def sources(self, local_views):
        """Every node's device list, including nodes that are currently down."""
        yield self.node_id, self.name, local_views
        for peer in self.peers.values():
            if peer["state"] and peer["node_id"]:
                yield peer["node_id"], peer["name"], peer["state"].get("devices") or []

    def merge(self, local_views):
        """One row per device, from however many nodes happen to know about it."""
        slots = {}
        for node_id, node_name, views in self.sources(local_views):
            for view in views:
                slot = slots.setdefault(view["key"], {"views": [], "sightings": {}})
                slot["views"].append((node_id, view))
                if view.get("rssi") is not None:
                    slot["sightings"][node_id] = {
                        "node_id": node_id, "node": node_name,
                        "rssi": view["rssi"], "seen_at": view.get("seen_at")}
        context = (self.node_names(), self.online_ids(), time.time())
        return [self._row(key, slots[key], *context) for key in sorted(slots)]

    @staticmethod
    def _activity(view):
        return max(view.get("updated_at") or 0, view.get("error_at") or 0)

    def _row(self, key, slot, names, online, now):
        # Whatever happened most recently, anywhere, is what the row should show.
        _, base = max(slot["views"], key=lambda item: self._activity(item[1]))
        device = dict(base, key=key)

        for field, stamp in (("label", "label_at"), ("auto", "auto_at"),
                             ("owner_pin", "pin_at")):
            # Records written before this version carry no stamp at all, so a value
            # beats a blank at the same timestamp instead of the sources' order.
            _, newest = max(slot["views"], key=lambda item: (item[1].get(stamp) or 0,
                                                             item[1].get(field) is not None))
            device[field] = newest.get(field)
            device[stamp] = newest.get(stamp)
        if device["auto"] is None:
            device["auto"] = True

        owner = self.owner(slot, device["owner_pin"], now)
        device["node_id"] = owner
        device["node"] = names.get(owner)
        device["node_online"] = owner in online
        # Only a node that heard it lately can be offered as a pin: anything else
        # would just fail to connect.
        device["sightings"] = sorted(
            (dict(heard, fresh=now - (heard["seen_at"] or 0) < SIGHTING_TTL)
             for heard in slot["sightings"].values()), key=lambda s: -s["rssi"])
        # Only a running node can still be working on it.
        device["pending"] = any(view.get("pending") for node_id, view in slot["views"]
                                if node_id in online)

        heard = slot["sightings"].get(owner)
        if heard:
            device["rssi"] = heard["rssi"]
            device["seen_at"] = heard["seen_at"]
        return device

    def owner(self, slot, pin, now):
        """Nearest node wins. A pin is absolute, so a stubborn device can be placed."""
        if pin:
            return pin
        fresh = [heard for heard in slot["sightings"].values()
                 if now - (heard["seen_at"] or 0) < SIGHTING_TTL]
        if fresh:
            # Quantising the signal first means every node computes the same owner and
            # a peer has to be clearly closer, not one noisy dBm closer, to take over.
            return min(fresh, key=lambda heard: (-round(heard["rssi"] / BUCKET),
                                                 heard["node_id"]))["node_id"]
        return min(slot["views"], key=lambda item: (-self._activity(item[1]), item[0]))[0]

    def my_share(self, merged):
        """(mine, taken_over): what this node should sync on a scheduled run."""
        online = self.online_ids()
        now = time.time()
        mine, taken = [], []
        for device in merged:
            if not device.get("auto", True):
                continue
            if device["node_id"] == self.node_id:
                mine.append(device["key"])
            elif (not device["owner_pin"] and device["node_id"] not in online
                    and now - self._activity(device) > TAKEOVER_AFTER
                    and any(heard["node_id"] == self.node_id
                            for heard in device["sightings"])):
                # The owner has been gone for a while and this radio can still hear
                # the device, so cover for it rather than let the clock drift.
                taken.append(device["key"])
        return mine, taken

    def my_work(self, merged):
        """Could a scheduled run here have anything to do at all?

        A node that owns nothing would otherwise still take the cluster lock and
        run a 25 second scan at every slot, only to hand every device to someone
        else. Skipping that is safe except in one case: if no device on the list
        has ever been heard by this radio, the scan is the only thing that could
        tell us whether something nearby is ours, so it still runs.
        """
        mine, taken = self.my_share(merged)
        if mine or taken:
            return True
        return not any(heard["node_id"] == self.node_id
                       for device in merged if device.get("auto", True)
                       for heard in device["sightings"])
