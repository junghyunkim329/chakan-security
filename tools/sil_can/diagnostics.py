"""Load and validate config/diagnostics/uds.yaml (shared by the C generator and the Python tester)."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import yaml

from .definitions import REPO_ROOT

DEFAULT_PATH = REPO_ROOT / "config" / "diagnostics" / "uds.yaml"


class DiagDefinitionError(ValueError):
    pass


@dataclass(frozen=True)
class DiagEcu:
    name: str
    logical_address: int
    request_id: int
    response_id: int
    software_version: str
    dids: tuple[int, ...]
    dtcs: tuple[int, ...]
    dtc_names: tuple[str, ...]


def load(path: Path = DEFAULT_PATH) -> dict:
    doc = yaml.safe_load(Path(path).read_text(encoding="utf-8"))
    if doc.get("schema") != 1:
        raise DiagDefinitionError("unsupported schema")
    d = doc["doip"]
    if len(d["vin"]) != 17 or not d["vin"].isascii():
        raise DiagDefinitionError("VIN must be 17 ASCII characters")
    for k in ("eid", "gid"):
        if len(bytes.fromhex(d[k].replace(":", ""))) != 6:
            raise DiagDefinitionError(f"{k} must be 6 bytes")
    if doc["security"]["seed_bytes"] != 16 or doc["security"]["key_file_bytes"] != 32:
        raise DiagDefinitionError("§9.4 requires a 16-byte seed and a 32-byte key")

    dids = {int(k): v for k, v in doc["dids"].items()}
    ecus, used_ids, used_las = [], set(), {d["tester_address"], d["gateway_address"]}
    for e in doc["ecus"]:
        ecu = DiagEcu(e["name"], e["logical_address"], e["request_id"], e["response_id"], e["software_version"],
                      tuple(e["dids"]), tuple(t["code"] for t in e["dtcs"]), tuple(t["name"] for t in e["dtcs"]))
        for can_id in (ecu.request_id, ecu.response_id):
            if not 0 <= can_id <= 0x7FF or can_id in used_ids:
                raise DiagDefinitionError(f"{ecu.name}: CAN ID {can_id:#x} invalid or duplicated")
            used_ids.add(can_id)
        if ecu.logical_address in used_las:
            raise DiagDefinitionError(f"{ecu.name}: logical address duplicated")
        used_las.add(ecu.logical_address)
        if any(did not in dids for did in ecu.dids):
            raise DiagDefinitionError(f"{ecu.name}: unknown DID")
        if not 0 < len(ecu.dtcs) <= 8 or any(not 0 <= c <= 0xFFFFFF for c in ecu.dtcs):
            raise DiagDefinitionError(f"{ecu.name}: 1..8 DTCs of 3 bytes required (F182 is an 8-bit bitmap)")
        if not ecu.software_version.isascii():
            raise DiagDefinitionError(f"{ecu.name}: software version must be ASCII")
        ecus.append(ecu)
    doc["ecus_parsed"] = ecus
    doc["dids_parsed"] = dids
    return doc
