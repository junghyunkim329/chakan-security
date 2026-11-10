#!/usr/bin/env bash
# Phase 4 시나리오 시험: Execution Manager가 ECU·Gateway·AP를 띄우고, 종료·응답 정지·반복 실패를 주입해 판정한다.
# 사용 (root, practice/scripts/setup.sh 실행 후):  ECU/scripts/run/em-scenario-test.sh
# 결과: docs/results/<시각>_em/summary.md
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
RUN_AS=$(stat -c %U "$ROOT")

[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }
for b in execution_manager ecu_engine ecu_brake ecu_steering vehicle_gateway vehicle_state_consumer; do
	[[ -x "$ROOT/build/$b" ]] || { echo "빌드가 없습니다: $b (ghwls로 cmake --build build 먼저)"; exit 1; }
done
for ns in hj_ap hj_gw hj_veh; do
	ip netns list | awk '{print $1}' | grep -qxF "$ns" || { echo "$ns 가 없습니다: practice/scripts/setup.sh 먼저"; exit 1; }
done

OUT="$ROOT/docs/results/$(date +%Y%m%d-%H%M%S)_em"
mkdir -p "$OUT"
# 배포: EM은 다른 사용자가 고칠 수 있는 실행 파일을 거부한다(§8.1). 빌드 결과물을
# root 소유·쓰기 불가(0755) 폴더로 복사하고 그 복사본만 실행한다. 실차의 읽기 전용 설치 영역에 해당.
BIN="$OUT/bin"
install -d -m 0755 -o root -g root "$BIN"
for b in execution_manager ecu_engine ecu_brake ecu_steering vehicle_gateway vehicle_state_consumer; do
	install -m 0755 -o root -g root "$ROOT/build/$b" "$BIN/$b"
done
sha256sum "$BIN"/* > "$OUT/deployed-sha256.txt"

echo "Phase 4 시나리오 시험 시작: 약 1분 걸립니다. Ctrl+C를 누르지 말고 기다려 주세요."

rc=0
python3 -I "$ROOT/ECU/tests/integration/em_scenarios.py" "$OUT" "$ROOT" "$RUN_AS" "$BIN" || rc=$?
# 시험이 끝났으므로 결과 폴더(배포 복사본 포함)를 작업 폴더 주인에게 돌려줘 나중에 지울 수 있게 한다
chown -R "$RUN_AS": "$OUT"
echo "결과: $OUT/summary.md (종료코드 $rc)"
exit "$rc"
