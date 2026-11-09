#!/usr/bin/env python3
"""Generate C tables from config/signals/can.yaml.

usage: gen_can_signals.py <can.yaml> <out_dir>
writes <out_dir>/sil_can_signals_gen.h and sil_can_signals_gen.c
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sil_can.definitions import load  # noqa: E402

C_TYPES = {"u8": "SIL_SIG_U8", "i8": "SIL_SIG_I8", "u16be": "SIL_SIG_U16BE",
           "i16be": "SIL_SIG_I16BE", "flag": "SIL_SIG_FLAG"}


def ident(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "_", name).upper()


def main() -> int:
    yaml_path, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    defs = load(yaml_path)
    p = defs.protection
    out_dir.mkdir(parents=True, exist_ok=True)

    h = ["/* Generated from config/signals/can.yaml by tools/codegen/gen_can_signals.py. Do not edit. */",
         "#ifndef SIL_CAN_SIGNALS_GEN_H", "#define SIL_CAN_SIGNALS_GEN_H", "",
         '#include "sil/can_defs.h"', "",
         f"#define SIL_COUNTER_BYTE {p.counter_byte}u", f"#define SIL_COUNTER_SHIFT {p.counter_shift}u",
         f"#define SIL_COUNTER_MODULO {1 << p.counter_bits}u", f"#define SIL_COUNTER_MAX_DELTA {p.counter_max_delta}u",
         f"#define SIL_FLAGS_BYTE {p.flags_byte}u", f"#define SIL_FLAG_VALID_BIT {p.valid_bit}u",
         f"#define SIL_FLAG_DEGRADED_BIT {p.degraded_bit}u", f"#define SIL_FLAG_FAULT_BIT {p.fault_bit}u",
         f"#define SIL_FLAG_RESERVED_BIT {p.reserved_bit}u", f"#define SIL_CRC_BYTE {p.crc_byte}u",
         f"#define SIL_CRC_POLY 0x{p.crc_poly:02X}u", f"#define SIL_CRC_INIT 0x{p.crc_init:02X}u",
         f"#define SIL_CRC_XOR_OUT 0x{p.crc_xor_out:02X}u", "",
         "enum sil_pdu_index {"]
    h += [f"\tSIL_PDU_{ident(pdu.name)}," for pdu in defs.pdus]
    h += ["\tSIL_PDU_COUNT", "};", ""]
    for pdu in defs.pdus:
        n = ident(pdu.name)
        h.append(f"#define SIL_{n}_ID 0x{pdu.can_id:03X}u")
        h.append(f"enum sil_{pdu.name.lower()}_signal {{")
        h += [f"\tSIL_{n}_{ident(s.name)}," for s in pdu.signals]
        h += [f"\tSIL_{n}_SIGNAL_COUNT", "};", ""]
    max_sig = max(len(pdu.signals) for pdu in defs.pdus)
    h += [f"#define SIL_MAX_SIGNALS {max_sig}u", "",
          "extern const sil_pdu_def_t sil_pdus[SIL_PDU_COUNT];", "", "#endif", ""]

    c = ["/* Generated from config/signals/can.yaml by tools/codegen/gen_can_signals.py. Do not edit. */",
         '#include "sil_can_signals_gen.h"', ""]
    for pdu in defs.pdus:
        c.append(f"static const sil_sig_def_t sigs_{pdu.name.lower()}[] = {{")
        for s in pdu.signals:
            c.append(f'\t{{"{s.name}", {s.byte}u, {s.bit}u, {C_TYPES[s.type]}, {s.raw_min}, {s.raw_max}, {s.scale}}},')
        c += ["};", ""]
    c.append("const sil_pdu_def_t sil_pdus[SIL_PDU_COUNT] = {")
    for pdu in defs.pdus:
        mask = ", ".join(f"0x{m:02X}u" for m in pdu.reserved_mask)
        c.append(f'\t{{0x{pdu.can_id:03X}u, "{pdu.name}", "{pdu.sender}", {pdu.period_ms}u, {pdu.dlc}u, '
                 f"{len(pdu.signals)}u, sigs_{pdu.name.lower()}, {{{mask}}}}},")
    c += ["};", ""]

    (out_dir / "sil_can_signals_gen.h").write_text("\n".join(h), encoding="utf-8")
    (out_dir / "sil_can_signals_gen.c").write_text("\n".join(c), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
