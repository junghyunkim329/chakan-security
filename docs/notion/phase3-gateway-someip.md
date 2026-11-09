# Phase 3. Gateway와 SOME/IP (호진)

> 상태: 🟢 구현·단위 시험·통합 시험 통과 (2026-11-09)
> 날짜: 2026-11-08 ~ 2026-11-09
> 환경: chakan VM (Ubuntu 26.04.1, kernel 7.0.0-38, g++ 15.2, CMake 4.2.3 + Ninja), vSomeIP 3.7.6, Boost 1.90.0
> 관련: `project.md` §5.4, §7.1~7.2, §7.5(이번 결정), §15.2
> ⚠️ 기준서에 없던 세부값은 **팀 확인 필요**

---

## ✅ 할 일 / 완료 기준

- [x] CAN 입력 검증
- [x] 최신 상태 보관
- [x] validity·freshness 처리
- [x] 서비스 제공·탐색·구독 구현

**완료 기준**
- [x] CAN 입력이 SOME/IP 이벤트까지 전달되고 서비스 발견·지연 기준 충족 — 발견 61.7ms (기준 5초), 지연 p99 13.7ms (기준 50ms)

---

## 1. 한 줄 요약

Gateway가 ECU의 CAN 메시지를 **검사하고**, 믿을 수 있는 **최신 값만 보관**해서, 중앙 컴퓨터(AP)에 **SOME/IP 서비스**로 20ms마다 전달하도록 구현함.

```
[ECU] ─CAN─→ [Gateway] ───────────────── SOME/IP ─────────────→ [AP]
              ① 입력 검증                ④ 서비스 알림 (SD)
              ② 최신 상태 보관            ⑤ AP가 발견·구독
              ③ validity·freshness 판단   ⑥ 20ms마다 event / 요청 시 GetSnapshot
```

AP에는 CAN 장치가 없으므로 이 서비스가 AP가 차량 상태를 아는 **유일한 경로**임.

---

## 2. 구현 내용

### 2.1 CAN 입력 검증 (`src/gateway/can_input_validator.cpp`)
- 검사 순서: **frame 형식 → ID → 길이 → CRC → 예약 비트 → counter → valid → 범위**
- Phase 2에서 시험을 마친 codec을 그대로 재사용
- 29-bit 확장 ID, RTR, error frame은 아래 11비트가 같아도 거부 (`ERR_FRAME_FORMAT`)
- **CRC를 counter보다 먼저** 검사 → 깨진 frame이 번호표 상태를 바꾸지 못함
- 메시지 종류(PDU)마다 counter 상태를 따로 관리

### 2.2 최신 상태 보관 (`src/gateway/vehicle_state_cache.cpp`)
- PDU마다 마지막 **정상** frame과 받은 시각(monotonic) 저장
- 거부된 frame은 마지막 정상값을 바꾸지 않음
- 통과·거부 개수, 연속 오류 수 기록

### 2.3 validity·freshness (§5.4)
| 구분 | 기준 |
| --- | --- |
| freshness | 첫 수신 전 `UNKNOWN` / 주기의 3배 이상 없으면 `DEGRADED` / 5배 이상 없으면 `STALE` |
| 연속 오류 | 3회 연속 거부 시 해당 PDU degraded, 정상 1회로 회복 |
| validity bit | 값을 받았고, 연속 오류 < 3, 마지막 frame의 fault = 0 |
| freshness bit | `FRESH`일 때만 1 |
| 오래된 값 | 마지막 정상값은 계속 보내되 freshness bit를 0으로 표시 |

**Gateway 상태**: 모든 PDU를 받기 전 `Startup` → 모두 정상이면 `Running` → 하나라도 오래됐거나 오류면 `Degraded` → 종료 시 `Shutdown`

**bitmap 배치**: bit0 RPM, bit1 속도, bit2 스로틀, bit3 냉각수, bit4 브레이크 압력, bit5 ABS

### 2.4 VehicleState 서비스 (§7.2)
| 항목 | 값 |
| --- | --- |
| Service / Instance | `0x1000` / `0x0001`, version 1.0 |
| Event `VehicleStateChanged` | `0x8001`, eventgroup `0x0001`, 20ms, UDP 30501 |
| Method `GetSnapshot` | `0x0001`, TCP 30502. 요청 payload가 비어 있지 않으면 `E_MALFORMED_MESSAGE`로 거부 |
| Payload | 28바이트, network byte order |
| SD | multicast `224.244.224.245:30490`, initial delay 10~100ms, 반복 3회·200ms, cyclic offer 1000ms, TTL 3초 |

