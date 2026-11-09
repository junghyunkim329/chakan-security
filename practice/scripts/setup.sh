#!/usr/bin/env bash
# 연습용 차량 SIL 네트워크 설치 스크립트 (1~4단계를 한 번에)
#
#   [hj_ap] ══ veth ══ [hj_gw] ── vxcan ── [hj_veh]
#   10.10.0.10         10.10.0.1  │
#                                 └─ vxlin ── [hj_body]   (Phase 6 LIN 차체 도메인: Door·Window·Light)
#
# 여러 번 실행해도 결과가 같다: 먼저 cleanup.sh로 지운 뒤 처음부터 다시 만든다.
# 사용: su - 로 root가 된 뒤  /home/ghwls/work/practice/scripts/setup.sh

set -euo pipefail   # 중간에 하나라도 실패하면 즉시 멈춘다

# ---- 이름과 주소 (여기만 바꾸면 전체가 바뀐다) ----
NS_AP=hj_ap
NS_GW=hj_gw
NS_VEH=hj_veh
NS_BODY=hj_body
ETH_AP=veth_ap;   IP_AP=10.10.0.10/24
ETH_GW=veth_gw;   IP_GW=10.10.0.1/24
CAN_GW=vxcan_gw
CAN_VEH=vxcan_veh
LIN_GW=vxlin_gw      # LIN 전용선: Gateway(LIN Master) 쪽 끝
LIN_BODY=vxlin_body  # LIN 전용선: 차체 ECU(LIN Slave) 쪽 끝

HERE="$(cd "$(dirname "$0")" && pwd)"        # 이 스크립트가 있는 폴더
FW_DIR="$HERE/../firewall"                     # 방화벽 규칙 파일 폴더

# ---- 0. 준비 ----
[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }

"$HERE/cleanup.sh"                             # 예전 연습 구성이 있으면 먼저 지운다
modprobe vxcan                                 # 가상 CAN 기능 켜기

# ---- 1단계. 방 4개 만들기 + 방 안 내선(lo) 켜기 ----
for ns in "$NS_AP" "$NS_GW" "$NS_VEH" "$NS_BODY"; do
	ip netns add "$ns"
	ip -n "$ns" link set lo up
	echo "[1] 방 생성: $ns"
done

# ---- 2단계. AP <-> Gateway 랜선(veth) ----
ip link add "$ETH_AP" netns "$NS_AP" type veth peer name "$ETH_GW" netns "$NS_GW"
ip -n "$NS_AP" addr add "$IP_AP" dev "$ETH_AP"
ip -n "$NS_GW" addr add "$IP_GW" dev "$ETH_GW"
ip -n "$NS_AP" link set "$ETH_AP" up
ip -n "$NS_GW" link set "$ETH_GW" up
# SOME/IP-SD multicast(224.244.224.245)가 나갈 길 (방 안에만 적용, 본체 라우팅은 그대로)
ip -n "$NS_AP" route add 224.0.0.0/4 dev "$ETH_AP"
ip -n "$NS_GW" route add 224.0.0.0/4 dev "$ETH_GW"
echo "[2] 랜선 연결: $NS_AP/$ETH_AP <-> $NS_GW/$ETH_GW"

# ---- 3단계. Gateway <-> 차량 CAN선(vxcan). AP에는 꽂지 않는다 ----
ip link add "$CAN_GW" netns "$NS_GW" type vxcan peer name "$CAN_VEH" netns "$NS_VEH"
ip -n "$NS_GW" link set "$CAN_GW" up
ip -n "$NS_VEH" link set "$CAN_VEH" up
echo "[3] CAN선 연결: $NS_GW/$CAN_GW <-> $NS_VEH/$CAN_VEH"

# ---- 3-2단계. Gateway <-> 차체 LIN 전용선. AP·차량 방에는 꽂지 않는다 ----
# LIN 물리 신호 대신 가상선(vxcan) 위에 LIN frame을 실어 보낸다 (config/lin/network.yaml encapsulation)
ip link add "$LIN_GW" netns "$NS_GW" type vxcan peer name "$LIN_BODY" netns "$NS_BODY"
ip -n "$NS_GW" link set "$LIN_GW" up
ip -n "$NS_BODY" link set "$LIN_BODY" up
echo "[3] LIN선 연결: $NS_GW/$LIN_GW <-> $NS_BODY/$LIN_BODY"

# ---- 4단계. 방화벽 ----
# 안전장치: 명령이 정말 그 방 안에서 돌 때만 nft를 실행한다.
# (실수로 본체에 걸리면 SSH 접속이 끊기기 때문)
for ns in "$NS_AP" "$NS_GW" "$NS_VEH" "$NS_BODY"; do
	# shellcheck disable=SC2016  # $$와 $1은 방 안의 sh가 해석해야 하므로 작은따옴표가 맞다
	ip netns exec "$ns" sh -c '[ "$(ip netns identify $$)" = "$1" ] && nft -f "$2"' _ "$ns" "$FW_DIR/$ns.nft"
	echo "[4] 방화벽 적용: $ns"
done

echo "설치 완료"
