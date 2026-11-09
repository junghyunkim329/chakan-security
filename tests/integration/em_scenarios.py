#!/usr/bin/env python3
"""Phase 4 scenario test: Execution Manager start order, exit, hang, repeated failure, shutdown (project.md §8, §15.2).

usage (root): em_scenarios.py <out_dir> <repo_root> <run_as_user> <deployed_bin_dir>
Starts build/execution_manager with config/manifests/execution.json on the practice namespaces (hj_*),
injects faults, and judges the audit log. Writes <out_dir>/summary.md and verdict.json.
"""
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

OUT, ROOT, RUN_AS, BIN_DIR = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
AUDIT, STATUS, RUNDIR = OUT / "audit.jsonl", OUT / "status.json", OUT / "run"
VARS = {"ROOT": str(ROOT), "BIN_DIR": BIN_DIR, "RUN_AS": RUN_AS, "NS_AP": "hj_ap", "NS_GW": "hj_gw", "NS_VEH": "hj_veh",
        "CAN_VEH_IF": "vxcan_veh", "CAN_GW_IF": "vxcan_gw"}
MEMORY_MB = {"engine": 64, "brake": 64, "steering": 64, "gateway": 1024, "consumer": 1024}
MS = 1_000_000

criteria = []
metrics = {}


def check(name, ok, value):
    criteria.append({"name": name, "pass": bool(ok), "value": str(value)})
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {value}", flush=True)


def events():
    if not AUDIT.exists():
        return []
    return [json.loads(l) for l in AUDIT.read_text(encoding="utf-8").splitlines() if l.strip()]


def status():
    try:
        return json.loads(STATUS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def pid_of(name):
    for p in status().get("processes", []):
        if p["name"] == name:
            return p["pid"]
    return 0


def wait_for(pred, timeout_s):
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.05)
    return None


def first(type_, process=None, after_ns=0, detail=None):
    for e in events():
        if e["type"] == type_ and e["mono_ns"] >= after_ns and (process is None or e["process"] == process) \
                and (detail is None or detail in e["detail"]):
            return e
    return None


def state_event(to, after_ns):
    return first("state", None, after_ns, f"-> {to}")


def all_running():
    s = status()
    return s.get("state") == "Running" and all(p["state"] == "Running" for p in s.get("processes", []))


