"""Load and validate config/signals/can.yaml.

The C code generator and the Python reference codec both use this module, so the
YAML is interpreted in exactly one place.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_PATH = REPO_ROOT / "config" / "signals" / "can.yaml"

TYPE_RAW_LIMITS = {
    "u8": (0, 0xFF),
    "i8": (-0x80, 0x7F),
    "u16be": (0, 0xFFFF),
    "i16be": (-0x8000, 0x7FFF),
    "flag": (0, 1),
}
TYPE_BYTES = {"u8": 1, "i8": 1, "u16be": 2, "i16be": 2}
PAYLOAD_SIGNAL_BYTES = 6  # byte 0~5: signals, byte 6: counter/flags, byte 7: CRC


class DefinitionError(ValueError):
    pass


@dataclass(frozen=True)
class Signal:
    name: str
    byte: int
    type: str
    bit: int = 0
    scale: Decimal = Decimal(1)
    unit: str = ""
    raw_min: int = 0
    raw_max: int = 1

    def to_raw(self, phys) -> int:
        return int((Decimal(str(phys)) / self.scale).to_integral_value())

    def to_phys(self, raw: int) -> float:
        return float(Decimal(raw) * self.scale)


@dataclass(frozen=True)
class Pdu:
    can_id: int
    name: str
    sender: str
    period_ms: int
    dlc: int
    signals: tuple[Signal, ...]
    reserved_mask: tuple[int, ...] = field(default=(0,) * PAYLOAD_SIGNAL_BYTES)

    def signal(self, name: str) -> Signal:
        for s in self.signals:
            if s.name == name:
                return s
        raise KeyError(f"{self.name}: no signal {name}")


@dataclass(frozen=True)
class Protection:
    counter_byte: int
    counter_shift: int
    counter_bits: int
    flags_byte: int
    valid_bit: int
    degraded_bit: int
    fault_bit: int
    reserved_bit: int
    crc_byte: int
    crc_poly: int
    crc_init: int
    crc_xor_out: int
    counter_max_delta: int


@dataclass(frozen=True)
class Definitions:
    protection: Protection
    pdus: tuple[Pdu, ...]

    def by_id(self, can_id: int) -> Pdu | None:
        for p in self.pdus:
            if p.can_id == can_id:
                return p
        return None


def _raw_bound(name: str, value, scale: Decimal) -> int:
    q = Decimal(str(value)) / scale
    if q != q.to_integral_value():
        raise DefinitionError(f"{name}: {value} is not a multiple of scale {scale}")
    return int(q)


def _signal_bits(sig: Signal) -> list[tuple[int, int]]:
    if sig.type == "flag":
        return [(sig.byte, 1 << sig.bit)]
    return [(sig.byte + i, 0xFF) for i in range(TYPE_BYTES[sig.type])]


def _load_pdu(doc: dict) -> Pdu:
    name = doc["name"]
    can_id = int(doc["id"])
    if not 0 <= can_id <= 0x7FF:
        raise DefinitionError(f"{name}: CAN ID {can_id:#x} is not 11-bit")
    if doc["dlc"] != 8:
        raise DefinitionError(f"{name}: DLC must be 8")
    if int(doc["period_ms"]) <= 0:
        raise DefinitionError(f"{name}: period_ms must be positive")

    signals = []
    used = [0] * PAYLOAD_SIGNAL_BYTES
    for s in doc["signals"]:
        typ = s["type"]
        if typ not in TYPE_RAW_LIMITS:
            raise DefinitionError(f"{name}.{s['name']}: unknown type {typ}")
        if typ == "flag":
            sig = Signal(name=s["name"], byte=int(s["byte"]), type=typ, bit=int(s["bit"]))
            if not 0 <= sig.bit <= 7:
                raise DefinitionError(f"{name}.{sig.name}: bit out of 0..7")
        else:
            scale = Decimal(str(s["scale"]))
            sig = Signal(
                name=s["name"], byte=int(s["byte"]), type=typ, scale=scale, unit=s.get("unit", ""),
                raw_min=_raw_bound(f"{name}.{s['name']}.min", s["min"], scale),
                raw_max=_raw_bound(f"{name}.{s['name']}.max", s["max"], scale),
            )
            lo, hi = TYPE_RAW_LIMITS[typ]
            if not lo <= sig.raw_min <= sig.raw_max <= hi:
                raise DefinitionError(f"{name}.{sig.name}: raw range {sig.raw_min}..{sig.raw_max} does not fit {typ}")
        for byte, mask in _signal_bits(sig):
            if not 0 <= byte < PAYLOAD_SIGNAL_BYTES:
                raise DefinitionError(f"{name}.{sig.name}: must lie in bytes 0..{PAYLOAD_SIGNAL_BYTES - 1}")
            if used[byte] & mask:
                raise DefinitionError(f"{name}.{sig.name}: overlaps another signal in byte {byte}")
            used[byte] |= mask
        signals.append(sig)

    if len({s.name for s in signals}) != len(signals):
        raise DefinitionError(f"{name}: duplicate signal name")
    return Pdu(
        can_id=can_id, name=name, sender=doc["sender"], period_ms=int(doc["period_ms"]), dlc=8,
        signals=tuple(signals), reserved_mask=tuple(0xFF & ~u for u in used),
    )


def load(path: Path = DEFAULT_PATH) -> Definitions:
    doc = yaml.safe_load(Path(path).read_text(encoding="utf-8"))
    if doc.get("schema") != 1:
        raise DefinitionError("unsupported schema")
    p = doc["protection"]
    prot = Protection(
        counter_byte=p["counter"]["byte"], counter_shift=p["counter"]["shift"], counter_bits=p["counter"]["bits"],
        flags_byte=p["flags"]["byte"], valid_bit=p["flags"]["valid_bit"], degraded_bit=p["flags"]["degraded_bit"],
        fault_bit=p["flags"]["fault_bit"], reserved_bit=p["flags"]["reserved_bit"],
        crc_byte=p["crc"]["byte"], crc_poly=p["crc"]["poly"], crc_init=p["crc"]["init"],
        crc_xor_out=p["crc"]["xor_out"], counter_max_delta=p["counter_rx"]["max_delta"],
    )
    if (prot.counter_byte, prot.flags_byte, prot.crc_byte) != (6, 6, 7) or p["crc"]["reflect"]:
        raise DefinitionError("protection layout differs from project.md §5.2")
    pdus = tuple(_load_pdu(d) for d in doc["pdus"])
    if len({p.can_id for p in pdus}) != len(pdus):
        raise DefinitionError("duplicate CAN ID")
    return Definitions(protection=prot, pdus=pdus)
