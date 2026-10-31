"""Everything that makes one installation part of a cluster.

Nodes are equals: no master, no shared database. Each node owns the devices its own
radio hears best and learns about the rest from its peers over HTTP, so any page can
drive any device. One lock, granted by every node at once, keeps two people - or two
schedulers - from driving two radios at the same moment.
"""

import asyncio
import json
import random
import re
import socket
import time
import urllib.error
import urllib.request
import uuid

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
        self.refreshed = 0.0        # when the last round of probes finished
        self._refreshing = None     # the round in flight, shared by everyone who asks
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
        return found

    def remember(self, node_id, name):
        if node_id and name and self.known.get(node_id) != name:
            self.known[node_id] = name
            self.hub.save_state()

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
            peer = {"url": url, "origin": origin, "node_id": None, "name": None,
                    "last_beacon": 0.0, "probed": 0.0, "attempted": 0.0,
                    "down_since": time.time(), "online": False, "state": None,
                    "error": None}
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
        # Counts as a beacon for expiry too, or the next refresh would drop it as
        # noise before it had been probed once.
        peer["last_beacon"] = time.time()
        if not peer["online"]:
            self._heard(peer)

    def _heard(self, peer):
        """It just spoke to us, so it is up: whatever backoff it was in is over,
        and the loop should go and talk to it now rather than at its next turn."""
        peer["attempted"] = 0.0
        self.wake()

    def on_beacon(self, data, addr):
        try:
            payload = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return
        if not isinstance(payload, dict) or payload.get("id") == self.node_id:
            return
        port = payload.get("port")
        if not isinstance(port, int) or not 1 <= port <= 65535:
            return
        url = f"http://{addr[0]}:{port}"
        peer = self.peers.get(url)
        # A node we have never seen, and one we had given up on, are both worth
        # probing right now: the second is how a machine that was switched off gets
        # back on the page before its backoff would have noticed.
        news = peer is None or not peer["online"]
        peer = self.touch(url, "beacon")
        peer["last_beacon"] = time.time()
        if payload.get("name"):
            peer["name"] = str(payload["name"])[:24]
        if news:
            # Both sides may be announcing once a minute, which would make a new
            # node take that long to appear. Answering a stranger immediately
            # keeps discovery quick without a fast beacon.
            self._heard(peer)

    def _announce(self, transport):
        payload = json.dumps({"id": self.node_id, "name": self.name,
                              "port": int(self.hub.config["port"])})
        try:
            transport.sendto(payload.encode("utf-8"), ("255.255.255.255", BEACON_PORT))
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
            await self.refresh()
            await self._nap("poll", self.cadence())

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
        now = time.time()
        if not self.enabled:
            self.peers.clear()
            self.refreshed = now
            return
        for url in self.manual_urls():
            self.touch(url, "manual")
        for url, peer in list(self.peers.items()):
            # A beacon we never managed to talk to is noise and goes quickly. One we
            # did talk to is a real node, so it stays listed - as offline - long
            # enough to keep owning its devices while it is down for repair.
            if (peer["origin"] == "beacon" and now - peer["last_beacon"] > BEACON_TTL
                    and now - peer["probed"] > SIGHTING_TTL):
                del self.peers[url]
        await asyncio.gather(*(self._probe(peer) for peer in list(self.peers.values())
                               if self._due(peer, now)))
        self.refreshed = time.time()

    async def ensure_fresh(self):
        """Probe now unless we just did. Every action goes through here, so the
        poll cadence is free to be as slow as it likes."""
        if time.time() - self.refreshed > FRESH_ENOUGH:
            await self.refresh()

    async def _probe(self, peer):
        peer["attempted"] = time.time()
        try:
            # Our own port goes along, so the answering node can add us to its list
            # if it has not heard our broadcast. See hear().
            state = await self.get(
                peer["url"], f"/api/node/state?port={int(self.hub.config['port'])}")
            node = state["node"]
        except Exception as error:
            self._lost(peer, error)
            return

        if node.get("id") == self.node_id:
            # Our own address, typed into our own peer list. Harmless, but drop it.
            self.peers.pop(peer["url"], None)
            return

        was_online = peer["online"]
        peer.update(node_id=node.get("id"), name=node.get("name") or peer["url"],
                    online=True, error=None, state=state, probed=time.time())
        self.remember(peer["node_id"], peer["name"])
        if not was_online:
            self.hub.log("cluster.peerUp", node=peer["name"], url=peer["url"])
        self.adopt(state)

    def _lost(self, peer, error):
        # A node that refuses because its cluster switch is off is not broken, so
        # pass its own words on and let the page say so in the reader's language.
        said = error.detail if isinstance(error, Rejected) and isinstance(
            error.detail, dict) else None
        if peer["online"]:
            peer["down_since"] = time.time()
            self.hub.log("cluster.peerDown", node=peer["name"] or peer["url"],
                         error=said["event"] if said else str(error))
        # The state is kept: a node that is down still has the devices it is nearest
        # to, and forgetting that would hand them to a radio that can barely hear them.
        peer.update(online=False, error=said or str(error))
        # A node that holds our lock and then disappears would otherwise block
        # everyone until the TTL expires.
        if (self.lock and self.lock["holder_id"] == peer["node_id"]
                and time.time() - peer["down_since"] > DOWN_GRACE):
            self.hub.log("cluster.lockDropped", node=self.lock["holder_name"])
            self.lock = None

    def adopt(self, state):
        """Take over the fields a peer may have edited more recently than we did."""
        changed = False
        for view in state.get("devices") or []:
            key = view.get("key")
            if not key:
                continue
            if key not in self.hub.devices:
                self.hub.record(key)["address"] = view.get("address") or key
                changed = True
            record = self.hub.devices[key]
            for field, stamp in (("label", "label_at"), ("auto", "auto_at"),
                                 ("owner_pin", "pin_at")):
                theirs = view.get(stamp) or 0
                if theirs > (record.get(stamp) or 0):
                    record[field] = view.get(field)
                    record[stamp] = theirs
                    changed = True
        if changed:
            self.hub.save_devices()

    # ------------------------------------------------------------------- HTTP

    async def get(self, url, path, timeout=STATE_TIMEOUT):
        return await asyncio.to_thread(self._request, url + path, None, timeout)

    async def post(self, url, path, payload=None, timeout=ACTION_TIMEOUT):
        return await asyncio.to_thread(self._request, url + path, payload or {}, timeout)

    @staticmethod
    def _request(target, payload, timeout):
        body = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            target, data=body,
            headers={"Content-Type": "application/json"} if body is not None else {})
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read().decode("utf-8") or "{}")
        except urllib.error.HTTPError as error:
            detail = None
            try:
                detail = json.loads(error.read().decode("utf-8")).get("detail")
            except Exception:
                pass
            raise Rejected(error.code, detail or f"HTTP {error.code}") from None

    # ------------------------------------------------------------------- lock

    def _held(self):
        if self.lock and time.time() - self.lock["at"] > LOCK_TTL:
            self.hub.log("cluster.lockExpired", node=self.lock["holder_name"])
            self.lock = None
        return self.lock

    def _take(self, token, holder_id, holder_name, op):
        if self._held() or self.hub.pending:
            return False
        self.lock = {"token": token, "holder_id": holder_id, "holder_name": holder_name,
                     "op": op, "at": time.time()}
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
            held["at"] = time.time()        # the holder renewing a long job
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
                blocker = await self._claim_peers(token, op)
                if blocker is None:
                    return token
                await self._release_peers(token)
                self.drop(token)
            else:
                held = self._held()
                blocker = held["holder_name"] if held else self.name
            if not attempt:
                # Two nodes can refuse each other at the same instant; let one win.
                await asyncio.sleep(random.uniform(0.1, 0.4))
                continue
            self.hub.log("cluster.lockRefused", node=blocker)
            raise Busy(blocker)

    async def _claim_peers(self, token, op):
        targets = self.online()
        if not targets:
            return None
        payload = {"token": token, "holder_id": self.node_id,
                   "holder_name": self.name, "op": op}
        answers = await asyncio.gather(
            *(self.post(peer["url"], "/api/node/lock", payload, STATE_TIMEOUT)
              for peer in targets), return_exceptions=True)
        for peer, answer in zip(targets, answers):
            # An unreachable peer cannot be driving a radio we care about, and letting
            # it veto would mean one sleeping machine freezes the whole house.
            if isinstance(answer, Exception):
                continue
            if not answer.get("granted"):
                return answer.get("holder") or peer["name"]
        return None

    async def _release_peers(self, token):
        await asyncio.gather(
            *(self.post(peer["url"], "/api/node/unlock", {"token": token}, STATE_TIMEOUT)
              for peer in self.online()), return_exceptions=True)

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
            self.lock["at"] = time.time()
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
