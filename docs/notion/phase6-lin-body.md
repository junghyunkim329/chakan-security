# Phase 6. LIN 차체 도메인 (호진, 연습 구성 hj_*)

> 상태: 🟢 구현·단위 시험·시나리오 시험 통과 (2026-11-09)
> 날짜: 2026-11-09
> 환경: chakan VM (Ubuntu 26.04.1, kernel 7.0.0-42, g++ 15.2, CMake 4.2.3 + Ninja), vSomeIP 3.7.6
> 관련: `project.md` §6(LIN), §7.3(BodyControl)
> ⚠️ **연습 구성(`hj_*`)에서만 구현.** 팀 공식 문서(project.md)와 공용 구성(`ns_*`)은 변경하지 않음. 이번 결정은 이 문서에만 기록 (팀 확인 필요)

---

## ✅ 할 일 / 완료 기준

- [x] Gateway의 Master와 Door·Window·Light Slave 구현
- [x] 스케줄·parity·checksum·sleep/wakeup·BodyControl 연결

**완료 기준**
- [x] 명령·응답 — 16/16
- [x] 응답 누락 — 4/4
- [x] 장애 제거 후 복구 확인 — 4/4 (LIN 프로토콜 11/11 포함, 전체 35/35)

---

## 1. 한 줄 요약

Gateway가 **LIN Master**가 되어 10ms 단위 시간표대로 차체 ECU(문·창문·실내등)에 차례로 말을 걸고, 중앙 컴퓨터(AP)의 "창문 열어", "실내등 켜" 요청을 **BodyControl 서비스(SOME/IP)** 로 받아 검사한 뒤 LIN 명령으로 바꿔 전달함.

```
[AP] ──SOME/IP BodyControl──> [Gateway: LIN Master] ──LIN 전용선──> [차체 방 hj_body: Door · Window · Light]
     <── BodyStatusChanged ───                       <── 상태 응답 ───
```

---

## 2. 쉽게 이해하기

| 용어 | 쉬운 뜻 |
| --- | --- |
| LIN | 문·창문·실내등처럼 **느리고 단순한 부품용 값싼 통신선** (19.2kbit/s) |
| Master / Slave | **Master(Gateway)만 말을 시작**하고, Slave는 자기 차례에 불렸을 때만 대답함. 반장이 출석 부르듯이 |
| schedule | Master의 **출석부 순서표**. 10ms마다 한 칸씩: 창문명령 → 창문상태 → 조명명령 → 조명상태 → 문상태 → 창문명령 → 창문상태 → 쉼 ×3 (100ms 반복) |
| header / response | Master가 "0x12번 대답해"(header)라고 부르면 해당 Slave가 데이터(response)로 대답 |
| parity | header 번호에 붙는 **오타 검사 비트 2개**. 틀리면 Slave가 무시 |
| checksum | 응답 끝에 붙는 **검사 숫자**. 틀리면 Master가 버림 |
| sleep / wakeup | 5초 동안 아무 일 없으면 **버스를 재움**(전력 절약), 명령이 오면 깨우고 출석부 처음부터 |

**왜 보안에서 중요한가:** 창문·문은 사람 안전과 직결됨. 그래서 AP의 요청을 Gateway가 **허용된 요청자인지, 값이 범위 안인지, 너무 자주 오지 않는지, 지금 해도 되는 상태인지** 확인한 뒤에만 LIN 명령으로 바꿈. AP는 LIN선에 직접 접근할 수 없음.

---

## 3. 구현 내용

### 3.1 구성 (연습 구성)
| 방 | 프로그램 | 비고 |
| --- | --- | --- |
| `hj_body` (**신규**) | `lin_slave --role door / window / light` | IP 없음, LIN 전용선만 연결, 방화벽 전부 차단 |
| `hj_gw` | `vehicle_gateway --lin-iface vxlin_gw` | LIN Master + BodyControl 제공 (VehicleState와 같은 프로세스) |
| `hj_ap` | `body_control_client` | 시험용 SOME/IP 클라이언트, CAN·LIN 접근 없음 |
| `hj_veh` | `ecu_engine` | 점화 상태 판단용 |