def fault_and_recover(label, proc, sig, detect_range_ms):
    """Inject sig into proc, then measure detection, Degraded, restart attempt and recovery."""
    old = pid_of(proc)
    t0 = time.monotonic_ns()
    os.kill(old, sig)
    fail = wait_for(lambda: first("failure", proc, t0), 5)
    start = wait_for(lambda: first("start", proc, t0), 6)
    ready = wait_for(lambda: first("ready", proc, t0), 10)
    running = wait_for(lambda: state_event("Running", t0) if all_running() else None, 10)
    degraded = state_event("Degraded", t0)
    det = (fail["mono_ns"] - t0) / MS if fail else None
    rst = (start["mono_ns"] - fail["mono_ns"]) / MS if fail and start else None
    rec = (running["mono_ns"] - t0) / MS if running else None
    metrics[label] = {"process": proc, "old_pid": old, "new_pid": pid_of(proc), "detect_ms": det,
                      "degraded_ms": (degraded["mono_ns"] - t0) / MS if degraded else None,
                      "restart_attempt_ms": rst, "recovered_ms": rec, "failure": fail and fail["detail"]}
    lo, hi = detect_range_ms
    check(f"[{label}] {proc} 장애 감지 {lo}~{hi} ms", det is not None and lo <= det <= hi, f"{det} ms ({fail and fail['detail']})")
    check(f"[{label}] 상태 Degraded 기록", degraded is not None and degraded["mono_ns"] <= fail["mono_ns"] + 100 * MS,
          f"{metrics[label]['degraded_ms']} ms")
    check(f"[{label}] 3초 안에 재시작 시도 (§8.2)", rst is not None and rst <= 3000, f"{rst} ms")
    check(f"[{label}] 새 pid로 Running 복귀", ready is not None and running is not None and pid_of(proc) not in (0, old),
          f"복구 {rec} ms, pid {old} -> {pid_of(proc)}")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = [str(Path(BIN_DIR) / "execution_manager"), "--manifest", str(ROOT / "config/manifests/execution.json"),
           "--runtime-dir", str(RUNDIR), "--audit-log", str(AUDIT), "--status-file", str(STATUS)]
    for k, v in VARS.items():
        cmd += ["--var", f"{k}={v}"]
    t_em = time.monotonic_ns()
    em = subprocess.Popen(cmd, stdout=open(OUT / "em.log", "w"), stderr=subprocess.STDOUT)
    try:
        run(em, t_em)
    finally:
        if em.poll() is None:
            em.send_signal(signal.SIGTERM)
            try:
                em.wait(15)
            except subprocess.TimeoutExpired:
                em.kill()

    ok = all(c["pass"] for c in criteria)
    md = ["# Phase 4 Execution Manager 시나리오 시험", "", f"- 결과: **{'PASS' if ok else 'FAIL'}**",
          f"- 기준 {sum(c['pass'] for c in criteria)}/{len(criteria)} 통과", "", "| 기준 | 결과 | 값 |", "| --- | --- | --- |"]
    md += [f"| {c['name']} | {'✅' if c['pass'] else '❌'} | {c['value']} |" for c in criteria]
    md += ["", "## 측정값", "", "```json", json.dumps(metrics, indent=2, ensure_ascii=False), "```", ""]
    (OUT / "summary.md").write_text("\n".join(md), encoding="utf-8")
    (OUT / "verdict.json").write_text(json.dumps({"pass": ok, "criteria": criteria, "metrics": metrics},
                                                 indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("\n".join(md))
    sys.exit(0 if ok else 1)


def run(em, t_em):
    # 1) startup in dependency order
    running = wait_for(lambda: all_running() and state_event("Running", t_em), 30)
    check("시작 후 모든 프로세스 Running", running is not None,
          f"{(running['mono_ns'] - t_em) / MS:.0f} ms" if running else f"state={status().get('state')}")
    if running is None:
        return
    starts = {e["process"]: e["mono_ns"] for e in events() if e["type"] == "start"}
    readies = {e["process"]: e["mono_ns"] for e in events() if e["type"] == "ready"}
    order_ok = max(readies[p] for p in ("engine", "brake", "steering")) <= starts["gateway"] <= readies["gateway"] <= starts["consumer"]
    check("의존 순서 시작: CAN ECU 준비 → Gateway → consumer", order_ok,
          " → ".join(sorted(starts, key=starts.get)))

    # 2) per-process user, capabilities, namespace, CPU, memory as applied by the kernel
    verifies = {e["process"]: e["detail"] for e in events() if e["type"] == "verify"}
    insecure = [e for e in events() if e["type"] == "security"]
    check("5개 프로세스 non-root·capability 0·NoNewPrivs·CPU·netns 확인", len(verifies) == 5 and not insecure,
          "; ".join(f"{k}: {v}" for k, v in sorted(verifies.items())))
    mem_ok = []
    for name, mb in MEMORY_MB.items():
        limits = Path(f"/proc/{pid_of(name)}/limits").read_text()
        line = next(l for l in limits.splitlines() if l.startswith("Max address space"))
        mem_ok.append(int(line.split()[3]) == mb * 1024 * 1024)
    check("메모리 상한(RLIMIT_AS) 적용", all(mem_ok), dict(zip(MEMORY_MB, mem_ok)))
    time.sleep(2)

    # 3) process exit, 4) hang (SIGSTOP: alive but silent)
    fault_and_recover("종료", "brake", signal.SIGKILL, (0, 500))
    time.sleep(2)
    fault_and_recover("응답 정지", "steering", signal.SIGSTOP, (1000, 2000))
    time.sleep(2)

    # 5) repeated failure: 3 failures of the same process within 60 s -> restart stops, Degraded
    t0 = time.monotonic_ns()
    for k in range(3):
        pid = pid_of("engine")
        tk = time.monotonic_ns()
        os.kill(pid, signal.SIGKILL)
        if k < 2:
            wait_for(lambda: first("ready", "engine", tk), 10)
    exhausted = wait_for(lambda: first("budget_exhausted", "engine", t0), 5)
    time.sleep(5)
    restarted_after = first("start", "engine", exhausted["mono_ns"]) if exhausted else None
    s = status()
    eng = next(p for p in s["processes"] if p["name"] == "engine")
    metrics["반복 실패"] = {"budget_exhausted_ms": (exhausted["mono_ns"] - t0) / MS if exhausted else None,
                         "engine": eng, "state": s["state"]}
    check("[반복 실패] 60초 안 3회 실패 → 자동 재시작 중지", exhausted is not None and restarted_after is None
          and eng["state"] == "Failed" and eng["pid"] == 0, f"{exhausted and exhausted['detail']}, engine={eng['state']}")
    check("[반복 실패] 상태 Degraded 유지", s["state"] == "Degraded", s["state"])

    # 6) shutdown in reverse dependency order
    pids = {p["name"]: p["pid"] for p in s["processes"] if p["pid"]}
    t0 = time.monotonic_ns()
    em.send_signal(signal.SIGTERM)
    rc = em.wait(20)
    stops = [e["process"] for e in events() if e["type"] == "stop" and e["mono_ns"] >= t0]
    alive = [n for n, p in pids.items() if Path(f"/proc/{p}").exists()]
    final = status().get("state")
    check("역순 종료: consumer → gateway → steering → brake", stops == ["consumer", "gateway", "steering", "brake"],
          " → ".join(stops))
    check("종료 후 남은 프로세스 0, 최종 상태 Shutdown, EM 종료코드 0", not alive and final == "Shutdown" and rc == 0,
          f"alive={alive}, state={final}, rc={rc}, {(time.monotonic_ns() - t0) / MS:.0f} ms")


if __name__ == "__main__":
    main()
