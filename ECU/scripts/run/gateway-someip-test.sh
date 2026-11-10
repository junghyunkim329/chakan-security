#!/usr/bin/env bash
# Phase 3 통합 시험: ECU(CAN) -> Gateway(검사·보관·SOME/IP 제공) -> AP(서비스 발견·구독)
#
#   hj_veh: ecu_engine, ecu_brake, ecu_steering   --vxcan-->
#   hj_gw : vehicle_gateway (vSomeIP provider)    --veth/SOME-IP-->
#   hj_ap : vehicle_state_consumer (CAN 장치 없음)
#
# 모든 프로그램은 root가 아닌 작업 폴더 주인 계정으로 실행한다 (project.md §11.1 non-root).
# 사용 (root, practice/scripts/setup.sh 실행 후):  ECU/scripts/run/gateway-someip-test.sh [초]
# 결과: docs/results/<시각>_someip/summary.md
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DURATION=${1:-30}
WARMUP=3
RUN_AS=$(stat -c %U "$ROOT")
B="$ROOT/build"

[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }
for b in ecu_engine ecu_brake ecu_steering vehicle_gateway vehicle_state_consumer; do
	[[ -x "$B/$b" ]] || { echo "빌드가 없습니다: $b (scripts/setup/build-vsomeip.sh 후 cmake 다시 빌드)"; exit 1; }
done
for ns in hj_ap hj_gw hj_veh; do
	ip netns list | awk '{print $1}' | grep -qxF "$ns" || { echo "$ns 가 없습니다: practice/scripts/setup.sh 먼저"; exit 1; }
done
ip -n hj_gw route show 224.0.0.0/4 | grep -q . || { echo "multicast 경로가 없습니다: practice/scripts/setup.sh를 다시 실행"; exit 1; }

OUT="$ROOT/docs/results/$(date +%Y%m%d-%H%M%S)_someip"
mkdir -p "$OUT"
as_user() { local ns=$1; shift; ip netns exec "$ns" runuser -u "$RUN_AS" -- "$@"; }

echo "Phase 3 통합 시험 시작: 약 $((DURATION + 6))초 걸립니다. Ctrl+C를 누르지 말고 기다려 주세요."
pids=()
for e in engine brake steering; do
	as_user hj_veh "$B/ecu_$e" --iface vxcan_veh --duration-s $((DURATION + 5)) 2>"$OUT/ecu_$e.log" &
	pids+=($!)
done
sleep 1
as_user hj_gw env VSOMEIP_CONFIGURATION="$ROOT/config/vsomeip/gateway.json" \
	"$B/vehicle_gateway" --iface vxcan_gw --duration-s $((DURATION + 3)) >"$OUT/gateway.json" 2>"$OUT/gateway.log" &
pids+=($!)
sleep 1
as_user hj_ap env VSOMEIP_CONFIGURATION="$ROOT/config/vsomeip/consumer.json" \
	"$B/vehicle_state_consumer" --duration-s "$DURATION" --warmup-s "$WARMUP" >"$OUT/consumer.json" 2>"$OUT/consumer.log" &
pids+=($!)

for p in "${pids[@]}"; do wait "$p" || true; done

rc=0
python3 -I "$ROOT/ECU/tests/integration/eval_someip_run.py" "$OUT" "$DURATION" "$WARMUP" || rc=$?
chown -R "$RUN_AS": "$OUT"
echo "결과: $OUT/summary.md (종료코드 $rc)"
exit "$rc"