- `practice/scripts/setup.sh`가 방을 **4개** 만들고 Gateway ↔ 차체 방 사이에 LIN 전용선(`vxlin_gw` ↔ `vxlin_body`)을 연결

### 3.2 LIN (§6)
- 정의 파일 하나: `config/lin/network.yaml` → C 코드 자동 생성 (Master·Slave·시험이 같은 정의 사용)
- 10ms slot, 100ms 주기 스케줄 (§6.2 그대로)
- protected ID parity (P0, P1), enhanced checksum (§6.2 공식 그대로, LIN 규격 예제 값으로 검증)
- Slave 동작 (§6.3): 창문 50ms마다 최대 5%, 실내등 fade 100ms마다 10%, 취소·장애물 시 즉시 정지, 장애물은 fault 표시
- 응답 3회 연속 누락(또는 checksum 오류) → 통신 오류, 정상 3회 → 복구
- 5초간 명령·상태 변화 없음 → sleep, 명령 시 wakeup → 100ms 뒤 0ms slot부터 재시작

### 3.3 BodyControl (§7.3)
| 검사 | 결과 코드 |
| --- | --- |
| 허용된 클라이언트(ID 0x2002)인가 | 아니면 2 + 보안 이벤트 |
| payload 1바이트, 창문 0~100, 실내등 0/1 | 아니면 1 |
| 메서드별 초당 5회 이하 | 넘으면 2 + 보안 이벤트 |
| 창문: 점화 ON (엔진 메시지가 valid·fresh) | 아니면 2 |
| 대상 Slave가 통신 오류인가 | 그러면 3 |
| LIN 명령 전송 후 Slave가 다음 상태로 응답 (500ms 안) | 0, 아니면 3 |

- `BodyStatusChanged` event: 100ms마다, 20바이트 (문·잠금, 창문 위치, 실내등, 장애·통신오류·sleep 플래그, Slave별 fresh)

---

## 4. 이번에 정한 값 (연습 구성, 팀 확인 필요)

project.md에 없어서 정한 값. 팀 공식 문서에는 반영하지 않음.

| 항목 | 결정 | 이유 |
| --- | --- | --- |
| Slave 위치 | 새 차체 방 `hj_body` | AP가 LIN에 접근 못 하게 (Phase 1과 같은 격리 원칙) |
| LIN 전송 | 전용 vxcan 위에 header(ID 0x001)·response(0x002)·wakeup(0x003)을 실어 보냄 | 물리 LIN 장비 없음. 다른 통신과 섞이지 않는 전용선 |
| LIN 프로세스 | Gateway 프로세스 안의 LIN Master 스레드 | §4.1 Gateway 구성요소, 점화 상태(CAN)와 BodyControl을 함께 판단 |
| 응답 대기 | header 후 5ms | 19.2kbit/s에서 3바이트 응답이 약 2~3ms |
| wakeup 후 대기 | 100ms | LIN 2.x 최소값 |
| Slave 버스 무활동 sleep | 4초 | LIN 2.x |
| 창문 명령 거부 조건 | 점화 OFF 또는 엔진 메시지가 valid·fresh가 아님 | 실차에서 창문은 점화 상태에 묶이는 경우가 일반적. 실내등은 항상 허용 |
| rate limit | 메서드별 초당 5회 | 사람이 누르는 속도보다 충분히 크고 반복 공격은 막음 |
| 허용 목록 | SOME/IP client ID 0x2002 | 시험 클라이언트. client ID는 위조 가능하므로 실제 인증이 아님 |
| 실내등 명령 | 1 → 밝기 100 fade, 0 → 0 fade | §7.3은 0/1만 정함 |
| event 주기 | 100ms | LIN 한 주기 |

---

## 5. 시험 결과

### 5.1 단위 시험 (2026-11-09, 직접 실행)

