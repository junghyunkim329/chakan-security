#!/usr/bin/env python3
"""Phase 6 scenario test: LIN body domain + BodyControl (project.md §6, §7.3), practice configuration hj_*.

usage (root): lin_scenarios.py <out_dir> <repo_root> <run_as_user>

  hj_veh : ecu_engine (ignition for the vehicle-state check)
  hj_body: lin_slave door/window/light (+ test control sockets), can_capture on the LIN wire
  hj_gw  : vehicle_gateway --lin-iface (LIN master + BodyControl provider)
  hj_ap  : body_control_client (SOME/IP client, no CAN/LIN access)
"""
import json
import os
import queue
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

OUT, ROOT, RUN_AS = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
B = ROOT / "build"
RUN = OUT / "run"
sys.path.insert(0, str(ROOT / "tools"))
from sil_can.lin import enhanced_checksum, protected_id  # noqa: E402

results = []
procs = {}
lines = []           # client output, (t_monotonic, dict)
lines_lock = threading.Lock()
SCHED = [0x11, 0x12, 0x13, 0x14, 0x10, 0x11, 0x12]


def check(group, name, ok, value):
    results.append({"group": group, "name": name, "pass": bool(ok), "value": str(value)})
    print(f"{'PASS' if ok else 'FAIL'}  [{group}] {name}: {value}", flush=True)


def start(name, ns, *cmd, env=None, stdin=None):
    pre = ["ip", "netns", "exec", ns, "setpriv", f"--reuid={RUN_AS}", f"--regid={RUN_AS}", "--init-groups", "--reset-env"]
    if env:
        pre += ["env"] + [f"{k}={v}" for k, v in env.items()]
    p = subprocess.Popen(pre + [str(c) for c in cmd], stdin=stdin, stdout=subprocess.PIPE if stdin else open(OUT / f"{name}.out", "w"),
                         stderr=open(OUT / f"{name}.log", "w"), text=True)
    procs[name] = p
    return p


def stop(name, sig=signal.SIGTERM):
    p = procs.get(name)
    if p and p.poll() is None:
        p.send_signal(sig)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()


