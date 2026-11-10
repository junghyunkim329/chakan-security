# Phase 4. 실행 및 상태 관리 (호진)

> 상태: 🟢 구현·단위 시험·시나리오 시험 통과 (2026-11-09)
> 날짜: 2026-11-09
> 환경: chakan VM (Ubuntu 26.04.1, kernel 7.0.0-42, g++ 15.2, CMake 4.2.3 + Ninja), Boost 1.90.0, vSomeIP 3.7.6
> 관련: `project.md` §8.1~8.2, §8.3(이번 결정), §11.1, §15.2
> ⚠️ 기준서에 없던 세부값은 **팀 확인 필요**

---

## ✅ 할 일 / 완료 기준

- [x] CAN 프로세스 시작 순서
- [x] heartbeat
- [x] 자원 제한
- [x] 재시작 횟수 제한
- [x] Startup·Running·Degraded·Shutdown 구현검증
- [x] 최신 상태 보관 · validity·freshness 처리 · 서비스 제공·탐색·구독 — **Phase 3에서 완료**

**완료 기준**
- [x] 종료·응답 정지·반복 실패 시나리오에서 상태 전환과 복구 확인 — 시나리오 16개 기준 전부 통과

---

## 1. 한 줄 요약

**Execution Manager(EM)** 가 manifest를 읽어 ECU·Gateway·AP 프로그램을 **정해진 순서로 띄우고**, 각 프로그램이 0.5초마다 보내는 **heartbeat**로 살아 있는지 감시하다가, 죽거나 멈추면 **재시작**하고, 같은 프로그램이 계속 죽으면 **포기하고 Degraded** 상태로 알림.

```
                 manifest (누구를, 어떤 순서로, 어떤 권한·자원으로)
                         │
                 [Execution Manager]  ← 감사 로그 / 상태 파일
          ┌───────┬──────┼────────┬──────────┐
       engine   brake  steering  gateway   consumer
         └──── heartbeat (0.5초마다 "살아 있음") ────┘
```

---

## 2. 구현 내용

### 2.1 시작 순서와 종료 순서
- CAN ECU 3개(engine·brake·steering) → **셋 다 준비되면** Gateway → **Gateway 준비되면** AP consumer
- "준비" = 첫 heartbeat 도착
- 종료는 **역순**: consumer → gateway → ECU. 하나씩 SIGTERM, 2초 안에 안 끝나면 SIGKILL

### 2.2 heartbeat
- 각 프로그램이 **주 작업 루프**에서 500ms마다 EM에 "살아 있음" 전송 (ECU는 CAN 송신 루프, Gateway는 event 발행 루프)
- 루프가 멈추면 heartbeat도 멈춤 → **"프로세스는 살아 있는데 일은 안 하는" 상태(응답 정지)** 를 잡을 수 있음
- EM은 메시지 내용이 아니라 **커널이 알려 주는 보낸 프로세스 번호(pid)** 로 신원 확인 → 다른 프로그램이 대신 "살아 있음"을 보내 속일 수 없음

### 2.3 장애 판정과 재시작 (§8.2)
| 상황 | 판정 | 조치 |
| --- | --- | --- |
| 프로그램이 죽음 | 즉시 | 0.5초 뒤 재시작 |
| heartbeat 3회 연속 없음 (1.5초) | 응답 정지 | 강제 종료 후 재시작 |
| 시작 후 5초 안에 heartbeat 없음 | 시작 실패 | 강제 종료 후 재시작 |
| **같은 프로그램이 60초 안에 3번 장애** | 반복 실패 | **재시작 중지**, Degraded 유지 |

### 2.4 상태 전이
| 상태 | 조건 |
| --- | --- |
| `Startup` | 처음 켜져서 아직 모두 준비되지 않음 |
| `Running` | 5개 모두 정상 |
| `Degraded` | 하나라도 재시작 중이거나 재시작을 포기함 |
| `Shutdown` | 종료 요청을 받음 |

