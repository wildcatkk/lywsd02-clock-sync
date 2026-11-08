"""A minimal SNTP client: how far this machine's clock is from a time server."""

import os
import socket
import struct
import time

NTP_EPOCH = 2208988800  # 1900-01-01 to 1970-01-01


def _stamp(data):
    seconds, fraction = struct.unpack("!II", data)
    return seconds - NTP_EPOCH + fraction / 2**32


def query(host, timeout=3.0):
    """Seconds to add to time.time() to get the server's time."""
    last = None
    # OS name resolution is blocking and cannot be cancelled by a socket timeout.
    addresses = socket.getaddrinfo(host, 123, type=socket.SOCK_DGRAM)
    deadline = time.monotonic() + timeout
    invalid = 0
    for family, kind, proto, _, address in addresses:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or invalid >= 16:
            last = last or socket.timeout("timed out")
            break
        try:
            with socket.socket(family, kind, proto) as sock:
                sock.settimeout(remaining)
                sock.connect(address)
                # Version 4, client mode. A random transmit stamp comes back as the
                # originate stamp, which ties the answer to this request.
                cookie = os.urandom(8)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise socket.timeout("timed out")
                sock.settimeout(remaining)
                sent = time.time()
                sock.send(b"\x23" + bytes(39) + cookie)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or invalid >= 16:
                        raise socket.timeout("timed out")
                    sock.settimeout(remaining)
                    reply = sock.recv(512)
                    received = time.time()
                    if len(reply) >= 48 and reply[24:32] == cookie:
                        break
                    invalid += 1
            leap, mode, stratum = reply[0] >> 6, reply[0] & 7, reply[1]
            if mode != 4 or not 1 <= stratum <= 15 or leap == 3:
                raise OSError(f"unsynchronised reply (stratum {stratum})")
            return ((_stamp(reply[32:40]) - sent) + (_stamp(reply[40:48]) - received)) / 2
        except OSError as error:
            last = error
    raise last or OSError(f"no address for {host}")
