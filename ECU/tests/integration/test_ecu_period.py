"""ECU transmit period test on a real SocketCAN link (project.md §5.1, §15.1).

Runs the ECU processes as an unprivileged user in the vehicle namespace and receives
on the Gateway side of the vxcan pair with kernel receive timestamps.

Pass criteria (project selection, 2026-11-02, team confirmation pending), per PDU over
1000 consecutive intervals:
  - mean interval within +-1 % of the nominal period
  - 99 % of intervals within nominal +-2 ms
  - no interval longer than 2x nominal
  - every frame passes ID/DLC/CRC/reserved/valid/range checks and counter delta == 1

Must be started as root inside the receiving namespace, e.g. ECU/scripts/run/ecu-period-test.sh.
"""
import json
import os
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from sil_can import codec, definitions  # noqa: E402

DEFS = definitions.load()
ECUS = ["engine", "brake", "steering"]
INTERVALS = 1000
WARMUP = 5
SO_TIMESTAMPNS = getattr(socket, "SO_TIMESTAMPNS", 35)
CAN_FRAME = struct.Struct("=IB3x8s")

pytestmark = pytest.mark.skipif(os.environ.get("SIL_RUN_CAN_TESTS") != "1",
                                reason="needs root and the vehicle namespaces; use ECU/scripts/run/ecu-period-test.sh")


def _env(name):
    v = os.environ.get(name)
    if not v:
        pytest.fail(f"{name} not set")
    return v


def test_ecu_transmit_periods():
    ecu_ns, ecu_if, rx_if = _env("SIL_ECU_NS"), _env("SIL_ECU_IFACE"), _env("SIL_RX_IFACE")
    build = Path(os.environ.get("SIL_BUILD_DIR", ROOT / "build"))
    tx_pdus = [p for p in DEFS.pdus if p.sender in ECUS]
    longest = max(p.period_ms for p in tx_pdus)
    duration = (INTERVALS + WARMUP + 50) * longest / 1000.0

    rx = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    rx.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    rx.bind((rx_if,))
    rx.settimeout(1.0)

    # ECU binaries must be readable by the unprivileged user
    bindir = Path(tempfile.mkdtemp(prefix="sil-ecu-"))
    bindir.chmod(0o755)
    procs = []
    frames = {p.can_id: [] for p in tx_pdus}
    try:
        for e in ECUS:
            dst = bindir / f"ecu_{e}"
            shutil.copy2(build / f"ecu_{e}", dst)
            dst.chmod(0o755)
            procs.append(subprocess.Popen(
                ["ip", "netns", "exec", ecu_ns, "runuser", "-u", "nobody", "--",
                 str(dst), "--iface", ecu_if, "--duration-s", f"{duration:.1f}"],
                stderr=subprocess.PIPE, text=True))

        t0 = time.monotonic()
        deadline = t0 + duration + 3
        next_report = t0
        print(f"\n[period] ECU {len(ECUS)}개 실행, 약 {duration:.0f}초 동안 수신합니다. 끝날 때까지 기다려 주세요.", flush=True)
        while time.monotonic() < deadline and any(p.poll() is None for p in procs):
            if time.monotonic() >= next_report:
                counts = ", ".join(f"0x{i:03X}={len(v)}" for i, v in sorted(frames.items()))
                print(f"[period] {time.monotonic() - t0:4.0f}s / {duration:.0f}s  수신: {counts}", flush=True)
                next_report += 5
            try:
                msg, anc, _, _ = rx.recvmsg(CAN_FRAME.size, socket.CMSG_SPACE(16))
            except socket.timeout:
                continue
            can_id, dlc, data = CAN_FRAME.unpack(msg)
            ts = next(struct.unpack("qq", d) for lvl, typ, d in anc if lvl == socket.SOL_SOCKET and typ == SO_TIMESTAMPNS)
            frames.setdefault(can_id & socket.CAN_SFF_MASK, []).append((ts[0] * 10**9 + ts[1], bytes(data[:dlc])))
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        logs = [p.communicate(timeout=5)[1] for p in procs]
        shutil.rmtree(bindir, ignore_errors=True)
        rx.close()

    unexpected = sorted(hex(i) for i in frames if DEFS.by_id(i) is None or DEFS.by_id(i).sender not in ECUS)
    results, failures = [], []
    for pdu in tx_pdus:
        got = frames[pdu.can_id]
        nominal = pdu.period_ms * 1e6
        r = {"id": hex(pdu.can_id), "name": pdu.name, "sender": pdu.sender, "period_ms": pdu.period_ms,
             "frames": len(got)}
        if len(got) < WARMUP + INTERVALS + 1:
            failures.append(f"{pdu.name}: only {len(got)} frames")
            results.append(r)
            continue

        checker = codec.CounterChecker(DEFS.protection.counter_max_delta)
        bad = []
        for _, data in got:
            d = codec.unpack(DEFS, pdu.can_id, data)
            st = d.status if d.status != codec.OK else codec.check_values(d)
            cst, lost = checker.check(d.counter) if d.status == codec.OK else (st, 0)
            if st != codec.OK or cst != codec.OK or lost:
                bad.append((st, cst, lost))

        win = [t for t, _ in got[WARMUP:WARMUP + INTERVALS + 1]]
        iv = [b - a for a, b in zip(win, win[1:])]
        dev = sorted(abs(x - nominal) for x in iv)
        mean = statistics.fmean(iv)
        r.update({
            "intervals": len(iv),
            "mean_ms": round(mean / 1e6, 4),
            "mean_error_pct": round((mean - nominal) / nominal * 100, 3),
            "p99_abs_dev_ms": round(dev[int(len(dev) * 0.99) - 1] / 1e6, 4),
            "max_ms": round(max(iv) / 1e6, 4),
            "min_ms": round(min(iv) / 1e6, 4),
            "within_2ms_pct": round(sum(d <= 2e6 for d in dev) / len(dev) * 100, 2),
            "protocol_errors": len(bad),
        })
        results.append(r)
        if abs(r["mean_error_pct"]) > 1.0:
            failures.append(f"{pdu.name}: mean error {r['mean_error_pct']} %")
        if r["within_2ms_pct"] < 99.0:
            failures.append(f"{pdu.name}: only {r['within_2ms_pct']} % within +-2 ms")
        if max(iv) > 2 * nominal:
            failures.append(f"{pdu.name}: interval {r['max_ms']} ms > 2x period")
        if bad:
            failures.append(f"{pdu.name}: {len(bad)} frames failed checks, first {bad[0]}")

    report = {"date": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "kernel": os.uname().release,
              "ecu_ns": ecu_ns, "ecu_iface": ecu_if, "rx_iface": rx_if, "results": results,
              "unexpected_ids": unexpected, "ecu_logs": logs, "failures": failures}
    print(json.dumps(report, indent=2, ensure_ascii=False))
    out = os.environ.get("SIL_RESULT_FILE")
    if out:
        Path(out).write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    assert not unexpected, f"unexpected CAN IDs {unexpected}"
    assert not failures, failures