def control(role, cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.sendto(cmd.encode(), str(RUN / f"{role}.sock"))
    s.close()


def client_reader(p):
    for line in p.stdout:
        try:
            d = json.loads(line)
        except ValueError:
            continue
        with lines_lock:
            lines.append((time.monotonic(), d))


def send(cmd):
    t = time.monotonic()
    procs["client"].stdin.write(cmd + "\n")
    procs["client"].stdin.flush()
    return t


def responses_after(t):
    with lines_lock:
        return [d for (ts, d) in lines if ts >= t and d["type"] == "response"]


def wait_response(t, timeout=2.0, n=1):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = responses_after(t)
        if len(r) >= n:
            return r[:n] if n > 1 else r[0]
        time.sleep(0.01)
    r = responses_after(t)
    return r if n > 1 else (r[0] if r else None)


def events_after(t):
    with lines_lock:
        return [(ts, d) for (ts, d) in lines if ts >= t and d["type"] == "event"]


def wait_event(t, pred, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for ts, d in events_after(t):
            if pred(d):
                return ts - t, d
        time.sleep(0.01)
    return None, None


def last_event():
    with lines_lock:
        ev = [d for (_, d) in lines if d["type"] == "event"]
    return ev[-1] if ev else None


def code(r):
    return None if r is None else r.get("code")


def inject_bad_header(pid):
    """Put a header with wrong parity on the LIN wire from the Gateway side (root, test only)."""
    py = ("import socket,struct;s=socket.socket(socket.AF_CAN,socket.SOCK_RAW,socket.CAN_RAW);s.bind(('vxlin_gw',));"
          f"s.send(struct.pack('=IB3x8s',1,1,bytes([{pid}])+bytes(7)))")
    subprocess.run(["ip", "netns", "exec", "hj_gw", "python3", "-I", "-c", py], check=True)
    return time.monotonic()


def main():
    RUN.mkdir(parents=True, exist_ok=True)
    os.chown(RUN, *(lambda pw: (pw.pw_uid, pw.pw_gid))(__import__("pwd").getpwnam(RUN_AS)))
    start("capture", "hj_body", "python3", "-I", ROOT / "tools/diag/can_capture.py", "vxlin_body", OUT / "lin.jsonl",
          "0x001", "0x002", "0x003")
    start("engine", "hj_veh", B / "ecu_engine", "--iface", "vxcan_veh")
    for role in ("door", "window", "light"):
        start(f"lin_{role}", "hj_body", B / "lin_slave", "--role", role, "--iface", "vxlin_body", "--control", RUN / f"{role}.sock")
    time.sleep(0.5)
    start("gateway", "hj_gw", B / "vehicle_gateway", "--iface", "vxcan_gw", "--lin-iface", "vxlin_gw",
          env={"VSOMEIP_CONFIGURATION": ROOT / "config/vsomeip/gateway.json"})
    time.sleep(1.0)
    client = start("client", "hj_ap", B / "body_control_client", env={"VSOMEIP_CONFIGURATION": ROOT / "config/vsomeip/body_client.json"},
                   stdin=subprocess.PIPE)
    threading.Thread(target=client_reader, args=(client,), daemon=True).start()
    try:
        scenario()
    finally:
        try:
            send("quit")
        except OSError:
            pass
        time.sleep(0.5)
        for name in list(procs):
            stop(name)
    evaluate()


def scenario():
    G1, G2, G3, G4 = "명령·응답", "응답 누락", "복구", "LIN 프로토콜"
    t0 = time.monotonic()
    ok = False
    while time.monotonic() - t0 < 10 and not ok:
        with lines_lock:
            ok = any(d["type"] == "available" for _, d in lines)
        time.sleep(0.05)
    check(G1, "AP가 BodyControl 서비스 발견", ok, f"{(time.monotonic() - t0) * 1000:.0f} ms")
    time.sleep(1.5)
    e = last_event()
    check(G1, "초기 상태: 문 닫힘·중앙 잠금, 창문 0, 실내등 꺼짐, 모두 fresh",
          e and e["door_bits"] == 0x10 and e["window"] == 0 and e["light"] == 0 and e["fresh"] == 0x07 and e["flags"] == 0, e)

    # ---- command and response ----
    t = send("window 50")
    r = wait_response(t)
    dt, ev = wait_event(t, lambda d: d["window"] == 50 and not d["flags"] & 0x80, 3)
    check(G1, "SetWindowPosition(50) → accepted(0)", code(r) == 0, f"{r}")
    check(G1, "창문이 5%씩 움직여 50 도달 (50 ms마다 5% → 약 0.5 s)", ev is not None and 0.45 <= dt <= 1.0, f"{dt and round(dt, 3)} s")
    steps = [d["window"] for _, d in events_after(t)]
    jumps = [b - a for a, b in zip(steps, steps[1:]) if b != a]
    check(G1, "event에서 창문 위치가 증가만 함 (100 ms event 사이 최대 10%)", jumps and all(0 < j <= 10 for j in jumps), jumps)

    t = send("light 1")
    r = wait_response(t)
    dt, ev = wait_event(t, lambda d: d["light"] == 1, 2)
    check(G1, "SetInteriorLight(1) → accepted(0), 실내등 켜짐", code(r) == 0 and ev is not None, f"{r}, {dt and round(dt, 3)} s")

    t = time.monotonic()
    control("door", "door FL 1")
    dt, ev = wait_event(t, lambda d: d["door_bits"] & 0x01, 1)
    check(G1, "Door ECU 입력(앞좌 문 열림)이 event에 반영", ev is not None, f"{dt and round(dt, 3)} s")
    control("door", "door FL 0")

    for args, name in (("window 101", "범위 밖 101"), ("raw 1 3201", "payload 2 byte"), ("light 2", "실내등 2")):
        t = send(args)
        r = wait_response(t)
        check(G1, f"잘못된 인자 ({name}) → invalid argument(1)", code(r) == 1, r)

    t = time.monotonic()
    control("window", "obstruct 1")
    send("window 100")
    time.sleep(0.6)
    e = last_event()
    check(G1, "장애물 주입: 창문 정지·obstruction·fault 표시", e and e["flags"] & 0x03 == 0x03 and not e["flags"] & 0x80, e)
    control("window", "obstruct 0")
    control("window", "fault 0")
    time.sleep(0.3)

    time.sleep(1.1)  # fresh rate-limit window
    t = send("window 40")
    for i in range(6):
        send(f"window {41 + i}")
    rs = wait_response(t, 2, 7)
    codes = [code(x) for x in rs] if isinstance(rs, list) else []
    check(G1, "초당 5회 초과 요청 거부 (state rejected 2)", codes.count(2) >= 2 and codes.count(0) >= 1, codes)
    time.sleep(1.2)

    stop("engine")
    time.sleep(0.2)
    t = send("window 10")
    r = wait_response(t)
    check(G1, "엔진 메시지 없음(점화 정보 stale) → 창문 명령 거부(2)", code(r) == 2, r)
    t = send("light 0")
    r = wait_response(t)
    check(G1, "같은 상황에서 실내등 명령은 허용(0)", code(r) == 0, r)
    start("engine", "hj_veh", B / "ecu_engine", "--iface", "vxcan_veh")
    time.sleep(0.5)
    t = send("window 10")
    r = wait_response(t)
    check(G1, "엔진 재개 후 창문 명령 허용(0)", code(r) == 0, r)
    time.sleep(1.2)

    # ---- missing responses ----
    t = time.monotonic()
    control("window", "mute 1")
    dt, ev = wait_event(t, lambda d: d["flags"] & 0x10 and not d["fresh"] & 0x02, 1.5)
    check(G2, "Window 응답 중단 → 3회 누락 후 통신 오류·not fresh 표시", ev is not None and dt <= 0.5, f"{dt and round(dt, 3)} s, {ev}")
    t = send("window 30")
    r = wait_response(t)
    check(G2, "통신 오류 중 창문 명령 → LIN timeout(3)", code(r) == 3, r)
    e = last_event()
    check(G2, "다른 Slave(Door·Light)는 정상 유지", e and not e["flags"] & 0x28 and e["fresh"] & 0x05 == 0x05, e)

    global t_corrupt_on, t_corrupt_off
    t = t_corrupt_on = time.monotonic()
    control("light", "corrupt 1")
    dt, ev = wait_event(t, lambda d: d["flags"] & 0x20, 1.5)
    check(G2, "Light checksum 오류 응답 → 3회 후 통신 오류", ev is not None and dt <= 0.6, f"{dt and round(dt, 3)} s")

    # ---- recovery ----
    t = time.monotonic()
    control("window", "mute 0")
    dt, ev = wait_event(t, lambda d: not d["flags"] & 0x10 and d["fresh"] & 0x02, 1.5)
    check(G3, "Window 응답 재개 → 정상 3회 후 복구·fresh", ev is not None and dt <= 0.5, f"{dt and round(dt, 3)} s")
    t = send("window 20")
    r = wait_response(t)
    dt, ev = wait_event(t, lambda d: d["window"] == 20, 3)
    check(G3, "복구 후 창문 명령 accepted(0)·동작", code(r) == 0 and ev is not None, f"{r}")
    t = t_corrupt_off = time.monotonic()
    control("light", "corrupt 0")
    dt, ev = wait_event(t, lambda d: not d["flags"] & 0x20 and d["fresh"] & 0x04, 1.5)
    check(G3, "Light checksum 정상화 → 복구", ev is not None, f"{dt and round(dt, 3)} s")
    t = send("light 1")
    r = wait_response(t)
    check(G3, "복구 후 실내등 명령 accepted(0)", code(r) == 0, r)

    # ---- protocol: parity, sleep / wakeup ----
    time.sleep(1.5)
    inject_bad_header(0x10)  # door frame 0x10 without parity bits (correct PID is 0x50)
    time.sleep(0.3)

    t_idle = time.monotonic()
    dt, ev = wait_event(t_idle, lambda d: d["flags"] & 0x40, 8)
    check(G4, "5초간 명령·상태 변화 없음 → LIN sleep", ev is not None and 4.0 <= dt <= 6.5, f"{dt and round(dt, 2)} s")
    global t_sleep, t_wake
    t_sleep = time.monotonic()
    time.sleep(1.0)
    t_wake = send("window 0")
    r = wait_response(t_wake)
    dt, ev = wait_event(t_wake, lambda d: not d["flags"] & 0x40, 2)
    check(G4, "sleep 중 명령 → wakeup 후 accepted(0)", code(r) == 0 and ev is not None, f"{r}")
    time.sleep(0.8)

    # ---- allowlist (client ID not allowed) ----
    send("quit")
    time.sleep(0.5)
    stop("client")
    with lines_lock:
        lines.clear()
    c2 = start("client_untrusted", "hj_ap", B / "body_control_client",
               env={"VSOMEIP_CONFIGURATION": ROOT / "config/vsomeip/body_client_untrusted.json"}, stdin=subprocess.PIPE)
    procs["client"] = c2
    threading.Thread(target=client_reader, args=(c2,), daemon=True).start()
    time.sleep(2.0)
    t = send("window 60")
    r = wait_response(t, 3)
    check(G1, "허용 목록에 없는 client ID 0x2FFF → 거부(2)", code(r) == 2, r)


def evaluate():
    G4 = "LIN 프로토콜"
    frames = [json.loads(l) for l in (OUT / "lin.jsonl").read_text().splitlines() if l.strip()]
    headers = [(f["t"], int(f["data"][:2], 16)) for f in frames if f["id"] == 1]
    # pair every response with the header right before it
    bad_ck, checked, last_pid = [], 0, None
    light = []
    for f in frames:
        if f["id"] == 1:
            last_pid = int(f["data"][:2], 16)
        elif f["id"] == 2 and last_pid is not None:
            d = bytes.fromhex(f["data"])
            checked += 1
            if d[-1] != enhanced_checksum(last_pid, d[:-1]):
                bad_ck.append(f["t"])
            elif last_pid == protected_id(0x14):
                light.append(d[0])
            last_pid = None
    check(G4, "모든 header의 PID parity가 표준 표와 일치 (주입한 1건 제외)",
          sum(1 for _, p in headers if p not in {protected_id(i) for i in SCHED}) == 1, f"{len(headers)} headers")

    # steady 1 s window after the start: header order (allowed successor pairs) and slot timing
    pids = [protected_id(i) for i in SCHED]
    allowed = {(pids[i], pids[(i + 1) % 7]) for i in range(7)}
    seq = [p for _, p in headers[14:84]]
    order_ok = len(seq) > 60 and all((a, b) in allowed for a, b in zip(seq, seq[1:]))
    gaps = [round((b[0] - a[0]) * 1000, 1) for a, b in zip(headers[14:84], headers[15:85])]
    timing_ok = gaps and all(7 <= g <= 13 or 37 <= g <= 43 for g in gaps)
    check(G4, "스케줄 순서 0x11→0x12→0x13→0x14→0x10→0x11→0x12 반복", order_ok, [hex(p) for p in seq[:8]])
    check(G4, "slot 간격 10 ms (±3), idle 구간 40 ms (±3)", timing_ok, f"min {min(gaps) if gaps else None} max {max(gaps) if gaps else None} ms")
    outside = [t for t in bad_ck if not t_corrupt_on <= t <= t_corrupt_off + 0.15]
    check(G4, "checksum 오류는 손상 주입 구간에만 (enhanced checksum)", len(bad_ck) >= 3 and not outside,
          f"오류 {len(bad_ck)} / {checked} 응답, 구간 밖 {len(outside)}")

    inj = next((i for i, f in enumerate(frames) if f["id"] == 1 and f["data"] == "10"), None)
    nxt = frames[inj + 1] if inj is not None and inj + 1 < len(frames) else None
    answered = nxt is not None and nxt["id"] == 2 and nxt["t"] - frames[inj]["t"] < 0.005
    check(G4, "parity가 틀린 header(0x10, 올바른 값 0x50)에는 Door가 응답하지 않음", inj is not None and not answered,
          f"주입 header {'있음' if inj is not None else '없음'}, 다음 frame {nxt and hex(nxt['id'])}")
    door_log = (OUT / "lin_door.log").read_text()
    check(G4, "Door Slave가 parity 오류 header를 기록", "bad_parity=1" in door_log, door_log.strip().splitlines()[-1] if door_log else "")

    quiet = [h for h in headers if t_sleep <= h[0] <= t_wake]
    check(G4, "sleep 동안 LIN 버스 header 0개", len(quiet) == 0, f"{len(quiet)} headers in {t_wake - t_sleep:.2f} s")
    wake = [f for f in frames if f["id"] == 3 and f["t"] >= t_wake]
    first = next((h for h in headers if h[0] > t_wake), None)
    check(G4, "wakeup 신호 후 100 ms 이상 지나 0 ms slot(0x11)부터 재시작",
          wake and first and first[1] == 0x11 and first[0] - wake[0]["t"] >= 0.099, f"{wake and round((first[0] - wake[0]['t']) * 1000, 1)} ms, PID {first and hex(first[1])}")

    up = [b - a for a, b in zip(light, light[1:]) if b > a]
    check(G4, "실내등 fade: 100 ms마다 10%씩 밝아짐", up and max(up) <= 10 and 10 in up, sorted(set(up)))

    ok = all(r["pass"] for r in results)
    gw = [l for l in (OUT / "gateway.out").read_text().splitlines() if l.startswith("{")]  # vSomeIP also logs to stdout
    groups = ["명령·응답", "응답 누락", "복구", "LIN 프로토콜"]
    md = ["# Phase 6 LIN 차체 도메인 시나리오 시험 (연습 구성 hj_*)", "", f"- 결과: **{'PASS' if ok else 'FAIL'}**",
          f"- 기준 {sum(r['pass'] for r in results)}/{len(results)} 통과", ""]
    for g in groups:
        rows = [r for r in results if r["group"] == g]
        md += [f"## {g} ({sum(r['pass'] for r in rows)}/{len(rows)})", "", "| 기준 | 결과 | 값 |", "| --- | --- | --- |"]
        md += [f"| {r['name']} | {'✅' if r['pass'] else '❌'} | {r['value'].replace('|', '/')[:200]} |" for r in rows]
        md.append("")
    md += ["## Gateway 보고", "", "```json", gw[-1] if gw else "(없음)", "```", ""]
    (OUT / "summary.md").write_text("\n".join(md), encoding="utf-8")
    (OUT / "verdict.json").write_text(json.dumps({"pass": ok, "results": results}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("\n".join(md))
    sys.exit(0 if ok else 1)


t_sleep = t_wake = t_corrupt_on = t_corrupt_off = 0.0

if __name__ == "__main__":
    main()
