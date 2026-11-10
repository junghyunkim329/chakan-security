#!/usr/bin/env python3
"""Phase 5 diagnostic scenarios from the AP side: DoIP -> Gateway -> ISO-TP -> UDS (project.md §9, §15.2).

usage (in hj_ap as the run user): diag_scenarios.py <out_dir> <key_file>
Writes <out_dir>/tester.json. Engine is started with --inject-dtc; brake is stopped/killed by this script.
"""
import json
import os
import signal
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from diag.doip_tester import DoIPTester, security_key, vehicle_identification  # noqa: E402

OUT, KEY = Path(sys.argv[1]), Path(sys.argv[2]).read_bytes()
GW, BCAST = "10.10.0.1", "10.10.0.255"
ENGINE, BRAKE = 0x1001, 0x1002
results = []


def check(group, name, ok, value):
    results.append({"group": group, "name": name, "pass": bool(ok), "value": str(value)})
    print(f"{'PASS' if ok else 'FAIL'}  [{group}] {name}: {value}", flush=True)


def nrc(r, sid, code):
    return r.response == bytes([0x7F, sid, code])


def hx(r):
    return f"ack={r.ack}{'' if r.nack_code is None else f'({r.nack_code:#04x})'} resp={r.response.hex() if r.response else None}"


def find_pid(needle):
    for p in Path("/proc").iterdir():
        if p.name.isdigit():
            try:
                cmd = (p / "cmdline").read_bytes().replace(b"\0", b" ").decode()
            except OSError:
                continue
            if needle in cmd and "setpriv" not in cmd and "python" not in cmd:
                return int(p.name)
    return None


