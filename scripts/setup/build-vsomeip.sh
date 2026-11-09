#!/usr/bin/env bash
# vSomeIP 3.7.6을 작업 폴더 안(third_party/vsomeip)에만 빌드·설치한다. root 불필요, 서버 설정 변경 없음.
#
# 의존성 고정 (project.md §11.1):
#   vSomeIP 3.7.6  commit 4187431d2815bd351845a09d01aa85b2a3163cab (2026-09-17)
#   Boost 1.90.0   Ubuntu 패키지 1.90.0-6ubuntu1을 apt-get download로 받아 third_party/boost-root에 압축만 풂
#                  (패키지 파일 SHA-256: third_party/debs/SHA256SUMS)
#
# 공용 VM 보호 (2026-11-09 동시 4개 컴파일 중 VM이 멈추고 SSH가 끊긴 뒤 추가):
#   - 기본 컴파일 1개씩, CPU 1개만 사용 (taskset), CPU·디스크 우선순위 최저 (nice 19, ionice idle)
#   - 컴파일러 1개당 메모리 상한 (ulimit -v). 넘으면 그 컴파일만 실패하고 VM은 영향 없음
#   - 메모리 감시: VM의 남은 메모리가 LOW_MB 아래로 내려가면 빌드를 일시정지(SIGSTOP)하고
#     HIGH_MB 위로 회복되면 이어서 진행(SIGCONT). 다른 사용자와 SSH가 먼저 메모리를 쓴다
#   - 접속과 분리된 백그라운드로 실행: SSH/VS Code가 끊겨도 빌드는 계속된다
#
# 사용:  scripts/setup/build-vsomeip.sh            백그라운드 시작 후 바로 돌아옴
#        scripts/setup/build-vsomeip.sh --status   진행 상황 보기
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="$ROOT/third_party"
SRC="$T/src/vsomeip-3.7.6"
COMMIT=4187431d2815bd351845a09d01aa85b2a3163cab
BOOST="$T/boost-root/usr"
LOG="$T/vsomeip-build.log"
STATE="$T/vsomeip-build.state"   # running / done / failed
JOBS=${JOBS:-1}                   # 동시 컴파일 수 = 사용 CPU 수
MEM_PER_JOB_MB=${MEM_PER_JOB_MB:-2048}
LOW_MB=${LOW_MB:-1500}            # VM 남은 메모리가 이보다 적으면 빌드 일시정지
HIGH_MB=${HIGH_MB:-2500}          # 이보다 많아지면 다시 진행

[[ $EUID -ne 0 ]] || { echo "root로 실행하지 마세요 (작업 폴더 안에만 설치합니다)"; exit 1; }

status() {
	local s="없음"
	[[ -f "$STATE" ]] && s=$(cat "$STATE")
	echo "상태: $s"
	if [[ -f "$LOG" ]]; then
		grep -Eo '^\[[0-9]+/[0-9]+\]' "$LOG" | tail -1 | sed 's/^/진행: /' || true
		echo "--- 로그 마지막 5줄 ($LOG)"
		tail -5 "$LOG"
	fi
}

if [[ "${1:-}" == "--status" ]]; then
	status
	exit 0
fi

# ---- 1) 백그라운드로 다시 실행하고 바로 돌아온다 ----
if [[ "${SIL_BUILD_CHILD:-}" != "1" ]]; then
	if [[ -f "$STATE" && "$(cat "$STATE")" == "running" ]] && pgrep -f "SIL_BUILD_MARK=vsomeip" >/dev/null; then
		echo "이미 빌드 중입니다."
		status
		exit 0
	fi
	echo running > "$STATE"
	SIL_BUILD_CHILD=1 JOBS="$JOBS" MEM_PER_JOB_MB="$MEM_PER_JOB_MB" LOW_MB="$LOW_MB" HIGH_MB="$HIGH_MB" \
		nohup setsid "$0" SIL_BUILD_MARK=vsomeip >"$LOG" 2>&1 </dev/null &
	echo "백그라운드에서 빌드를 시작했습니다 (동시 ${JOBS}개, CPU ${JOBS}개, 컴파일러당 메모리 ${MEM_PER_JOB_MB}MB)."
	echo "메모리가 ${LOW_MB}MB 아래로 줄면 자동으로 일시정지했다가 이어서 합니다."
	echo "접속이 끊겨도 계속됩니다. 30~60분 걸릴 수 있습니다."
	echo "진행 확인:  $0 --status"
	exit 0