| 시험 | 결과 | 내용 |
| --- | --- | --- |
| `test_lin` | ✅ 268 checks | parity 표준 표(0x10→0x50 등, 64개 전부), LIN 규격 checksum 예제(0xE6), 스케줄 표, Door 초기값·입력, 창문 5% 이동·취소·장애물·checksum 오류 무시, 실내등 fade, 응답 중단·손상 주입, parity 오류 무시, 4초 버스 sleep |
| `test_lin_master` | ✅ 113 checks | **실제 Slave 로직과 Master를 1ms 단위 가상 버스로 연결**: 스케줄 순서·10ms·40ms 간격, 창문·실내등·문 상태 반영, 3회 누락 → 통신 오류 → 2회로는 미복구 → 3회 복구, checksum 오류 누적, 5초 sleep·버스 조용함·wakeup 100ms 후 0x11부터, BodyControl 허용 목록·길이·범위·점화·rate limit·확인 응답·LIN timeout |
| 이전 Phase 시험 | ✅ | 전체 13개 통과 |
| cppcheck / shellcheck | ✅ | 경고 0 |

### 5.2 시나리오 시험 (실행 대기)

`scripts/run/lin-scenario-test.sh`: 실제 프로세스를 띄우고 AP 클라이언트로 요청, Slave에 장애를 주입, LIN선을 캡처해서 자동 판정 (약 40초).

| 분류 | 주요 시험 |
| --- | --- |
| 명령·응답 | 서비스 발견, 초기 상태, 창문 50 (5%씩, 약 0.5초), 실내등 켜기, 문 열림 반영, 잘못된 인자 3종, 장애물 정지, 초당 5회 초과 거부, 엔진 정지 시 창문 거부·실내등 허용, 엔진 재개 후 허용, 허용 목록 밖 클라이언트 거부 |
| 응답 누락 | Window 응답 중단 → 통신 오류, 그 상태에서 명령 → LIN timeout(3), 다른 Slave 정상 유지, Light checksum 손상 → 통신 오류 |
| 복구 | Window 재개 → 복구 후 명령 동작, Light 정상화 → 복구 후 명령 동작 |
| LIN 프로토콜 (LIN선 캡처) | parity, 스케줄 순서와 10ms 간격, checksum 오류는 주입 구간에만, parity 틀린 header 무응답, 5초 sleep·sleep 중 header 0개·wakeup 후 0x11부터, 실내등 fade 10% |

**결과: PASS (35/35), 2026-11-09 13:17, `docs/results/20261109-131719_lin/`**

| 분류 | 통과 |
| --- | --- |
| 명령·응답 | 16/16 |
| 응답 누락 | 4/4 |
| 복구 | 4/4 |
| LIN 프로토콜 | 11/11 |

**명령·응답**
| 시험 | 결과 |
| --- | --- |
| AP가 BodyControl 발견 | ✅ 201 ms |
| 초기 상태 (문 닫힘·중앙 잠금, 창문 0, 실내등 꺼짐, 모두 fresh) | ✅ |
| SetWindowPosition(50) | ✅ accepted, 응답 52 ms |
| 창문 5%씩 이동해 50 도달 | ✅ 0.64 s, event마다 +10% (100 ms = 50 ms × 2회) |
| SetInteriorLight(1) | ✅ accepted, 0.19 s 후 켜짐 |
| 앞좌 문 열림 입력 → event 반영 | ✅ 0.09 s |
| 잘못된 인자 (101, 2바이트, 실내등 2) | ✅ 모두 invalid argument(1) |
| 장애물 주입 | ✅ 창문 정지, obstruction·fault 표시 |
| 초당 5회 초과 (7회 연속) | ✅ 5개 accepted, 2개 거부(2) |
| 엔진 정지(점화 정보 stale) | ✅ 창문 거부(2), 실내등은 허용(0) |
| 엔진 재개 | ✅ 창문 허용(0) |
| 허용 목록 밖 client ID 0x2FFF | ✅ 거부(2) + 보안 이벤트 |

**응답 누락**
| 시험 | 결과 |
| --- | --- |
| Window 응답 중단 → 통신 오류·not fresh | ✅ 0.23 s |
| 통신 오류 중 창문 명령 | ✅ LIN timeout(3), 11 ms (LIN 명령 만들지 않음) |
| Door·Light는 정상 유지 | ✅ |
| Light checksum 손상 → 통신 오류 | ✅ 0.28 s |

**복구**
| 시험 | 결과 |
| --- | --- |
| Window 응답 재개 → 복구·fresh | ✅ 0.20 s |
| 복구 후 창문 명령 | ✅ accepted, 이동 확인 |
| Light 정상화 → 복구 | ✅ 0.29 s |
| 복구 후 실내등 명령 | ✅ accepted |