def main():
    # ---- connection and access control on the DoIP side ----
    vid = vehicle_identification(BCAST)
    check("정상", "차량 식별 (UDP broadcast)", vid and vid.get("logical_address") == 0x1000 and len(vid.get("vin", "")) == 17,
          vid)

    t = DoIPTester(GW)
    code, closed = t.routing_activation(sa=0x0E81)
    check("권한 부족", "등록되지 않은 tester 주소 0x0E81 routing 거부", code == 0x00 and closed, f"code={code} closed={closed}")
    t.close()

    t = DoIPTester(GW)
    r = t.request(ENGINE, b"\x22\xf1\x00", 2)
    check("권한 부족", "routing activation 전 진단 요청 거부", r.ack == "nack" and r.nack_code == 0x02 and r.closed, hx(r))
    t.close()

    t = DoIPTester(GW)
    code, closed = t.routing_activation()
    check("정상", "tester 0x0E80 routing activation", code == 0x10 and not closed, f"code={code}")

    # ---- normal requests in the Default session ----
    r = t.request(ENGINE, b"\x22\xf1\x00")
    rpm = int.from_bytes(r.response[3:5], "big") if r.positive and len(r.response) == 5 else None
    check("정상", "Engine F100 엔진 회전수", rpm is not None and 800 <= rpm <= 2500, f"{rpm} rpm, {r.elapsed_ms:.1f} ms")
    r = t.request(BRAKE, b"\x22\xf1\x01")
    spd = int.from_bytes(r.response[3:5], "big") if r.positive and len(r.response) == 5 else None
    check("정상", "Brake F101 차량 속도", spd is not None and 0 <= spd <= 6000, f"{spd and spd / 100} km/h")
    r = t.request(ENGINE, b"\x19\x02\xff")
    dtcs = [r.response[i:i + 4].hex() for i in range(3, len(r.response), 4)] if r.positive else None
    check("정상", "Engine DTC 읽기 (주입한 2개)", dtcs == ["01170009", "03350009"], dtcs)
    r = t.request(ENGINE, b"\x3e\x00")
    check("정상", "TesterPresent", r.response == b"\x7e\x00", hx(r))

    # ---- multi-frame (ISO-TP FF/FC/CF; frames checked again in the CAN capture) ----
    r = t.request(ENGINE, b"\x22\xf1\x80")
    check("다중 프레임", "Engine F180 소프트웨어 버전 (응답 > 7 byte)",
          r.positive and len(r.response) > 7 and b"SIL-ENGINE-ECU" in r.response, f"{len(r.response or b'')} byte")
    r = t.request(ENGINE, b"\x22\xf1\x00\xf1\x02\xf1\x80\xf1\x81")
    check("다중 프레임", "Engine DID 4개 요청 (요청 9 byte)", r.positive and r.response[:3] == b"\x62\xf1\x00", f"{len(r.response or b'')} byte 응답")
    r = t.request(BRAKE, b"\x22\xf1\x01\xf1\x10\xf1\x11\xf1\x80\xf1\x81\xf1\x82")
    check("다중 프레임", "Brake DID 6개 요청 (요청 13 byte)", r.positive and b"SIL-BRAKE-ECU" in r.response, f"{len(r.response or b'')} byte 응답")

    # ---- permission ----
    r = t.request(ENGINE, b"\x14\xff\xff\xff")
    check("권한 부족", "Default 세션 DTC 삭제 거부 (NRC 0x7F)", nrc(r, 0x14, 0x7F), hx(r))
    r = t.request(ENGINE, b"\x27\x01")
    check("권한 부족", "Default 세션 SecurityAccess 거부 (NRC 0x7F)", nrc(r, 0x27, 0x7F), hx(r))
    r = t.request(ENGINE, b"\x10\x03")
    check("정상", "Extended 세션 진입", r.response and r.response[:2] == b"\x50\x03", hx(r))
    r = t.request(ENGINE, b"\x14\xff\xff\xff")
    check("권한 부족", "보안 해제 전 DTC 삭제 거부 (NRC 0x33)", nrc(r, 0x14, 0x33), hx(r))
    r = t.request(ENGINE, b"\x11\x01")
    check("권한 부족", "보안 해제 전 ECUReset 거부 (NRC 0x33)", nrc(r, 0x11, 0x33), hx(r))
    r = t.request(ENGINE, b"\x22\xf1\x01")
    check("권한 부족", "지원하지 않는 DID 거부 (NRC 0x31)", nrc(r, 0x22, 0x31), hx(r))
    r = t.request(ENGINE, b"\x34\x00\x44\x00\x00\x00\x00\x00\x00\x00\x10")
    check("권한 부족", "범위 밖 서비스 RequestDownload 거부 (NRC 0x11)", nrc(r, 0x34, 0x11), hx(r))
    r = t.request(0x1003, b"\x3e\x00", 2)
    check("권한 부족", "알 수 없는 대상 주소 0x1003 (DoIP NACK 0x03)", r.ack == "nack" and r.nack_code == 0x03 and not r.closed, hx(r))

    # brake: three wrong keys -> 10 s delay (§9.4)
    t.request(BRAKE, b"\x10\x03")
    codes = []
    for _ in range(3):
        t.request(BRAKE, b"\x27\x01")
        codes.append(t.request(BRAKE, b"\x27\x02" + b"\x00" * 32).response)
    t_delay = time.monotonic()
    check("권한 부족", "잘못된 key 3회: 0x35, 0x35, 0x36",
          codes == [b"\x7f\x27\x35", b"\x7f\x27\x35", b"\x7f\x27\x36"], [c.hex() if c else None for c in codes])
    r = t.request(BRAKE, b"\x27\x01")
    check("시간 초과", "10초 지연 중 seed 요청 거부 (NRC 0x37)", nrc(r, 0x27, 0x37), hx(r))

    # ---- privileged path on the engine ----
    seed, sk = t.unlock(ENGINE, KEY)
    check("정상", "SecurityAccess (16-byte seed, HMAC-SHA-256)", len(seed.response or b"") == 18 and sk.response == b"\x67\x02", hx(sk))
    r = t.request(ENGINE, b"\x14\xff\xff\xff")
    check("정상", "보안 해제 후 DTC 삭제", r.response == b"\x54", hx(r))
    r = t.request(ENGINE, b"\x19\x02\xff")
    check("정상", "삭제 후 DTC 없음", r.response == b"\x59\x02\x09", hx(r))
    r = t.request(ENGINE, b"\x11\x01")
    check("정상", "ECUReset", r.response == b"\x51\x01", hx(r))
    r = t.request(ENGINE, b"\x22\xf1\x81")
    check("정상", "Reset 후 Default 세션·잠김", r.response == b"\x62\xf1\x81\x01\x00", hx(r))

    # ---- timeouts ----
    t.request(ENGINE, b"\x10\x03")
    t.unlock(ENGINE, KEY)
    time.sleep(5.5)  # no TesterPresent
    r = t.request(ENGINE, b"\x22\xf1\x81")
    check("시간 초과", "TesterPresent 5초 없으면 Default 복귀 (S3)", r.response == b"\x62\xf1\x81\x01\x00", hx(r))
    r = t.request(ENGINE, b"\x14\xff\xff\xff")
    check("시간 초과", "S3 이후 DTC 삭제 거부 (NRC 0x7F)", nrc(r, 0x14, 0x7F), hx(r))

    t.request(ENGINE, b"\x10\x03")
    t.unlock(ENGINE, KEY)
    for _ in range(4):  # keep-alive every 2 s for 8 s
        time.sleep(2)
        t.request(ENGINE, b"\x3e\x80", 0.3)  # suppressed response: only the DoIP ack comes back
    r = t.request(ENGINE, b"\x22\xf1\x81")
    check("시간 초과", "TesterPresent 유지 시 Extended·해제 유지", r.response == b"\x62\xf1\x81\x03\x01", hx(r))

    wait = 10.2 - (time.monotonic() - t_delay)
    if wait > 0:
        time.sleep(wait)
    t.request(BRAKE, b"\x10\x03")  # S3 dropped the brake to Default meanwhile
    r = t.request(BRAKE, b"\x27\x01")
    check("시간 초과", "10초 지연 후 seed 요청 다시 허용", r.response and r.response[:2] == b"\x67\x01" and len(r.response) == 18, hx(r))
    t.request(BRAKE, b"\x27\x02" + b"\x00" * 32)
    t.request(BRAKE, b"\x27\x01")
    r5 = t.request(BRAKE, b"\x27\x02" + b"\x00" * 32)
    r = t.request(BRAKE, b"\x27\x01")
    check("권한 부족", "5회 실패 후 잠금 (NRC 0x36, 재시작 전까지)", nrc(r5, 0x27, 0x36) and nrc(r, 0x27, 0x36), f"{hx(r5)} / {hx(r)}")
    seed, sk = t.unlock(BRAKE, KEY)
    check("권한 부족", "잠금 중에는 올바른 key도 불가", nrc(seed, 0x27, 0x36), hx(seed))

    # ECU stops answering (process stopped): DoIP ack, then no UDS response within the P2* client time
    brake = find_pid("ecu_brake --iface vxcan_veh")
    os.kill(brake, signal.SIGSTOP)
    r = t.request(BRAKE, b"\x22\xf1\x01", 6.0)
    check("시간 초과", "응답 없는 ECU: ack 후 P2* 안에 응답 없음", r.ack == "ack" and r.response is None and r.elapsed_ms >= 5900,
          f"{hx(r)}, {r.elapsed_ms:.0f} ms")
    # ECU gone (process killed): a multi-frame request cannot get Flow Control -> target unreachable
    os.kill(brake, signal.SIGKILL)
    time.sleep(0.5)
    r = t.request(BRAKE, b"\x22\xf1\x01\xf1\x10\xf1\x11\xf1\x80", 4.0)
    check("시간 초과", "없는 ECU에 다중 프레임 요청: Flow Control 시간 초과 → NACK 0x06", r.ack == "nack" and r.nack_code == 0x06,
          f"{hx(r)}, {r.elapsed_ms:.0f} ms")
    r = t.request(ENGINE, b"\x3e\x00")
    check("정상", "다른 ECU는 계속 응답", r.response == b"\x7e\x00", hx(r))
    t.close()

    (OUT / "tester.json").write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
