#!/usr/bin/env bash
# ECU 송신 주기 시험 (Phase 2 완료 기준).
# ECU 3개를 차량 namespace에서 일반 사용자(nobody)로 실행하고, Gateway 쪽 CAN선에서 받아 주기를 잰다.
#
# 사용 (root):  ECU/scripts/run/ecu-period-test.sh [ECU_NS RX_NS ECU_IF RX_IF]
#   기본값은 연습 구성: hj_veh hj_gw vxcan_veh vxcan_gw
# 결과: docs/results/<시각>_ecu-period/report.json
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ECU_NS=${1:-hj_veh}
RX_NS=${2:-hj_gw}
ECU_IF=${3:-vxcan_veh}
RX_IF=${4:-vxcan_gw}

[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }
for b in ecu_engine ecu_brake ecu_steering; do
	[[ -x "$ROOT/build/$b" ]] || { echo "빌드가 없습니다: cmake --build build 먼저 실행"; exit 1; }
done

OUT="$ROOT/docs/results/$(date +%Y%m%d-%H%M%S)_ecu-period"
mkdir -p "$OUT"
echo "송신 주기 시험 시작: 약 25초 걸립니다. Ctrl+C를 누르지 말고 기다려 주세요."

SIL_RUN_CAN_TESTS=1 SIL_ECU_NS="$ECU_NS" SIL_ECU_IFACE="$ECU_IF" SIL_RX_IFACE="$RX_IF" \
SIL_BUILD_DIR="$ROOT/build" SIL_RESULT_FILE="$OUT/report.json" \
	ip netns exec "$RX_NS" python3 -m pytest -q -s -p no:cacheprovider "$ROOT/ECU/tests/integration/test_ecu_period.py" \
	&& rc=0 || rc=$?

# root가 만든 결과 파일을 작업 폴더 주인에게 돌려준다
chown -R --reference="$ROOT" "$ROOT/docs/results" 2>/dev/null || true
echo "결과: $OUT/report.json (pytest 종료코드 $rc)"
exit "$rc"
