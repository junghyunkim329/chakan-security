"""Python reference codec tests and C/Python cross-check from the same YAML."""
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from sil_can import codec, definitions  # noqa: E402

DEFS = definitions.load()


def test_crc_check_value():
    assert codec.crc8(b"123456789") == 0x4B


def test_definition_reserved_masks():
    brake = DEFS.by_id(0x180)
    assert brake.reserved_mask == (0x00, 0x00, 0x00, 0xF0, 0xFF, 0xFF)
    dyn = DEFS.by_id(0x110)
    assert dyn.reserved_mask[5] == 0xF3
    assert dyn.sender == "brake"


@pytest.mark.skipif("SIL_CODEC_VECTORS" not in os.environ, reason="run through ctest")
def test_c_and_python_encode_identically():
    out = subprocess.run([os.environ["SIL_CODEC_VECTORS"]], check=True, capture_output=True, text=True).stdout
    vectors = [json.loads(line) for line in out.splitlines()]
    assert len(vectors) > 200
    for v in vectors:
        pdu = DEFS.by_id(v["id"])
        raw = dict(zip((s.name for s in pdu.signals), v["raw"]))
        data = codec.encode(DEFS, pdu, raw, v["counter"], bool(v["valid"]), bool(v["degraded"]), bool(v["fault"]))
        assert data.hex() == v["data"], f"{pdu.name} {v}"
        d = codec.unpack(DEFS, v["id"], bytes.fromhex(v["data"]))
        assert d.status == codec.OK and d.raw == raw and d.counter == v["counter"]


def test_python_rejects_invalid_data():
    pdu = DEFS.by_id(0x100)
    raw = {s.name: s.raw_min for s in pdu.signals}
    good = codec.encode(DEFS, pdu, raw, 1)
    assert codec.unpack(DEFS, 0x101, good).status == codec.ERR_UNKNOWN_ID
    assert codec.unpack(DEFS, 0x100, good[:7]).status == codec.ERR_DLC
    bad = bytearray(good)
    bad[0] ^= 0x01
    assert codec.unpack(DEFS, 0x100, bytes(bad)).status == codec.ERR_CRC
    bad = bytearray(good)
    bad[5] |= 0x10
    bad[7] = codec.frame_crc(DEFS, 0x100, bad)
    assert codec.unpack(DEFS, 0x100, bytes(bad)).status == codec.ERR_RESERVED
    bad = bytearray(good)
    bad[0:2] = (8001).to_bytes(2, "big")
    bad[7] = codec.frame_crc(DEFS, 0x100, bad)
    d = codec.unpack(DEFS, 0x100, bytes(bad))
    assert d.status == codec.OK and codec.check_values(d) == codec.ERR_RANGE


def test_counter_checker_matches_c_rules():
    c = codec.CounterChecker(DEFS.protection.counter_max_delta)
    seq = [(14, codec.OK, 0), (15, codec.OK, 0), (0, codec.OK, 0), (0, codec.ERR_COUNTER_DUPLICATE, 0),
           (2, codec.OK, 1), (5, codec.OK, 2), (9, codec.ERR_COUNTER_JUMP, 0), (10, codec.OK, 0),
           (8, codec.ERR_COUNTER_JUMP, 0), (9, codec.OK, 0)]
    for counter, status, lost in seq:
        assert c.check(counter) == (status, lost), counter


def test_definition_validation(tmp_path):
    text = definitions.DEFAULT_PATH.read_text(encoding="utf-8")
    overlap = text.replace("{name: throttle,       byte: 2,", "{name: throttle,       byte: 1,")
    p = tmp_path / "bad.yaml"
    p.write_text(overlap, encoding="utf-8")
    with pytest.raises(definitions.DefinitionError, match="overlaps"):
        definitions.load(p)
