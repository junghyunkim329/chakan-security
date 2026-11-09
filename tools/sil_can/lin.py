"""Load and validate config/lin/network.yaml, plus reference LIN parity and checksum (project.md §6.2)."""
from __future__ import annotations

from pathlib import Path

import yaml

from .definitions import REPO_ROOT

DEFAULT_PATH = REPO_ROOT / "config" / "lin" / "network.yaml"


class LinDefinitionError(ValueError):
    pass


def protected_id(frame_id: int) -> int:
    """§6.2: P0 = ID0^ID1^ID2^ID4, P1 = NOT(ID1^ID3^ID4^ID5)."""
    b = [(frame_id >> i) & 1 for i in range(6)]
    p0 = b[0] ^ b[1] ^ b[2] ^ b[4]
    p1 = 1 - (b[1] ^ b[3] ^ b[4] ^ b[5])
    return (frame_id & 0x3F) | (p0 << 6) | (p1 << 7)


def enhanced_checksum(pid: int, data: bytes) -> int:
    """§6.2: carry-around 8-bit sum of the protected ID and data, inverted."""
    s = pid
    for b in data:
        s += b
        if s > 0xFF:
            s -= 0xFF
    return (~s) & 0xFF


def load(path: Path = DEFAULT_PATH) -> dict:
    doc = yaml.safe_load(Path(path).read_text(encoding="utf-8"))
    if doc.get("schema") != 1:
        raise LinDefinitionError("unsupported schema")
    nodes = set(doc["nodes"])
    frames = {}
    for f in doc["frames"]:
        if not 0 <= f["id"] <= 0x3B:
            raise LinDefinitionError(f"{f['name']}: unconditional frame IDs are 0x00..0x3B")
        if f["id"] in frames:
            raise LinDefinitionError(f"{f['name']}: duplicate ID")
        if not 1 <= f["dlc"] <= 7:
            raise LinDefinitionError(f"{f['name']}: DLC 1..7 (payload + checksum must fit one CAN frame)")
        if f["publisher"] not in nodes or any(s not in nodes for s in f["subscribers"]):
            raise LinDefinitionError(f"{f['name']}: unknown node")
        if f["checksum"] != "enhanced":
            raise LinDefinitionError(f"{f['name']}: only enhanced checksum is in scope (§6.2)")
        used = [0] * f["dlc"]
        for s in f["signals"]:
            if s["byte"] >= f["dlc"] or s["bit"] + s["bits"] > 8:
                raise LinDefinitionError(f"{f['name']}.{s['name']}: outside the payload")
            mask = ((1 << s["bits"]) - 1) << s["bit"]
            if used[s["byte"]] & mask:
                raise LinDefinitionError(f"{f['name']}.{s['name']}: overlaps")
            used[s["byte"]] |= mask
        frames[f["id"]] = f
    sched = doc["schedule"]
    if len(sched) * doc["bus"]["tick_ms"] != 100:
        raise LinDefinitionError("major schedule must be 100 ms")
    if any(x is not None and x not in frames for x in sched):
        raise LinDefinitionError("schedule references an unknown frame")
    doc["frames_by_id"] = frames
    return doc