**LIN 프로토콜 (LIN선 캡처 1412 header)**
| 시험 | 결과 |
| --- | --- |
| 모든 header PID parity가 표준 표와 일치 (주입 1건 제외) | ✅ |
| 스케줄 순서 0x11→0x12→0x13→0x14→0x10→0x11→0x12 반복 | ✅ |
| slot 간격 10 ms, idle 40 ms (±3) | ✅ 최소 9.0 / 최대 40.9 ms |
| checksum 오류는 손상 주입 구간에만 | ✅ 7 / 1292 응답, 구간 밖 0 |
| parity가 틀린 header(0x10)에 Door 무응답 + 기록 | ✅ Slave 3개 모두 bad_parity=1 |
| 5초 무활동 → sleep | ✅ 4.24 s (마지막 상태 변화 기준 5 s) |
| sleep 중 LIN 버스 | ✅ 1초 동안 header 0개 |
| sleep 중 명령 → wakeup → accepted | ✅ 응답 123 ms |
| wakeup 후 100 ms 지나 0 ms slot(0x11)부터 재시작 | ✅ 100.1 ms |
| 실내등 fade 100 ms마다 10% | ✅ |

**Gateway 보고 요약:** LIN sleep 1회, 보안 이벤트 4건(허용 목록 1, 잘못된 길이 1, rate limit 2), Window 누락 10·복구 1, Light checksum 오류 7·복구 1, 엔진 0x100 거부 1건(엔진 재시작 후 번호표 재동기화, 정상 동작).

**쉽게 말하면:** AP가 창문을 열라고 하면 Gateway가 확인한 뒤 LIN으로 전달해 창문이 5%씩 열렸고, 이상한 요청·너무 잦은 요청·허용 안 된 요청자·시동 정보가 없을 때의 창문 요청은 막혔다. 창문 ECU가 대답을 안 하거나 실내등 ECU가 틀린 데이터를 보내면 0.3초 안에 "통신 오류"로 표시됐고, 고쳐지면 다시 정상으로 돌아왔다. 5초간 아무 일 없으면 LIN이 잠들고 명령이 오면 깨어났다.

**참고:** rate limit 결과 목록 `[2, 2, 0, 0, 0, 0, 0]`은 **응답이 도착한 순서**임. 거부는 바로 답하고 accepted는 Slave 확인을 기다렸다 답하기 때문에 거부 2건이 먼저 도착함 (요청 순서로는 6·7번째가 거부).

## 6. 실행 방법

```bash
# 빌드 + 단위 시험 (ghwls)
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

# 시나리오 시험 (root, 약 40초 — 중간에 Ctrl+C 금지)
su -
/home/ghwls/work/practice/scripts/setup.sh
/home/ghwls/work/scripts/run/lin-scenario-test.sh
exit
```

---

## 7. 주장할 수 있는 것 / 없는 것

✅ 주장 가능 (연습 구성, 시나리오 1회 + 단위 시험)
- §6.2 스케줄·parity·checksum, §6.3 Slave 동작·누락·복구, sleep/wakeup이 실제 프로세스에서 동작
- BodyControl의 허용 목록·범위·rate limit·점화 상태 검사와 LIN timeout 응답
- AP → SOME/IP → Gateway → LIN → 차체 ECU 명령 경로와 반대 방향 상태 event

❌ 아직 주장 불가
- 반복 실행에 따른 시간 분포 (1회 측정)
- 공식 구성(`ns_*`)에서의 동작

⚠️ 한계 (§6.2 명시)
- UART break, sync 파형, 전압, 실제 baud rate·타이밍, EMC는 재현하지 않음 (논리 동작만)
- Slave가 먼저 깨우는 wakeup(문 열림 등)은 구현하지 않음 → sleep 중 문 상태 변화는 다음 wakeup 때 반영
- 진단 frame(0x3C/0x3D)·UDS-on-LIN은 범위 밖
- 허용 목록의 client ID는 위조 가능 → 실제 요청자 인증이 아님
- 연습 구성(`hj_*`) 전용. 공식 구성(`ns_*`)에는 차체 방이 없음
