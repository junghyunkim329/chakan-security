#!/usr/bin/env bash
# 연습용 차량 SIL 네트워크 설치 스크립트 (1~4단계를 한 번에)
#
#   [ns_ap] ══ veth ══ [ns_gateway] ── vxcan ── [ns_vehicle]
#   10.10.0.10         10.10.0.1  │
#                                      └─ vxlin ── [ns_body]   (Phase 6 LIN 차체 도메인: Door·Window·Light)
#
# 여러 번 실행해도 결과가 같다: 먼저 cleanup.sh로 지운 뒤 처음부터 다시 만든다.
# 사용: root에서 실행. 기본 계정 vehicle-body-control 또는 명시적인 BODY_CONTROL_UID/GID 쌍 사용.

set -euo pipefail   # 중간에 하나라도 실패하면 즉시 멈춘다


# ---- 이름과 주소 (여기만 바꾸면 전체가 바뀐다) ----
NS_AP=ns_ap
NS_GW=ns_gateway
NS_VEH=ns_vehicle
NS_BODY=ns_body
ETH_AP=veth_ap;   IP_AP=10.10.0.10/24
ETH_GW=veth_gw;   IP_GW=10.10.0.1/24
CAN_GW=vxcan_gw
CAN_VEH=vxcan_veh
LIN_GW=vxlin_gw      # LIN 전용선: Gateway(LIN Master) 쪽 끝
LIN_BODY=vxlin_body  # LIN 전용선: 차체 ECU(LIN Slave) 쪽 끝

HERE="$(cd "$(dirname "$0")" && pwd)"        # 이 스크립트가 있는 폴더
FW_DIR="$HERE/../firewall"                     # 방화벽 규칙 파일 폴더
ROOT="$(cd "$HERE/../.." && pwd)"
# shellcheck source-path=SCRIPTDIR source=../../ECU/scripts/lib/body-control-principal.sh
source "$ROOT/ECU/scripts/lib/body-control-principal.sh"
# shellcheck disable=SC2119 # setup은 저장된 값 없이 배포 계정을 조회한다.
resolve_body_control_principal
RUNTIME_DIR=/run/chakan-security
VSOMEIP_RUNTIME="$RUNTIME_DIR/vsomeip"
BODY_REQID=1100
WORKSPACE_UID=$(stat -c %u "$ROOT")
WORKSPACE_GID=$(stat -c %g "$ROOT")
[[ $BODY_CONTROL_UID != "$WORKSPACE_UID" && $BODY_CONTROL_GID != "$WORKSPACE_GID" ]] || {
	echo "BodyControl UID/GID는 다른 AP 프로세스의 workspace UID/GID와 분리해야 합니다"; exit 1;
}

# ---- 0. 준비 ----
[[ $EUID -eq 0 ]] || { echo "root 권한이 필요합니다 (su - 후 실행)"; exit 1; }

"$HERE/cleanup.sh"                             # 예전 연습 구성이 있으면 먼저 지운다
modprobe vxcan                                 # 가상 CAN 기능 켜기

# 배포 계정에서 조회하거나 명시한 principal을 root-owned runtime config에 기록한다.
install -d -m 0755 "$RUNTIME_DIR" "$VSOMEIP_RUNTIME"
install -m 0600 /dev/null "$RUNTIME_DIR/body-control-principal"
printf '%s %s\n' "$BODY_CONTROL_UID" "$BODY_CONTROL_GID" >"$RUNTIME_DIR/body-control-principal"
python3 -I "$ROOT/ECU/tools/security/render_body_vsomeip.py" \
	"$ROOT/config/vsomeip/body_client.json" "$VSOMEIP_RUNTIME/body_client.json" "$BODY_CONTROL_UID" "$BODY_CONTROL_GID"
python3 -I "$ROOT/ECU/tools/security/render_body_vsomeip.py" \
	"$ROOT/config/vsomeip/body_client_untrusted.json" "$VSOMEIP_RUNTIME/body_client_alt_id.json" "$BODY_CONTROL_UID" "$BODY_CONTROL_GID"

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

# ---- 2-2단계. BodyControl TCP/30512 전용 IPsec transport policy ----
# 방향별 160-bit AES-GCM key(128-bit key + 32-bit salt)는 이 shell 안에서만 유지하고 파일/로그에 쓰지 않는다.
KEY_AP_GW=$(openssl rand -hex 20)
KEY_GW_AP=$(openssl rand -hex 20)
SPI_AP_GW=0x110001
SPI_GW_AP=0x110002
for ns in "$NS_AP" "$NS_GW"; do
	ip -n "$ns" xfrm state add src 10.10.0.10 dst 10.10.0.1 proto esp spi "$SPI_AP_GW" reqid "$BODY_REQID" \
		mode transport aead 'rfc4106(gcm(aes))' "0x$KEY_AP_GW" 128
	ip -n "$ns" xfrm state add src 10.10.0.1 dst 10.10.0.10 proto esp spi "$SPI_GW_AP" reqid "$BODY_REQID" \
		mode transport aead 'rfc4106(gcm(aes))' "0x$KEY_GW_AP" 128
done
ip -n "$NS_AP" xfrm policy add dir out src 10.10.0.10/32 dst 10.10.0.1/32 proto tcp dport 30512 \
	tmpl src 10.10.0.10 dst 10.10.0.1 proto esp reqid "$BODY_REQID" mode transport level required
ip -n "$NS_AP" xfrm policy add dir in src 10.10.0.1/32 dst 10.10.0.10/32 proto tcp sport 30512 \
	tmpl src 10.10.0.1 dst 10.10.0.10 proto esp reqid "$BODY_REQID" mode transport level required
ip -n "$NS_GW" xfrm policy add dir in src 10.10.0.10/32 dst 10.10.0.1/32 proto tcp dport 30512 \
	tmpl src 10.10.0.10 dst 10.10.0.1 proto esp reqid "$BODY_REQID" mode transport level required
ip -n "$NS_GW" xfrm policy add dir out src 10.10.0.1/32 dst 10.10.0.10/32 proto tcp sport 30512 \
	tmpl src 10.10.0.1 dst 10.10.0.10 proto esp reqid "$BODY_REQID" mode transport level required
unset KEY_AP_GW KEY_GW_AP
echo "[2] BodyControl 보호 통신: TCP/30512, XFRM reqid $BODY_REQID"

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
	if [[ $ns == "$NS_AP" ]]; then
		ip netns exec "$ns" sh -c '[ "$(ip netns identify $$)" = "$1" ] && nft -D body_uid="$3" -f "$2"' \
			_ "$ns" "$FW_DIR/$ns.nft" "$BODY_CONTROL_UID"
	else
		ip netns exec "$ns" sh -c '[ "$(ip netns identify $$)" = "$1" ] && nft -f "$2"' _ "$ns" "$FW_DIR/$ns.nft"
	fi
	echo "[4] 방화벽 적용: $ns"
done

echo "설치 완료"
