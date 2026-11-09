#!/usr/bin/env python3
"""Generate C++20 SOME/IP service constants, serializers and golden fixtures (project.md §7.4).

usage: gen_services.py <out_dir> <service.yaml>...
writes <out_dir>/sil_services_gen.hpp
"""
import sys
from pathlib import Path

import yaml

TYPES = {"u8": ("std::uint8_t", 1, False), "u16": ("std::uint16_t", 2, False), "u32": ("std::uint32_t", 4, False),
         "u64": ("std::uint64_t", 8, False), "i8": ("std::int8_t", 1, True), "i16": ("std::int16_t", 2, True)}


def check_layout(tname, t):
    end = 0
    for f in sorted(t["fields"], key=lambda f: f["offset"]):
        if f["type"] not in TYPES:
            sys.exit(f"{tname}.{f['name']}: unknown type {f['type']}")
        if f["offset"] != end:
            sys.exit(f"{tname}.{f['name']}: gap or overlap at offset {f['offset']} (expected {end})")
        end += TYPES[f["type"]][1]
    if end != t["size"]:
        sys.exit(f"{tname}: fields cover {end} bytes, size is {t['size']}")


def golden_bytes(t):
    out = bytearray(t["size"])
    for f in t["fields"]:
        _, n, signed = TYPES[f["type"]]
        out[f["offset"]:f["offset"] + n] = int(t["golden"][f["name"]]).to_bytes(n, "big", signed=signed)
    return bytes(out)


def gen_type(tname, t):
    lines = [f"struct {tname} {{"]
    lines += [f"\t{TYPES[f['type']][0]} {f['name']} = 0;" for f in t["fields"]]
    lines += [f"\n\tstatic constexpr std::size_t kSize = {t['size']};", "};", ""]

    lines += [f"inline std::array<std::uint8_t, {t['size']}> serialize(const {tname} &v)", "{",
              f"\tstd::array<std::uint8_t, {t['size']}> b{{}};"]
    for f in t["fields"]:
        ctype, n, _ = TYPES[f["type"]]
        val = "0" if f.get("must_be_zero") else f"v.{f['name']}"
        lines.append(f"\tput_be<{n}>(b.data() + {f['offset']}, static_cast<std::uint64_t>(static_cast<{ctype}>({val})));")
    lines += ["\treturn b;", "}", ""]

    lines += [f"// Returns false (and leaves out unchanged) if the length is not {t['size']} or a reserved field is not 0.",
              f"inline bool deserialize(const std::uint8_t *data, std::size_t len, {tname} &out)", "{",
              f"\tif (data == nullptr || len != {t['size']})", "\t\treturn false;", f"\t{tname} v;"]
    for f in t["fields"]:
        ctype, n, _ = TYPES[f["type"]]
        lines.append(f"\tv.{f['name']} = static_cast<{ctype}>(get_be<{n}>(data + {f['offset']}));")
        if f.get("must_be_zero"):
            lines += [f"\tif (v.{f['name']} != 0)", "\t\treturn false;"]
    lines += ["\tout = v;", "\treturn true;", "}", ""]

    if "golden" in t:
        g = golden_bytes(t)
        vals = ", ".join(f"0x{b:02X}" for b in g)
        lines += [f"// golden fixture computed by the generator from the YAML 'golden' block",
                  f"inline constexpr std::array<std::uint8_t, {t['size']}> k{tname}GoldenBytes = {{{vals}}};",
                  f"inline {tname} golden_{tname}()", "{", f"\t{tname} v;"]
        lines += [f"\tv.{f['name']} = static_cast<{TYPES[f['type']][0]}>({int(t['golden'][f['name']])}LL);"
                  for f in t["fields"]]
        lines += ["\treturn v;", "}", ""]
    return lines


def main():
    out_dir = Path(sys.argv[1])
    out = ["// Generated from config/services/*.yaml by tools/codegen/gen_services.py. Do not edit.",
           "// Project-specific service definitions; not ARXML and not ara::com.",
           "#pragma once", "", "#include <array>", "#include <cstddef>", "#include <cstdint>", "",
           "namespace sil::services {", "",
           "template <std::size_t N> inline void put_be(std::uint8_t *p, std::uint64_t v)", "{",
           "\tfor (std::size_t i = 0; i < N; i++)", "\t\tp[i] = static_cast<std::uint8_t>(v >> (8 * (N - 1 - i)));", "}", "",
           "template <std::size_t N> inline std::uint64_t get_be(const std::uint8_t *p)", "{",
           "\tstd::uint64_t v = 0;", "\tfor (std::size_t i = 0; i < N; i++)", "\t\tv = (v << 8) | p[i];",
           "\treturn v;", "}", ""]

    for path in sys.argv[2:]:
        doc = yaml.safe_load(Path(path).read_text(encoding="utf-8"))
        if doc.get("schema") != 1:
            sys.exit(f"{path}: unsupported schema")
        s = doc["service"]
        ns = s["name"]
        for tname, t in doc["types"].items():
            check_layout(tname, t)
            out += gen_type(tname, t)
        out += [f"namespace {ns} {{",
                f"inline constexpr std::uint16_t kServiceId = 0x{s['id']:04X};",
                f"inline constexpr std::uint16_t kInstanceId = 0x{s['instance']:04X};",
                f"inline constexpr std::uint8_t kMajorVersion = {s['major']};",
                f"inline constexpr std::uint32_t kMinorVersion = {s['minor']};",
                f"inline constexpr std::uint16_t kUnreliablePort = {s['unreliable_port']};",
                f"inline constexpr std::uint16_t kReliablePort = {s['reliable_port']};"]
        for m in doc.get("methods", []):
            out += [f"inline constexpr std::uint16_t k{m['name']}MethodId = 0x{m['id']:04X};",
                    f"inline constexpr std::size_t k{m['name']}RequestSize = {m['request_size']};"]
        for e in doc.get("events", []):
            out += [f"inline constexpr std::uint16_t k{e['name']}EventId = 0x{e['id']:04X};",
                    f"inline constexpr std::uint16_t k{e['name']}EventgroupId = 0x{e['eventgroup']:04X};",
                    f"inline constexpr unsigned k{e['name']}CycleMs = {e['cycle_ms']};"]
        out += [f"}} // namespace {ns}", ""]

    out += ["} // namespace sil::services", ""]
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "sil_services_gen.hpp").write_text("\n".join(out), encoding="utf-8")


if __name__ == "__main__":
    main()
