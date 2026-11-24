#!/usr/bin/env python3
"""Generate C tables from config/lin/network.yaml (project.md §6.2).

usage: gen_lin.py <network.yaml> <out_dir>   -> <out_dir>/sil_lin_gen.h, sil_lin_gen.c
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sil_can.lin import load, protected_id  # noqa: E402


def ident(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "_", s).upper()


def main() -> int:
    doc = load(Path(sys.argv[1]))
    out = Path(sys.argv[2])
    bus, enc, beh = doc["bus"], doc["encapsulation"], doc["slave_behaviour"]
    frames = sorted(doc["frames"], key=lambda f: f["id"])
    nodes = doc["nodes"]

    h = ["/* Generated from config/lin/network.yaml by tools/codegen/gen_lin.py. Do not edit. */",
         "#ifndef SIL_LIN_GEN_H", "#define SIL_LIN_GEN_H", "", "#include <stdint.h>", "",
         "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for k in ("tick_ms", "response_window_ms", "missed_limit", "recovery_count", "sleep_after_ms", "wakeup_delay_ms",
              "slave_bus_idle_ms"):
        h.append(f"#define SIL_LIN_{k.upper()} {bus[k]}u")
    for k, v in enc.items():
        h.append(f"#define SIL_LIN_{k.upper()} 0x{v:03X}u")
    h += [f"#define SIL_LIN_WINDOW_STEP {beh['window_step_percent']}u",
          f"#define SIL_LIN_LIGHT_FADE_STEP {beh['light_fade_step_percent']}u",
          f"#define SIL_LIN_SCHEDULE_SLOTS {len(doc['schedule'])}u",
          "#define SIL_LIN_IDLE_SLOT 0xFFu", "", "enum sil_lin_node {"]
    h += [f"\tSIL_LIN_NODE_{ident(n)}," for n in nodes]
    h += ["\tSIL_LIN_NODE_COUNT", "};", ""]
    for f in frames:
        n = ident(f["name"])
        h.append(f"#define SIL_LIN_{n}_ID 0x{f['id']:02X}u")
        h.append(f"#define SIL_LIN_{n}_PID 0x{protected_id(f['id']):02X}u")
        h.append(f"#define SIL_LIN_{n}_DLC {f['dlc']}u")
        for s in f["signals"]:
            sn = f"SIL_LIN_{n}_{ident(s['name'])}"
            h.append(f"#define {sn}_BYTE {s['byte']}u")
            h.append(f"#define {sn}_BIT {s['bit']}u")
            h.append(f"#define {sn}_BITS {s['bits']}u")
        h.append("")
    ranges = [(f, sig) for f in frames for sig in f["signals"] if "min" in sig]
    h += ["/* signal with a defined value range (min/max in network.yaml, enums included) */",
          "typedef struct {", "\tuint8_t byte;", "\tuint8_t bit;", "\tuint8_t bits;", "\tuint8_t min;", "\tuint8_t max;",
          "} sil_lin_range_t;", "",
          "typedef struct {", "\tuint8_t id;", "\tuint8_t pid;", "\tuint8_t dlc;", "\tuint8_t publisher; /* enum sil_lin_node */",
          "\tconst char *name;",
          "\tuint8_t used_mask[8];   /* bits owned by a signal; every other payload bit is reserved and must be 0 */",
          "\tuint8_t has_valid;      /* 1: the frame carries a 'valid' signal that must be 1 */",
          "\tuint8_t valid_byte;", "\tuint8_t valid_bit;",
          "\tuint8_t range_first;    /* index into sil_lin_ranges */", "\tuint8_t range_count;",
          "} sil_lin_frame_t;", "",
          f"#define SIL_LIN_FRAME_COUNT {len(frames)}u",
          f"#define SIL_LIN_RANGE_COUNT {len(ranges)}u",
          "extern const sil_lin_frame_t sil_lin_frames[SIL_LIN_FRAME_COUNT];",
          "extern const sil_lin_range_t sil_lin_ranges[SIL_LIN_RANGE_COUNT];",
          "/* frame ID per 10 ms slot, SIL_LIN_IDLE_SLOT for idle */",
          "extern const uint8_t sil_lin_schedule[SIL_LIN_SCHEDULE_SLOTS];", "",
          "#ifdef __cplusplus", "}", "#endif", "", "#endif", ""]

    c = ["/* Generated from config/lin/network.yaml by tools/codegen/gen_lin.py. Do not edit. */",
         '#include "sil_lin_gen.h"', "", "const sil_lin_frame_t sil_lin_frames[SIL_LIN_FRAME_COUNT] = {"]
    first = 0
    for f in frames:
        used = [0] * 8
        for sig in f["signals"]:
            used[sig["byte"]] |= ((1 << sig["bits"]) - 1) << sig["bit"]
        valid = next((sig for sig in f["signals"] if sig["name"] == "valid"), None)
        n_rng = sum(1 for sig in f["signals"] if "min" in sig)
        mask = ", ".join(f"0x{m:02X}u" for m in used)
        vb = f"1u, {valid['byte']}u, {valid['bit']}u" if valid else "0u, 0u, 0u"
        c.append(f"\t{{0x{f['id']:02X}u, 0x{protected_id(f['id']):02X}u, {f['dlc']}u, SIL_LIN_NODE_{ident(f['publisher'])}, "
                 f"\"{f['name']}\", {{{mask}}}, {vb}, {first}u, {n_rng}u}},")
        first += n_rng
    c += ["};", "", "const sil_lin_range_t sil_lin_ranges[SIL_LIN_RANGE_COUNT] = {"]
    for f, sig in ranges:
        c.append(f"\t{{{sig['byte']}u, {sig['bit']}u, {sig['bits']}u, {sig['min']}u, {sig['max']}u}}, /* {f['name']}.{sig['name']} */")
    c += ["};", "", "const uint8_t sil_lin_schedule[SIL_LIN_SCHEDULE_SLOTS] = {"]
    c.append("\t" + ", ".join("SIL_LIN_IDLE_SLOT" if x is None else f"0x{x:02X}u" for x in doc["schedule"]) + ",")
    c += ["};", ""]

    out.mkdir(parents=True, exist_ok=True)
    (out / "sil_lin_gen.h").write_text("\n".join(h), encoding="utf-8")
    (out / "sil_lin_gen.c").write_text("\n".join(c), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
