#!/usr/bin/env python3
"""Merge tester results with the CAN capture and judge Phase 5 (project.md §9, §15.2).

usage: eval_diag_run.py <out_dir>   reads tester.json, can.jsonl; writes summary.md, verdict.json
"""
import json
import sys
from collections import Counter
from pathlib import Path

OUT = Path(sys.argv[1])
results = json.loads((OUT / "tester.json").read_text(encoding="utf-8")) if (OUT / "tester.json").exists() else []
frames = [json.loads(l) for l in (OUT / "can.jsonl").read_text(encoding="utf-8").splitlines() if l.strip()] \
    if (OUT / "can.jsonl").exists() else []

# ISO 15765-2 PCI: 0x0n SF, 0x1n FF, 0x2n CF, 0x3n FC
pci = Counter((f["id"], {0: "SF", 1: "FF", 2: "CF", 3: "FC"}.get(int(f["data"][:1], 16), "?")) for f in frames)
padded = all(len(f["data"]) == 16 for f in frames)


def cap(name, ok, value):
    results.append({"group": "다중 프레임", "name": name, "pass": bool(ok), "value": str(value)})


cap("CAN 캡처: Engine 요청 0x7E0에 FF·CF, 응답 0x7E8에 FC", pci[(0x7E0, "FF")] and pci[(0x7E0, "CF")] and pci[(0x7E8, "FC")],
    {k: pci[(0x7E0, k)] for k in ("SF", "FF", "CF")} | {"0x7E8 FC": pci[(0x7E8, "FC")]})
cap("CAN 캡처: Engine 응답 0x7E8에 FF·CF, 요청 0x7E0에 FC", pci[(0x7E8, "FF")] and pci[(0x7E8, "CF")] and pci[(0x7E0, "FC")],
    {k: pci[(0x7E8, k)] for k in ("SF", "FF", "CF")} | {"0x7E0 FC": pci[(0x7E0, "FC")]})
cap("CAN 캡처: Brake 0x7E1/0x7E9 다중 프레임", pci[(0x7E1, "FF")] and pci[(0x7E9, "FF")], dict((f"{k[0]:#x} {k[1]}", v)
    for k, v in sorted(pci.items()) if k[0] in (0x7E1, 0x7E9)))
cap("CAN 캡처: 모든 진단 frame 8 byte (padding 0xCC)", frames and padded, f"{len(frames)} frames")

ok = bool(results) and all(r["pass"] for r in results)
groups = ["정상", "권한 부족", "시간 초과", "다중 프레임"]
md = ["# Phase 5 진단 시나리오 시험", "", f"- 결과: **{'PASS' if ok else 'FAIL'}**",
      f"- 기준 {sum(r['pass'] for r in results)}/{len(results)} 통과", ""]
for g in groups:
    rows = [r for r in results if r["group"] == g]
    md += [f"## {g} ({sum(r['pass'] for r in rows)}/{len(rows)})", "", "| 기준 | 결과 | 값 |", "| --- | --- | --- |"]
    md += [f"| {r['name']} | {'✅' if r['pass'] else '❌'} | {r['value'].replace('|', '/')} |" for r in rows]
    md.append("")
(OUT / "summary.md").write_text("\n".join(md), encoding="utf-8")
(OUT / "verdict.json").write_text(json.dumps({"pass": ok, "results": results, "pci": {f"{k[0]:#x} {k[1]}": v for k, v in pci.items()}},
                                             indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print("\n".join(md))
sys.exit(0 if ok else 1)
