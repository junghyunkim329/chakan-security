#!/usr/bin/env bash
# Phase 6 LIN 차체 도메인 시나리오 시험 (연습 구성 hj_*): 명령·응답, 응답 누락, 복구, sleep/wakeup, parity·checksum
# 사용 (root, practice/scripts/setup.sh 실행 후):  ECU/scripts/run/lin-scenario-test.sh
# 결과: docs/results/<시각>_lin/summary.md
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
RUN_AS=$(stat -c %U "$ROOT")

[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }
for b in ecu_engine lin_slave vehicle_gateway body_control_client; do
	[[ -x "$ROOT/build/$b" ]] || { echo "빌드가 없습니다: $b (ghwls로 cmake --build build 먼저)"; exit 1; }
done
for ns in hj_ap hj_gw hj_veh hj_body; do
	ip netns list | awk '{print $1}' | grep -qxF "$ns" || { echo "$ns 가 없습니다: practice/scripts/setup.sh 먼저"; exit 1; }
done

OUT="$ROOT/docs/results/$(date +%Y%m%d-%H%M%S)_lin"
mkdir -p "$OUT"
chown "$RUN_AS": "$OUT"   # 캡처 프로그램(일반 사용자)이 여기에 기록한다
echo "Phase 6 LIN 시나리오 시험 시작: 약 40초 걸립니다. Ctrl+C를 누르지 말고 기다려 주세요."

rc=0
python3 -I "$ROOT/ECU/tests/integration/lin_scenarios.py" "$OUT" "$ROOT" "$RUN_AS" || rc=$?
chown -R "$RUN_AS": "$OUT"
echo "결과: $OUT/summary.md (종료코드 $rc)"
exit "$rc"
