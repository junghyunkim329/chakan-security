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
    h += ["typedef struct {", "\tuint8_t id;", "\tuint8_t pid;", "\tuint8_t dlc;", "\tuint8_t publisher; /* enum sil_lin_node */",
          "\tconst char *name;", "} sil_lin_frame_t;", "",
          f"#define SIL_LIN_FRAME_COUNT {len(frames)}u",
          "extern const sil_lin_frame_t sil_lin_frames[SIL_LIN_FRAME_COUNT];",
          "/* frame ID per 10 ms slot, SIL_LIN_IDLE_SLOT for idle */",
          "extern const uint8_t sil_lin_schedule[SIL_LIN_SCHEDULE_SLOTS];", "",
          "#ifdef __cplusplus", "}", "#endif", "", "#endif", ""]

    c = ["/* Generated from config/lin/network.yaml by tools/codegen/gen_lin.py. Do not edit. */",
         '#include "sil_lin_gen.h"', "", "const sil_lin_frame_t sil_lin_frames[SIL_LIN_FRAME_COUNT] = {"]
    for f in frames:
        c.append(f"\t{{0x{f['id']:02X}u, 0x{protected_id(f['id']):02X}u, {f['dlc']}u, SIL_LIN_NODE_{ident(f['publisher'])}, "
                 f"\"{f['name']}\"}},")
    c += ["};", "", "const uint8_t sil_lin_schedule[SIL_LIN_SCHEDULE_SLOTS] = {"]
    c.append("\t" + ", ".join("SIL_LIN_IDLE_SLOT" if x is None else f"0x{x:02X}u" for x in doc["schedule"]) + ",")
    c += ["};", ""]

    out.mkdir(parents=True, exist_ok=True)
    (out / "sil_lin_gen.h").write_text("\n".join(h), encoding="utf-8")
    (out / "sil_lin_gen.c").write_text("\n".join(c), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
