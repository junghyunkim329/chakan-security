"""Reference PDU codec in Python. Must produce the same bytes as src/common/pdu_codec.c."""
from __future__ import annotations

from dataclasses import dataclass, field

from .definitions import Definitions, Pdu, Signal

# Status names match sil_status_t in src/common/include/sil/pdu_codec.h
OK = "OK"
ERR_UNKNOWN_ID = "ERR_UNKNOWN_ID"
ERR_DLC = "ERR_DLC"
ERR_CRC = "ERR_CRC"
ERR_RESERVED = "ERR_RESERVED"
ERR_NOT_VALID = "ERR_NOT_VALID"
ERR_RANGE = "ERR_RANGE"
ERR_COUNTER_DUPLICATE = "ERR_COUNTER_DUPLICATE"
ERR_COUNTER_JUMP = "ERR_COUNTER_JUMP"
ERR_FRAME_FORMAT = "ERR_FRAME_FORMAT"


def crc8(data: bytes, poly: int = 0x1D, init: int = 0xFF, xor_out: int = 0xFF) -> int:
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc ^ xor_out


def frame_crc(defs: Definitions, can_id: int, payload: bytes) -> int:
    p = defs.protection
    return crc8(can_id.to_bytes(2, "big") + bytes(payload[:7]), p.crc_poly, p.crc_init, p.crc_xor_out)


def _put(sig: Signal, raw: int, buf: bytearray) -> None:
    if sig.type == "flag":
        if raw:
            buf[sig.byte] |= 1 << sig.bit
    elif sig.type in ("u8", "i8"):
        buf[sig.byte] = raw & 0xFF
    else:
        buf[sig.byte:sig.byte + 2] = (raw & 0xFFFF).to_bytes(2, "big")


def _get(sig: Signal, buf: bytes) -> int:
    if sig.type == "flag":
        return (buf[sig.byte] >> sig.bit) & 1
    if sig.type == "u8":
        return buf[sig.byte]
    if sig.type == "i8":
        return int.from_bytes(buf[sig.byte:sig.byte + 1], "big", signed=True)
    return int.from_bytes(buf[sig.byte:sig.byte + 2], "big", signed=sig.type == "i16be")


def encode(defs: Definitions, pdu: Pdu, raw: dict[str, int], counter: int,
           valid: bool = True, degraded: bool = False, fault: bool = False) -> bytes:
    p = defs.protection
    if not 0 <= counter < (1 << p.counter_bits):
        raise ValueError("counter out of range")
    buf = bytearray(8)
    for sig in pdu.signals:
        v = raw[sig.name]
        if not sig.raw_min <= v <= sig.raw_max:
            raise ValueError(f"{pdu.name}.{sig.name}: raw {v} out of range")
        _put(sig, v, buf)
    buf[p.flags_byte] = ((counter << p.counter_shift)
                         | (valid << p.valid_bit) | (degraded << p.degraded_bit) | (fault << p.fault_bit))
    buf[p.crc_byte] = frame_crc(defs, pdu.can_id, buf)
    return bytes(buf)


@dataclass
class Decoded:
    status: str
    pdu: Pdu | None = None
    raw: dict[str, int] = field(default_factory=dict)
    counter: int = 0
    valid: bool = False
    degraded: bool = False
    fault: bool = False


def unpack(defs: Definitions, can_id: int, data: bytes) -> Decoded:
    """Stateless structure check: ID -> DLC -> CRC -> reserved bits."""
    pdu = defs.by_id(can_id)
    if pdu is None:
        return Decoded(ERR_UNKNOWN_ID)
    if len(data) != pdu.dlc:
        return Decoded(ERR_DLC, pdu)
    p = defs.protection
    if data[p.crc_byte] != frame_crc(defs, can_id, data):
        return Decoded(ERR_CRC, pdu)
    if any(data[i] & m for i, m in enumerate(pdu.reserved_mask)) or data[p.flags_byte] & (1 << p.reserved_bit):
        return Decoded(ERR_RESERVED, pdu)
    f = data[p.flags_byte]
    return Decoded(
        OK, pdu, {s.name: _get(s, data) for s in pdu.signals},
        counter=(f >> p.counter_shift) & ((1 << p.counter_bits) - 1),
        valid=bool(f >> p.valid_bit & 1), degraded=bool(f >> p.degraded_bit & 1), fault=bool(f >> p.fault_bit & 1),
    )


def check_values(d: Decoded) -> str:
    """Validity flag, then physical range."""
    if not d.valid:
        return ERR_NOT_VALID
    for s in d.pdu.signals:
        if not s.raw_min <= d.raw[s.name] <= s.raw_max:
            return ERR_RANGE
    return OK


class CounterChecker:
    """Per-PDU receive counter state (project.md §5.4)."""

    def __init__(self, max_delta: int, modulo: int = 16):
        self.max_delta = max_delta
        self.modulo = modulo
        self.last: int | None = None

    def check(self, counter: int) -> tuple[str, int]:
        if self.last is None:
            self.last = counter
            return OK, 0
        delta = (counter - self.last) % self.modulo
        if delta == 0:
            return ERR_COUNTER_DUPLICATE, 0
        self.last = counter
        if delta <= self.max_delta:
            return OK, delta - 1
        return ERR_COUNTER_JUMP, 0
