#!/usr/bin/env python3
"""Evaluate one Phase 3 run (project.md §15.2): CAN -> Gateway -> SOME/IP event -> AP consumer.

usage: eval_someip_run.py <run_dir> <duration_s> <warmup_s>
reads  <run_dir>/gateway.json, <run_dir>/consumer.json
writes <run_dir>/summary.md, <run_dir>/verdict.json; exit 0 only if every criterion passes
"""
import json
import sys
from pathlib import Path


def load_last_json(path: Path) -> dict:
    lines = [l for l in path.read_text(encoding="utf-8").splitlines() if l.startswith("{")]
    if not lines:
        raise SystemExit(f"{path}: no JSON report (process failed?)")
    return json.loads(lines[-1])


def main() -> int:
    run, duration, warmup = Path(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
    gw = load_last_json(run / "gateway.json")
    co = load_last_json(run / "consumer.json")
    expected = (duration - warmup) * 1000 / 20  # 20 ms events after warm-up

    rejected = sum(p["rejected"] for p in gw["pdus"])
    criteria = [
        ("서비스 발견 5초 이내", 0 <= co["discovery_ms"] <= 5000, f"{co['discovery_ms']} ms"),
        ("CAN 수신→event 수신 p99 ≤ 50 ms", 0 <= co["latency_p99_ms"] <= 50,
         f"p50 {co['latency_p50_ms']} / p99 {co['latency_p99_ms']} / max {co['latency_max_ms']} ms"),
        ("측정 event 수 ≥ 예상의 90%", co["measured"] >= 0.9 * expected, f"{co['measured']} / 예상 {expected:.0f}"),
        ("event 순번 누락 0", co["sequence_gaps"] == 0, str(co["sequence_gaps"])),
        ("잘못된 event 0", co["malformed_events"] == 0, str(co["malformed_events"])),
        ("워밍업 뒤 validity 전부 1", co["not_valid_events"] == 0, f"예외 {co['not_valid_events']}건"),
        ("워밍업 뒤 freshness 전부 1", co["not_fresh_events"] == 0, f"예외 {co['not_fresh_events']}건"),
        ("워밍업 뒤 Gateway 상태 Running", co["running_events"] == co["measured"], f"{co['running_events']} / {co['measured']}"),
        ("CAN 값이 event에 반영 (rpm 800~2500)", co["rpm_min"] <= 850 and co["rpm_max"] >= 2400,
         f"rpm {co['rpm_min']}~{co['rpm_max']}"),
        ("CAN 값이 event에 반영 (속도 60 km/h)", co["speed_max"] >= 5900, f"speed max {co['speed_max'] / 100:.2f} km/h"),
        ("GetSnapshot 응답 정상", co["snapshot_ok"] is True, str(co["snapshot_ok"])),
        ("비어 있지 않은 GetSnapshot 요청 거부", co["malformed_request_rejected"] is True,
         str(co["malformed_request_rejected"])),
        ("Gateway가 거부한 정상 CAN frame 0", rejected == 0 and gw["unattributed_errors"] == 0 and gw["lost_frames"] == 0,
         f"rejected {rejected}, unattributed {gw['unattributed_errors']}, lost {gw['lost_frames']}"),
    ]
    ok = all(c[1] for c in criteria)

    md = ["# Phase 3 SOME/IP 통합 시험", "", f"- 결과: **{'PASS' if ok else 'FAIL'}**",
          f"- 실행 시간 {duration:.0f}s, 워밍업 {warmup:.0f}s 제외", "", "| 기준 | 결과 | 값 |", "| --- | --- | --- |"]
    md += [f"| {name} | {'✅' if passed else '❌'} | {value} |" for name, passed, value in criteria]
    md += ["", "## Gateway CAN 입력", "", "| ID | 메시지 | 통과 | 거부 |", "| --- | --- | --- | --- |"]
    md += [f"| {p['id']} | {p['name']} | {p['accepted']} | {p['rejected']} |" for p in gw["pdus"]]
    md += ["", f"- Gateway가 보낸 event: {gw['events_sent']}, GetSnapshot 요청: {gw['snapshot_requests']}"
           f" (그중 거부 {gw['malformed_requests']})", ""]
    (run / "summary.md").write_text("\n".join(md), encoding="utf-8")
    (run / "verdict.json").write_text(json.dumps(
        {"pass": ok, "criteria": [{"name": n, "pass": p, "value": v} for n, p, v in criteria],
         "gateway": gw, "consumer": co}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("\n".join(md))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