- 서비스 정의는 `config/services/vehicle_state.yaml` **한 곳**에 두고, 생성기가 C++ 직렬화 코드와 **정답 바이트(golden fixture)** 를 자동 생성 (§7.4)
- AP consumer는 길이 ≠ 28, 예약 byte ≠ 0, 버전 불일치 payload를 **사용하지 않고** security event로 기록

### 2.5 실행 구성
| namespace | 프로그램 | 비고 |
| --- | --- | --- |
| `hj_veh` | `ecu_engine`, `ecu_brake`, `ecu_steering` | Phase 2 |
| `hj_gw` | `vehicle_gateway` | CAN + Ethernet 둘 다 접근하는 유일한 프로그램 |
| `hj_ap` | `vehicle_state_consumer` | CAN 장치 없음 |

- 모든 프로그램은 **root가 아닌 계정**으로 실행
- namespace는 `/tmp`를 공유하므로, vSomeIP 내부 소켓이 섞이지 않게 방마다 다른 `network` 이름 사용
- vSomeIP와 Boost는 서버 시스템이 아니라 **작업 폴더(`third_party/`) 안에만 설치** (서버 설정 변경 없음)

---

## 3. 이번에 정한 값 (project.md §7.5, 팀 확인 필요)

| 항목 | 결정 |
| --- | --- |
| vSomeIP 버전 | 3.7.6 (2026-09-17, 3.7 계열 최신 패치. 3.8.0은 공개 5일이라 제외) |
| Boost | Ubuntu 패키지 1.90.0-6ubuntu1을 받아 작업 폴더에 압축만 풂 (SHA-256 기록) |
| 범위 | VehicleState만. BodyControl은 LIN(Phase 6)과 함께 |
| bitmap·상태 전이 | 위 2.3 |
| 지연 측정 | Gateway가 CAN을 받은 시각(payload timestamp) → AP가 event를 받은 시각. 기준서의 "발행"보다 엄격 |
| SD initial delay | 10~100ms 범위만 설정. vSomeIP가 난수로 고르며 **seed 고정 불가** (§7.1 "결정적 seed"는 미충족) |

---

## 4. 시험 결과

### 4.1 단위 시험 (2026-11-08, 직접 실행)

| 시험 | 결과 | 내용 |
| --- | --- | --- |
| 빌드 (C11/C++20, 경고=에러) | ✅ | |
| `test_can_input_validator` | ✅ 95 checks | 정상, 잘못된 ID·길이, 중복 counter, CRC 오류 후 counter 유지, 누락·점프, 범위 초과, valid=0, PDU별 counter, 29-bit·RTR·error frame, 무작위 20만 frame |
| `test_vehicle_state_cache` | ✅ 41 checks | Startup→Running, 3배·5배 freshness 경계, 연속 3회 오류와 회복, 거부 frame이 값 유지, fault bit, 검증기 연동 |
| `test_vehicle_state_payload` | ✅ 40 checks | 생성기 golden 바이트 일치, 왕복, 길이 27·29·예약 byte≠0 거부, 서비스 상수 |
| Phase 2 시험 (CRC, codec, 시나리오, C/Python 교차) | ✅ | 회귀 없음 |
| Gateway·consumer 빌드 | ✅ | vSomeIP 3.7.6과 연결, 경고 0 |

```
100% tests passed, 0 tests failed out of 7
```

메모리 오류 검사기(ASan/UBSan)를 켠 빌드에서도 7개 모두 통과.

### 4.2 통합 시험 (2026-11-09 11:34, `docs/results/20261109-113440_someip/`)

**방법**
- `scripts/run/gateway-someip-test.sh`: ECU 3개(`hj_veh`) → Gateway(`hj_gw`) → AP consumer(`hj_ap`)를 30초 실행, 처음 3초 제외하고 자동 판정
- 모든 프로그램은 non-root 계정으로 실행. AP 방에는 CAN 장치 없음
- 순서: ECU 시작 → 1초 뒤 Gateway → 1초 뒤 consumer (consumer가 시작된 시점부터 발견 시간 측정)
- 환경: kernel 7.0.0-42-generic, 무부하 VM, vSomeIP 3.7.6

**결과: PASS (13개 기준 전부 통과)**

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 서비스 발견 5초 이내 (§15.2) | ✅ | **61.7 ms** |
| CAN 수신 → event 수신 p99 ≤ 50ms (§15.2) | ✅ | p50 12.758 / **p99 13.691** / max 14.62 ms |
| 측정 event 수 ≥ 예상의 90% | ✅ | 1353 / 예상 1350 |
| event 순번 누락 0 | ✅ | 0 |
| 잘못된 event 0 | ✅ | 0 |
| 워밍업 뒤 validity 전부 1 | ✅ | 예외 0건 |
| 워밍업 뒤 freshness 전부 1 | ✅ | 예외 0건 |
| 워밍업 뒤 Gateway 상태 Running | ✅ | 1353 / 1353 |
| CAN 값이 event에 반영 (rpm) | ✅ | rpm 800~2498 |
| CAN 값이 event에 반영 (속도) | ✅ | 최고 60.00 km/h |
| GetSnapshot 응답 정상 | ✅ | True |
| 비어 있지 않은 GetSnapshot 요청 거부 | ✅ | True (Gateway 로그에 security event 1건 기록) |
| Gateway가 거부한 정상 CAN frame 0 | ✅ | rejected 0, unattributed 0, lost 0 |