### 2.5 권한과 자원 제한 (§8.1, §11.1)
- EM만 root(네트워크 방에 넣고 권한을 낮추는 데 필요), **각 프로그램은 일반 사용자**
- 모든 프로그램: capability 0, 권한 상승 금지(`no_new_privs`), 보조 그룹 없음
- 시작 직후 `/proc`에서 실제로 적용됐는지 **다시 읽어서 확인**, 어긋나면 강제 종료
- CAN 통신은 특별 권한(`CAP_NET_RAW`) 없이 동작 → 어떤 프로그램에도 주지 않음
- 메모리 상한: ECU 64MB, Gateway·consumer 1024MB
- CPU: ECU는 1번, Gateway는 2번, consumer는 3번 코어만 사용 (0번은 시스템·SSH용)
- 실행 파일 검사: 절대 경로, 다른 사람이 수정할 수 없는 파일인지 등

### 2.6 기록
- **감사 로그(JSONL)**: 시각(timezone 포함), monotonic 시각, 사건 종류, 프로그램, pid, 상세
- **상태 파일(JSON)**: 현재 상태와 프로그램별 상태·pid·재시작 횟수

---

## 3. 이번에 정한 값 (project.md §8.3, 팀 확인 필요)

| 항목 | 결정 | 이유 |
| --- | --- | --- |
| heartbeat 방식 | unix datagram + 커널 pid 확인 | 위조 방지 |
| 재시작 대기 | 0.5초 | 기준 "3초 이내 재시작 시도" 안에서 이전 프로세스 정리 시간 확보 |
| 준비 기준 | 첫 heartbeat | 프로세스가 떠도 초기화 중일 수 있음 |
| 시작 제한 시간 | 5초 | 기준 "service discovery 5초" |
| CPU 제한 방식 | 코어 지정(affinity) + nice | cgroup은 서버 설정을 바꾸므로 사용 안 함 → **사용률 상한은 아님** |
| manifest 형식 | JSON + `${변수}` | 연습(`hj_`)과 공식(`ns_`) 구성에 같은 manifest 사용 |

---

## 4. 시험 결과

### 4.1 단위 시험 (2026-11-09, 직접 실행)

| 시험 | 결과 | 내용 |
| --- | --- | --- |
| `test_supervisor` | ✅ 59 checks | 시작 순서, 종료→즉시 Degraded→3초 안 재시작→Running, 응답 정지 1.5초 감지(±0.1초), 정지된 프로세스가 끝나기 전엔 재시작 안 함, 60초 3회 실패 시 재시작 중지, 60초 넘게 간격을 둔 실패는 계속 재시작, 시작 5초 제한, 가짜 heartbeat 거부, 역순 종료와 SIGKILL, 순환 의존·없는 이름 거부 |
| `test_manifest` | ✅ 23 checks | 프로젝트 manifest 로드, 변수 치환, 잘못된 변수·CPU·메모리·capability·상대 경로·이름·schema·JSON 거부 |
| Phase 2·3 시험 | ✅ | 회귀 없음 (전체 9개 통과) |
| cppcheck / shellcheck | ✅ | 경고 0 |

```
100% tests passed, 0 tests failed out of 9
```

### 4.2 시나리오 시험 (2026-11-09 11:57, `docs/results/20261109-115706_em/`)

**방법**
- `ECU/scripts/run/em-scenario-test.sh`: 빌드 결과물을 root 소유·쓰기 불가 폴더로 **배포**(SHA-256 기록)한 뒤, EM이 그 복사본으로 5개 프로그램을 연습용 방(`hj_*`)에 띄움
- 정상 시작 확인 → brake 강제 종료 → steering 응답 정지(SIGSTOP) → engine 3회 연속 종료 → EM 종료 요청 순서로 장애를 일으키고 감사 로그로 자동 판정
- 환경: kernel 7.0.0-42-generic, 무부하 VM

**결과: PASS (16개 기준 전부 통과)**

