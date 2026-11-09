#!/usr/bin/env python3
"""Generate C tables from config/diagnostics/uds.yaml (project.md §9).

usage: gen_diag.py <uds.yaml> <out_dir>   -> <out_dir>/sil_diag_gen.h, sil_diag_gen.c
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sil_can.diagnostics import load  # noqa: E402


def c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    doc = load(Path(sys.argv[1]))
    out = Path(sys.argv[2])
    d, t, sec = doc["doip"], doc["timing"], doc["security"]
    ecus = doc["ecus_parsed"]
    eid = ", ".join(f"0x{b:02X}u" for b in bytes.fromhex(d["eid"].replace(":", "")))
    gid = ", ".join(f"0x{b:02X}u" for b in bytes.fromhex(d["gid"].replace(":", "")))

    h = ["/* Generated from config/diagnostics/uds.yaml by tools/codegen/gen_diag.py. Do not edit. */",
         "#ifndef SIL_DIAG_GEN_H", "#define SIL_DIAG_GEN_H", "", "#include <stdint.h>", "",
         "#ifdef __cplusplus", 'extern "C" {', "#endif", "",
         f"#define SIL_DOIP_PROTOCOL_VERSION 0x{d['protocol_version']:02X}u",
         f"#define SIL_DOIP_PORT {d['port']}u",
         f"#define SIL_DOIP_TESTER_ADDRESS 0x{d['tester_address']:04X}u",
         f"#define SIL_DOIP_GATEWAY_ADDRESS 0x{d['gateway_address']:04X}u",
         f"#define SIL_DOIP_VIN {c_str(d['vin'])}",
         f"#define SIL_DOIP_EID {{{eid}}}",
         f"#define SIL_DOIP_GID {{{gid}}}",
         f"#define SIL_DOIP_MAX_UDS_BYTES {d['max_uds_bytes']}u",
         f"#define SIL_DOIP_INITIAL_INACTIVITY_MS {d['initial_inactivity_ms']}u",
         f"#define SIL_DOIP_GENERAL_INACTIVITY_MS {d['general_inactivity_ms']}u",
         f"#define SIL_UDS_P2_SERVER_MS {t['p2_server_ms']}u",
         f"#define SIL_UDS_P2_STAR_SERVER_MS {t['p2_star_server_ms']}u",
         f"#define SIL_UDS_S3_SERVER_MS {t['s3_server_ms']}u",
         f"#define SIL_UDS_SECURITY_DELAY_MS {t['security_delay_ms']}u",
         f"#define SIL_UDS_SECURITY_LEVEL 0x{sec['level']:02X}u",
         f"#define SIL_UDS_SEED_BYTES {sec['seed_bytes']}u",
         f"#define SIL_UDS_KEY_BYTES {sec['key_file_bytes']}u",
         f"#define SIL_UDS_DELAY_AFTER_FAILURES {sec['delay_after_failures']}u",
         f"#define SIL_UDS_LOCKOUT_AFTER_FAILURES {sec['lockout_after_failures']}u",
         f"#define SIL_ISOTP_TX_PADDING 0x{doc['isotp']['tx_padding']:02X}u",
         f"#define SIL_DIAG_MAX_DTCS 8u", ""]
    for did, v in sorted(doc["dids_parsed"].items()):
        h.append(f"#define SIL_DID_{v['name'].upper()} 0x{did:04X}u")
    h += ["", "typedef struct {", "\tconst char *name;", "\tuint16_t logical_address;", "\tuint16_t request_id;",
          "\tuint16_t response_id;", "\tconst char *software_version;", "\tuint8_t did_count;", "\tconst uint16_t *dids;",
          "\tuint8_t dtc_count;", "\tconst uint32_t *dtcs;", "} sil_diag_ecu_t;", "", "enum sil_diag_ecu_index {"]
    h += [f"\tSIL_DIAG_ECU_{e.name.upper()}," for e in ecus]
    h += ["\tSIL_DIAG_ECU_COUNT", "};", "", "extern const sil_diag_ecu_t sil_diag_ecus[SIL_DIAG_ECU_COUNT];", "",
          "#ifdef __cplusplus", "}", "#endif", "", "#endif", ""]

    c = ["/* Generated from config/diagnostics/uds.yaml by tools/codegen/gen_diag.py. Do not edit. */",
         '#include "sil_diag_gen.h"', ""]
    for e in ecus:
        c.append(f"static const uint16_t dids_{e.name}[] = {{{', '.join(f'0x{x:04X}u' for x in e.dids)}}};")
        c.append(f"static const uint32_t dtcs_{e.name}[] = {{{', '.join(f'0x{x:06X}u' for x in e.dtcs)}}};")
    c += ["", "const sil_diag_ecu_t sil_diag_ecus[SIL_DIAG_ECU_COUNT] = {"]
    for e in ecus:
        c.append(f"\t{{{c_str(e.name)}, 0x{e.logical_address:04X}u, 0x{e.request_id:03X}u, 0x{e.response_id:03X}u, "
                 f"{c_str(e.software_version)}, {len(e.dids)}u, dids_{e.name}, {len(e.dtcs)}u, dtcs_{e.name}}},")
    c += ["};", ""]

    out.mkdir(parents=True, exist_ok=True)
    (out / "sil_diag_gen.h").write_text("\n".join(h), encoding="utf-8")
    (out / "sil_diag_gen.c").write_text("\n".join(c), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
