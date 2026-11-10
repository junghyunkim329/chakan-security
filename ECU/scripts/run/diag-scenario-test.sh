#!/usr/bin/env bash
# Phase 5 진단 시나리오 시험: AP 테스터 -> DoIP -> Gateway -> ISO-TP -> Engine·Brake UDS 서버
#
#   hj_veh: ecu_engine (--inject-dtc), ecu_brake, can_capture (진단 CAN frame 기록)
#   hj_gw : doip_gateway (TCP/UDP 13400)
#   hj_ap : diag_scenarios.py (테스터)
#
# 실습 키: ~작업폴더주인/.config/vehicle-sil/diag_key (32 byte, 0600). 없으면 이 스크립트가 만든다. 소스에는 넣지 않는다.
# 사용 (root, practice/scripts/setup.sh 실행 후):  ECU/scripts/run/diag-scenario-test.sh
# 결과: docs/results/<시각>_diag/summary.md
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
RUN_AS=$(stat -c %U "$ROOT")
HOME_DIR=$(getent passwd "$RUN_AS" | cut -d: -f6)
KEY="$HOME_DIR/.config/vehicle-sil/diag_key"
B="$ROOT/build"

[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }
for b in ecu_engine ecu_brake doip_gateway; do
	[[ -x "$B/$b" ]] || { echo "빌드가 없습니다: $b (ghwls로 cmake --build build 먼저)"; exit 1; }
done
for ns in hj_ap hj_gw hj_veh; do
	ip netns list | awk '{print $1}' | grep -qxF "$ns" || { echo "$ns 가 없습니다: practice/scripts/setup.sh 먼저"; exit 1; }
done

if [[ ! -f "$KEY" ]]; then
	install -d -m 0700 -o "$RUN_AS" -g "$RUN_AS" "$(dirname "$KEY")"
	tmp=$(mktemp)
	head -c 32 /dev/urandom > "$tmp"
	install -m 0600 -o "$RUN_AS" -g "$RUN_AS" "$tmp" "$KEY"
	rm -f "$tmp"
	echo "실습용 진단 키를 새로 만들었습니다: $KEY (0600)"
fi

OUT="$ROOT/docs/results/$(date +%Y%m%d-%H%M%S)_diag"
mkdir -p "$OUT"
chown "$RUN_AS": "$OUT"
# setpriv는 권한만 낮추고 바로 exec한다(runuser처럼 감독 프로세스가 남지 않음). 그래서 $!가 실제 프로그램 pid이고,
# 시험이 ECU를 SIGSTOP해도 감독 프로세스가 따라 멈춰 정리 단계가 끝없이 기다리는 일이 없다.
as_user() { local ns=$1; shift; ip netns exec "$ns" setpriv --reuid="$RUN_AS" --regid="$RUN_AS" --init-groups --reset-env -- "$@"; }

echo "Phase 5 진단 시나리오 시험 시작: 약 1분 걸립니다. Ctrl+C를 누르지 말고 기다려 주세요."
pids=()
as_user hj_veh python3 -I "$ROOT/ECU/tools/diag/can_capture.py" vxcan_veh "$OUT/can.jsonl" 0x7E0 0x7E8 0x7E1 0x7E9 &
pids+=($!)
as_user hj_veh "$B/ecu_engine" --iface vxcan_veh --diag-key "$KEY" --inject-dtc 2>"$OUT/ecu_engine.log" &
pids+=($!)
as_user hj_veh "$B/ecu_brake" --iface vxcan_veh --diag-key "$KEY" 2>"$OUT/ecu_brake.log" &
pids+=($!)
as_user hj_gw "$B/doip_gateway" --eth-addr 10.10.0.1 --can-iface vxcan_gw >"$OUT/doip_gateway.json" 2>"$OUT/doip_gateway.log" &
pids+=($!)
sleep 1

rc=0
as_user hj_ap python3 -I "$ROOT/ECU/tests/integration/diag_scenarios.py" "$OUT" "$KEY" 2>&1 | tee "$OUT/tester.log" || rc=$?

# 프로세스 정리: 멈춘(SIGSTOP) 프로세스도 끝날 수 있게 SIGCONT 후 SIGTERM, 3초 안에 안 끝나면 SIGKILL
for p in "${pids[@]}"; do kill -CONT "$p" 2>/dev/null || true; kill -TERM "$p" 2>/dev/null || true; done
for _ in $(seq 30); do
	alive=0
	for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null && alive=1; done
	[[ $alive -eq 0 ]] && break
	sleep 0.1
done
for p in "${pids[@]}"; do kill -KILL "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; done

python3 -I "$ROOT/ECU/tests/integration/eval_diag_run.py" "$OUT" > /dev/null || rc=1
cat "$OUT/summary.md"
chown -R "$RUN_AS": "$OUT"
echo "결과: $OUT/summary.md (종료코드 $rc)"
exit "$rc"