| 시나리오 | 기준 | 결과 | 값 |
| --- | --- | --- | --- |
| 시작 | 모든 프로세스 Running | ✅ | 184 ms |
| 시작 | 의존 순서 | ✅ | engine → brake → steering → gateway → consumer |
| 시작 | non-root·capability 0·NoNewPrivs·CPU·netns | ✅ | 5개 모두 uid 1002, CapEff/CapPrm 0, NoNewPrivs 1, ECU cpu1·Gateway cpu2·consumer cpu3, 지정한 방 |
| 시작 | 메모리 상한 적용 | ✅ | ECU 64MB, Gateway·consumer 1024MB |
| 종료 (brake SIGKILL) | 감지 0~500ms | ✅ | 0.2 ms |
| 종료 | Degraded 기록 | ✅ | 감지와 동시 |
| 종료 | 3초 안 재시작 시도 | ✅ | 543 ms |
| 종료 | 새 pid로 Running 복귀 | ✅ | 544 ms (pid 10226 → 10300) |
| 응답 정지 (steering SIGSTOP) | 감지 1~2초 | ✅ | 1363 ms (heartbeat 3회 누락) |
| 응답 정지 | Degraded 기록 | ✅ | 감지와 동시 |
| 응답 정지 | 3초 안 재시작 시도 | ✅ | 512 ms |
| 응답 정지 | 새 pid로 Running 복귀 | ✅ | 1877 ms (pid 10227 → 10323) |
| 반복 실패 (engine 3회) | 재시작 중지 | ✅ | 3 failures within 60 s, engine = Failed |
| 반복 실패 | Degraded 유지 | ✅ | Degraded |
| 종료 요청 | 역순 종료 | ✅ | consumer → gateway → steering → brake (engine은 이미 중지) |
| 종료 요청 | 남은 프로세스 0, Shutdown, EM 종료코드 0 | ✅ | 17 ms |

**쉽게 말하면:** 프로그램이 죽으면 바로 알아채고 0.5초 만에 다시 띄웠고, 멈춘 프로그램은 1.4초 만에 알아채서 강제로 끄고 다시 띄웠다. 같은 프로그램이 계속 죽자 더 이상 살리지 않고 "일부 기능 저하(Degraded)" 상태로 남았다. 종료할 때는 띄운 순서의 반대로 내렸다.

**같이 확인된 것**
- 장애 동안 AP consumer가 받은 event 중 301개에 freshness 0이 표시됨 → ECU가 멈추면 **오래된 값이라는 표시가 AP까지 전달**됨 (Phase 3 기능이 실제 장애에서 동작)
- 감사 로그: start 9, ready 9, verify 9, exit 9, failure 5, restart_scheduled 4, budget_exhausted 1, stop 4, security 0
- 실패한 첫 실행(11:54): 그룹 쓰기 가능한 실행 파일을 EM이 **보안 검사로 거부** → 배포 단계(읽기 전용 복사본) 추가 후 통과

**관찰 사항 (판정에 영향 없음, 추후 확인)**
- consumer 로그에 vSomeIP `Invalid client id` 에러 1건 (GetSnapshot 응답). GetSnapshot 정상 응답·잘못된 요청 거부는 모두 확인됨
- consumer 로그에 `Didn't receive a multicast SD message for 1100ms` 경고 1건 (cyclic offer 1000ms 기준). Phase 3 단독 실행에서는 없었음

## 5. 실행 방법

```bash
# 빌드 + 단위 시험 (ghwls)
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

# 시나리오 시험 (root, 약 1분 — 중간에 Ctrl+C 금지)
su -
/home/ghwls/work/practice/scripts/setup.sh
/home/ghwls/work/scripts/run/em-scenario-test.sh
exit
```

---

## 6. 주장할 수 있는 것 / 없는 것

✅ 주장 가능
- 의존 순서 시작·역순 종료, heartbeat 기반 응답 정지 감지, 재시작 횟수 제한, 4개 상태 전이가 기준서 시간 안에 동작함 (단위 시험 + 실제 프로세스 시나리오 1회)
- 모든 관리 프로세스가 non-root, capability 0, 권한 상승 금지, 지정 CPU·메모리 상한·네트워크 방에서 실행됨 (커널 값으로 확인)
- 수정 가능한 실행 파일은 실행을 거부함

❌ 아직 주장 불가
- 반복 측정에 따른 감지·복구 시간 분포 (이번은 시나리오당 1회)
- 30분 장시간 안정성
- Gateway·consumer 장애 시나리오 (이번엔 ECU만 장애 주입)

⚠️ 한계
- AUTOSAR Execution Management API·Function Group·State Management의 구현이 아님 (교육용 supervisor)
- heartbeat는 "주 루프가 돈다"만 보여 주고 **기능이 올바른지는 보장하지 않음**
- CPU는 코어 지정이지 사용률 상한이 아님
- EM 자신은 감시받지 않음 (실차의 하드웨어 watchdog 없음)
- 일반 Linux 위의 시간이며 실차 FTTI나 기능안전 요구가 아님
