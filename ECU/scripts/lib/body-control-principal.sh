#!/usr/bin/env bash
# Shared by setup and the LIN runner. Sets variables in the calling script only.
resolve_body_control_principal() {
	if [[ ${BODY_CONTROL_UID+x} || ${BODY_CONTROL_GID+x} ]]; then
		[[ -n ${BODY_CONTROL_UID:-} && -n ${BODY_CONTROL_GID:-} ]] || {
			echo "BODY_CONTROL_UID/GID는 두 값을 함께 지정해야 합니다" >&2; return 1;
		}
	elif [[ $# -eq 2 ]]; then
		BODY_CONTROL_UID=$1
		BODY_CONTROL_GID=$2
	else
		if ! BODY_CONTROL_UID=$(id -u vehicle-body-control) || ! BODY_CONTROL_GID=$(id -g vehicle-body-control); then
			echo "vehicle-body-control 계정이 없습니다. 전용 계정을 준비하거나 BODY_CONTROL_UID/GID를 함께 지정하세요" >&2
			return 1
		fi
	fi
	local value
	for value in "$BODY_CONTROL_UID" "$BODY_CONTROL_GID"; do
		if [[ ! $value =~ ^[0-9]{1,10}$ ]] || ! (( 10#$value > 0 && 10#$value < 4294967296 )); then
			echo "BODY_CONTROL_UID/GID는 root가 아닌 32-bit 10진수여야 합니다" >&2; return 1;
		fi
	done
	BODY_CONTROL_UID=$((10#$BODY_CONTROL_UID))
	BODY_CONTROL_GID=$((10#$BODY_CONTROL_GID))
}