fi

# ---- 2) 실제 빌드 (백그라운드) ----
trap 'echo failed > "$STATE"; echo "빌드 실패"' ERR

if [[ ! -d "$BOOST/include/boost" ]]; then
	echo "Boost가 없습니다: $T/debs의 .deb를 받아 압축을 풉니다"
	mkdir -p "$T/debs" && cd "$T/debs"
	apt-get download libboost1.90-dev=1.90.0-6ubuntu1 libboost-atomic1.90.0=1.90.0-6ubuntu1 \
		libboost-atomic1.90-dev=1.90.0-6ubuntu1 libboost-filesystem1.90.0=1.90.0-6ubuntu1 \
		libboost-filesystem1.90-dev=1.90.0-6ubuntu1
	sha256sum ./*.deb > SHA256SUMS
	mkdir -p "$T/boost-root"
	for f in ./*.deb; do dpkg-deb -x "$f" "$T/boost-root"; done
fi

if [[ ! -d "$SRC" ]]; then
	git clone --depth 1 --branch 3.7.6 https://github.com/COVESA/vsomeip.git "$SRC"
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$COMMIT" ]] || { echo "vSomeIP commit 불일치"; false; }

# 강제 종료로 남은 0바이트 object 파일은 ninja가 "이미 빌드됨"으로 착각하므로 지우고 다시 시작
if [[ -d "$SRC/build" ]] && find "$SRC/build" -name '*.o' -size 0 | grep -q .; then
	echo "중단된 빌드 흔적(0바이트 object)이 있어 build 폴더를 지우고 다시 빌드합니다"
	rm -rf "$SRC/build"
fi

cmake -S "$SRC" -B "$SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$T/vsomeip" \
	-DCMAKE_PREFIX_PATH="$BOOST" -DBoost_ROOT="$BOOST" -DBoost_NO_SYSTEM_PATHS=ON \
	-DCMAKE_INSTALL_RPATH="$T/vsomeip/lib;$BOOST/lib/x86_64-linux-gnu" \
	-DCMAKE_BUILD_RPATH="$BOOST/lib/x86_64-linux-gnu"

mem_avail_mb() { awk '/^MemAvailable:/ {print int($2 / 1024)}' /proc/meminfo; }

echo "빌드 시작 $(date '+%F %T'): 동시 ${JOBS}개, CPU 0-$((JOBS - 1)), 컴파일러당 메모리 ${MEM_PER_JOB_MB}MB," \
	"nice 19, ionice idle, 남은 메모리 ${LOW_MB}MB 미만이면 일시정지"
# 빌드를 자기 프로세스 그룹으로 띄워서 그룹 전체를 멈추거나 이어갈 수 있게 한다
# shellcheck disable=SC2016  # $1~$3은 안쪽 bash가 받는 인자
setsid bash -c 'ulimit -v "$1"; exec taskset -c "0-$(($2 - 1))" nice -n 19 ionice -c 3 cmake --build "$3" -j"$2"' \
	_ $((MEM_PER_JOB_MB * 1024)) "$JOBS" "$SRC/build" &
bpid=$!
sleep 1
pgid=$(ps -o pgid= -p "$bpid" | tr -d ' ')
paused=0
while kill -0 "$bpid" 2>/dev/null; do
	avail=$(mem_avail_mb)
	if [[ $paused -eq 0 && $avail -lt $LOW_MB ]]; then
		kill -STOP -- "-$pgid" 2>/dev/null || true
		paused=1
		echo "$(date '+%T') 남은 메모리 ${avail}MB < ${LOW_MB}MB: 빌드 일시정지"
	elif [[ $paused -eq 1 && $avail -gt $HIGH_MB ]]; then
		kill -CONT -- "-$pgid" 2>/dev/null || true
		paused=0
		echo "$(date '+%T') 남은 메모리 ${avail}MB: 빌드 재개"
	fi
	sleep 2
done
if ! wait "$bpid"; then
	echo "failed" > "$STATE"
	echo "빌드 실패 (메모리 상한 초과면 'virtual memory exhausted'가 로그에 있음)"
	exit 1
fi
cmake --install "$SRC/build"

echo "done" > "$STATE"
echo "vSomeIP 설치 완료 $(date '+%F %T'): $T/vsomeip"
echo "다음: cmake -S $ROOT -B $ROOT/build -G Ninja && cmake --build $ROOT/build"