**Gateway CAN 입력**

| ID | 메시지 | 통과 | 거부 |
| --- | --- | --- | --- |
| 0x100 | PT_EngineStatus | 3300 | 0 |
| 0x110 | PT_VehicleDynamics | 1650 | 0 |
| 0x180 | CH_BrakeStatus | 3300 | 0 |
| 0x200 | CH_SteeringStatus | 3301 | 0 |

- Gateway가 보낸 event 1649개, GetSnapshot 요청 2개 (그중 잘못된 요청 1개 거부)
- ECU 송신: 10ms 메시지 각 3500개, 20ms 메시지 1750개, overruns 0, errors 0. Gateway보다 먼저 시작하고 늦게 끝나므로 Gateway 수신 수보다 많음

**쉽게 말하면:** 중앙 컴퓨터가 차량 상태 서비스를 0.06초 만에 찾았고, ECU가 보낸 값이 평균 약 13ms 만에 중앙 컴퓨터에 도착했다. 30초 동안 메시지 하나도 빠지거나 깨지지 않았다.

**측정 조건 주의**
- 발견 시간은 Gateway가 **이미 서비스를 알리고 있는 상태**에서 consumer가 시작했을 때의 값. 두 쪽이 동시에 켜지거나 Gateway가 나중에 켜지는 경우는 따로 측정하지 않음
- 지연은 Gateway가 CAN을 받은 시각부터 AP가 event를 받은 시각까지 (기준서의 "발행"까지보다 엄격). event 주기 20ms 안에서 기다리는 시간이 포함됨
- 30초 1회 실행 결과이며 표본은 1353개

---

## 5. 실행 방법

```bash
# 1) vSomeIP 빌드 (root 불필요, 작업 폴더 안에만 설치)
#    백그라운드 실행: 접속이 끊겨도 계속됨. 컴파일 1개씩·CPU 1개·컴파일러당 메모리 2GB·최저 우선순위,
#    VM 남은 메모리가 1.5GB 아래면 자동 일시정지 (2026-11-09 동시 4개 빌드로 VM이 멈춘 뒤 적용)
scripts/setup/build-vsomeip.sh
scripts/setup/build-vsomeip.sh --status   # "상태: done"이 될 때까지 가끔 확인

# 2) 프로젝트 다시 빌드 + 단위 시험
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

# 3) 통합 시험 (root, 약 36초 — 중간에 Ctrl+C 금지)
su -
/home/ghwls/work/practice/scripts/setup.sh
/home/ghwls/work/scripts/run/gateway-someip-test.sh
exit
```

---

## 6. 주장할 수 있는 것 / 없는 것

✅ 주장 가능
- Gateway는 형식·ID·길이·CRC·예약 비트·counter·valid·범위를 통과한 CAN 값만 보관한다 (단위 시험)
- 오래된 값과 오류가 난 값은 freshness·validity bit로 구분되어 AP에 전달된다 (단위 시험)
- 잘못된 payload와 요청은 양쪽에서 거부된다 (단위 시험 + 통합 시험의 GetSnapshot)
- 무부하 VM에서 CAN 입력이 SOME/IP event까지 전달되고, 서비스 발견 61.7ms, CAN 수신→event 수신 p99 13.7ms였다 (30초 1회 측정)

❌ 아직 주장 불가
- Gateway가 나중에 켜지거나 재시작되는 경우의 재발견 시간
- 부하 상태, 장시간(30분) 안정성 → §15.3
- BodyControl 서비스 → Phase 6
- 30분 장시간 안정성 → §15.3

⚠️ 한계
- **CRC와 counter를 맞춘 범위 안의 위조 frame은 통과함** (시험으로 확인). 진위성 검증(SecOC 수준)은 범위 밖 → IDS(Phase 7)에서 다룰 예정
- vSomeIP를 쓰지만 `ara::com`이나 AUTOSAR 적합 구현이 아님
- SD initial delay를 seed로 고정할 수 없음
- VM 위 Linux 프로세스 결과이며 실차 Ethernet·CAN 타이밍을 대변하지 않음
