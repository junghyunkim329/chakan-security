#!/usr/bin/env bash
# 연습용 차량 SIL 네트워크 정리 스크립트
# 방을 지우면 그 안의 랜선(veth), CAN선(vxcan), 방화벽도 함께 사라진다.
# 여러 번 실행해도 안전하다: 없는 방은 건너뛴다.
# 연습용 hj_ 방만 지운다. 팀 공용 ns_ 방은 절대 건드리지 않는다.

set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }

for ns in hj_ap hj_gw hj_veh hj_body; do
	if ip netns list | awk '{print $1}' | grep -qxF "$ns"; then
		ip netns delete "$ns"
		echo "[정리] 방 삭제: $ns"
	else
		echo "[정리] 없음, 건너뜀: $ns"
	fi
done
