"""Minimal DoIP/UDS tester (ISO 13400-2 / ISO 14229-1 subset) used by the Phase 5 scenario test.

Plays the Adaptive Diagnostic Client role (project.md §9.1) from the AP namespace. Not a production tester.
"""
from __future__ import annotations

import hashlib
import hmac
import socket
import struct
import time
from dataclasses import dataclass, field

VERSION = 0x02
T_GENERIC_NACK, T_VID_REQ, T_ANNOUNCE = 0x0000, 0x0001, 0x0004
T_RA_REQ, T_RA_RES = 0x0005, 0x0006
T_DIAG, T_DIAG_ACK, T_DIAG_NACK = 0x8001, 0x8002, 0x8003


def message(ptype: int, payload: bytes, version: int = VERSION) -> bytes:
    return struct.pack(">BBHI", version, version ^ 0xFF, ptype, len(payload)) + payload


def security_key(key: bytes, logical_address: int, seed: bytes) -> bytes:
    return hmac.new(key, logical_address.to_bytes(2, "big") + seed, hashlib.sha256).digest()


@dataclass
class UdsResult:
    ack: str = "none"              # "ack", "nack", "none"
    nack_code: int | None = None
    response: bytes | None = None  # final UDS response (after any 0x78)
    elapsed_ms: float = 0.0
    closed: bool = False
    raw: list = field(default_factory=list)

    @property
    def positive(self) -> bool:
        return self.response is not None and self.response[:1] != b"\x7f"

    def nrc(self) -> int | None:
        if self.response and len(self.response) == 3 and self.response[0] == 0x7F:
            return self.response[2]
        return None


def vehicle_identification(dest: str, port: int = 13400, timeout: float = 2.0) -> dict | None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    try:
        s.sendto(message(T_VID_REQ, b""), (dest, port))
        data, peer = s.recvfrom(512)
    except OSError:
        return None
    finally:
        s.close()
    _, _, ptype, plen = struct.unpack(">BBHI", data[:8])
    p = data[8:]
    if ptype != T_ANNOUNCE or plen < 32:
        return {"type": ptype}
    return {"type": ptype, "vin": p[:17].decode("ascii", "replace"), "logical_address": int.from_bytes(p[17:19], "big"),
            "eid": p[19:25].hex(":"), "gid": p[25:31].hex(":"), "further_action": p[31], "from": peer[0]}


class DoIPTester:
    def __init__(self, host: str, source_address: int = 0x0E80, port: int = 13400):
        self.sa = source_address
        self.sock = socket.create_connection((host, port), timeout=3)
        self.buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _recv_msg(self, timeout: float):
        """Returns (type, payload), or None on timeout, or ('closed', b'') if the peer closed."""
        end = time.monotonic() + timeout
        while True:
            if len(self.buf) >= 8:
                _, _, ptype, plen = struct.unpack(">BBHI", self.buf[:8])
                if len(self.buf) >= 8 + plen:
                    payload, self.buf = self.buf[8:8 + plen], self.buf[8 + plen:]
                    return ptype, payload
            left = end - time.monotonic()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            except OSError:
                return ("closed", b"")
            if not chunk:
                return ("closed", b"")
            self.buf += chunk

    def routing_activation(self, sa: int | None = None, activation_type: int = 0x00) -> tuple[int | None, bool]:
        """Returns (response code, connection closed)."""
        sa = self.sa if sa is None else sa
        self.sock.sendall(message(T_RA_REQ, struct.pack(">HB4x", sa, activation_type)))
        m = self._recv_msg(3)
        code = m[1][4] if m and m[0] == T_RA_RES else None
        closed = self._recv_msg(0.3) == ("closed", b"")
        return code, closed

    def request(self, target: int, uds: bytes, p2_client_s: float = 6.0) -> UdsResult:
        """Send one UDS request; wait for the DoIP ack/nack and then the UDS response (P2* client timer)."""
        r = UdsResult()
        t0 = time.monotonic()
        try:
            self.sock.sendall(message(T_DIAG, struct.pack(">HH", self.sa, target) + uds))
        except OSError:
            r.closed = True
            return r
        while True:
            m = self._recv_msg(max(0.0, t0 + p2_client_s - time.monotonic()))
            if m is None:
                break
            if m[0] == "closed":
                r.closed = True
                break
            ptype, p = m
            r.raw.append((ptype, p.hex()))
            if ptype == T_DIAG_ACK:
                r.ack = "ack"
            elif ptype == T_DIAG_NACK:
                r.ack, r.nack_code = "nack", p[4]
                # an invalid source address also closes the connection
                if self._recv_msg(0.3) == ("closed", b""):
                    r.closed = True
                break
            elif ptype == T_DIAG and int.from_bytes(p[:2], "big") == target:
                rsp = p[4:]
                if len(rsp) == 3 and rsp[0] == 0x7F and rsp[2] == 0x78:
                    continue  # response pending
                r.response = rsp
                break
            elif ptype == T_GENERIC_NACK:
                r.ack, r.nack_code = "generic_nack", p[0]
                break
        r.elapsed_ms = (time.monotonic() - t0) * 1000
        return r

    def unlock(self, target: int, key: bytes) -> tuple[UdsResult, UdsResult]:
        seed = self.request(target, b"\x27\x01")
        if not seed.positive:
            return seed, seed
        return seed, self.request(target, b"\x27\x02" + security_key(key, target, seed.response[2:18]))
