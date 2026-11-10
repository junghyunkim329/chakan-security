# Phase 2. CAN PDU와 ECU (호진)

> 상태: 🟢 엔진·브레이크·조향 구현, 단위 시험·송신 주기 시험 통과 / 🟡 변속기는 R 인코딩 팀 확인 대기
> 날짜: 2026-11-02
> 환경: chakan VM (Ubuntu 26.04.1, kernel 7.0.0-38, gcc 15.2, CMake 4.2.3 + Ninja, Python 3.14, pytest 9.0.2)
> 관련: `project.md` §5, §5.5(이번 결정), §15.1
> ⚠️ 기준서에 없던 세부값은 **팀 확인 필요**

---

## ✅ 할 일 / 완료 기준

- [x] 공통 신호 정의 파일 — `config/signals/can.yaml`
- [x] PDU 인코딩·디코딩 — C(`ECU/src/common/pdu_codec.c`) + Python 기준 구현(`tools/sil_can/codec.py`)
- [x] CRC·counter 구현
- [ ] Engine·Brake·Steering·Transmission 독립 프로세스 — **3/4 완료** (Transmission은 기어 R 값 확인 후)

**완료 기준: 정상값·경계값·잘못된 데이터·송신 주기 시험 통과**
- [x] 정상값
- [x] 경계값
- [x] 잘못된 데이터
- [x] 송신 주기 — 엔진·브레이크·조향 4개 메시지 통과 (변속기 0x280은 구현 후 추가 시험)

---

## 1. 쉽게 이해하기

Phase 1이 **전화선**을 깐 단계라면, Phase 2는 그 선으로 **말하는 사람(ECU)** 과 **말하는 규칙**을 만드는 단계.

| 할 일 | 쉬운 뜻 |
| --- | --- |
| 신호 정의 파일 | **사전.** "0x100번 메시지의 0~1번째 칸은 엔진 회전수" 같은 약속. 보내는 쪽(C)과 받는 쪽·시험(Python)이 **같은 파일 하나**를 읽음 |
| 인코딩·디코딩 | 숫자(시속 60km)를 **8칸 상자(8바이트)** 에 넣고(인코딩) 꺼내는(디코딩) 것 |
| CRC | 상자 내용으로 계산한 **검사 숫자.** 1비트만 바뀌어도 값이 달라져서 손상을 알아챔 |
| counter | 0~15를 도는 **번호표.** 같은 번호가 또 오면 중복, 번호가 건너뛰면 누락 |
| 독립 프로세스 | 엔진·브레이크·조향이 **각자 따로 도는 프로그램.** 10ms·20ms마다 메시지를 보냄 |

**한계 (보안 관점에서 중요)**
CRC와 counter는 **실수(손상·누락·중복)** 를 잡는 장치지 **해커**를 막는 장치가 아님. 비밀키가 없어서 해커도 CRC를 다시 계산할 수 있음. UN R155 M10은 "수신 메시지의 **진위성과 무결성** 검증"을 요구하는데, 이 프로젝트는 무결성(우발적 오류)만 다루고 진위성(누가 보냈나)은 다루지 않음 → IDS(Phase 7)에서 기록할 한계.

---

## 2. 메시지 상자 구조 (8바이트)

```
byte:  0      1      2      3      4      5      6               7
      [  신호 데이터 (메시지마다 다름)         ] [counter|상태비트] [CRC]
                                                 상위4bit: 번호표   CRC-8
                                                 bit3 valid
                                                 bit2 degraded
                                                 bit1 fault
```

| ID | 이름 | 보내는 ECU | 주기 | 내용 |
| --- | --- | --- | --- | --- |
| 0x100 | PT_EngineStatus | Engine | 10ms | 회전수, 스로틀, 냉각수 온도, 시동/회전 |
| 0x110 | PT_VehicleDynamics | **Brake** | 20ms | 속도, 가속도, 진행 방향 |
| 0x180 | CH_BrakeStatus | Brake | 10ms | 브레이크 압력, 페달, 스위치, ABS |
| 0x200 | CH_SteeringStatus | Steering | 10ms | 조향각, 조향 속도, 보조 토크 |
| 0x280 | PT_TransmissionStatus | Transmission | 20ms | ⏸ 보류 |

---

## 3. 이번에 정한 값 (project.md §5.5, 팀 확인 필요)

| 항목 | 결정 | 이유 |
| --- | --- | --- |
| 0x110 송신 | Brake(ABS/ESC) | 실차에서 속도는 ABS/ESC가 바퀴 속도로 계산 |
| 상태 비트 | bit0~1 기능별, bit2 degraded, bit3 fault | 모든 메시지 같은 패턴 |
| 범위 없던 신호 | 가속도 ±15 m/s², 조향 속도 ±2000 °/s, 보조 토크 ±12 Nm | 일반 승용차·센서 수준 |
| CRC | SAE J1850 CRC-8 (반전 없음) | 기준서 값과 일치하는 표준 |
| 수신 검사 순서 | ID → 길이 → CRC → 예약 비트 → counter → valid → 범위 | 깨진 메시지가 번호표 상태를 바꾸지 않게 CRC 먼저 |
| counter | 차이 1 정상 / 2~3 수락+누락 기록 / 0 중복 거부 / 4 이상 거부 | IDS "비정상 점프" 기준 |
| 주기 합격 | 평균 ±1%, 99%가 ±2ms 이내, 2배 초과 0건 | VM 기준 |

**보류:** 0x280 실제 기어에서 기준서가 R=1, 1단=1로 겹침 → 팀 확인 필요 (제안: R=-1)

---

## 4. 시험 결과 (2026-11-02, 직접 실행)

| 시험 | 결과 | 내용 |
| --- | --- | --- |
| 빌드 (경고=에러) | ✅ | C11, `-Wall -Wextra -Wpedantic -Werror` 등 |
| `test_crc8` | ✅ 71 checks | 표준 검사값 `0x4B`, ID·데이터 1비트 변경 전부 탐지 |
| `test_codec` | ✅ 3093 checks | 모든 메시지 최소·최대·중간값 왕복, 범위 ±1 거부, 빅엔디언·음수, 잘못된 ID·길이·CRC·예약 비트·valid=0·범위 초과, counter 규칙 |
| `test_scenario` | ✅ | 2시간 분량(1ms 단위) 시나리오 값이 전부 범위 안 |
| `python_codec` | ✅ 6 tests | **C와 Python이 같은 바이트를 만드는지 272개 벡터로 교차 확인** |
| cppcheck / shellcheck | ✅ | 정적 분석 경고 0 |
| 송신 주기 | ✅ passed (22.15s) | 아래 표 |

```
100% tests passed, 0 tests failed out of 4
```

### 송신 주기 시험 (2026-11-02 20:41, `docs/results/20261102-204133_ecu-period/report.json`)

**방법**
- ECU 3개를 차량 방(`hj_veh`)에서 **일반 사용자(nobody)** 로 실행, `vxcan_veh`로 송신
- Gateway 방(`hj_gw`)의 `vxcan_gw`에서 수신, **커널 수신 시각**으로 간격 측정
- 시작 직후 5개는 제외하고 메시지별 **연속 1000개 간격** 분석
- 받은 모든 메시지를 Python 기준 codec으로 다시 검사 (ID·길이·CRC·예약 비트·valid·범위·counter)
- 환경: kernel 7.0.0-38-generic, 무부하 VM

**결과**

| ID | 메시지 | 보낸 ECU | 목표 | 평균 | 평균 오차 | 99% 편차 | 최소 / 최대 | ±2ms 이내 | 검사 오류 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0x100 | PT_EngineStatus | engine | 10ms | 10.0000ms | 0.000% | 0.935ms | 9.002 / 11.071ms | 100% | 0 |
| 0x110 | PT_VehicleDynamics | brake | 20ms | 19.9995ms | −0.002% | 0.847ms | 18.960 / 20.969ms | 100% | 0 |
| 0x180 | CH_BrakeStatus | brake | 10ms | 10.0000ms | 0.000% | 0.812ms | 8.923 / 11.000ms | 100% | 0 |
| 0x200 | CH_SteeringStatus | steering | 10ms | 10.0000ms | 0.000% | 0.761ms | 9.011 / 11.019ms | 100% | 0 |

**합격 기준 대비**

| 기준 | 결과 |
| --- | --- |
| 평균 주기 ±1% 이내 | ✅ 최대 0.002% |
| 99%가 ±2ms 이내 | ✅ 100%가 ±2ms 이내 (99% 편차 최대 0.935ms) |
| 목표의 2배 초과 간격 0건 | ✅ 최대 간격 20.969ms (20ms 메시지), 11.071ms (10ms 메시지) |
| 모든 메시지 검사 통과 | ✅ 오류 0, 예상 밖 CAN ID 0 |
| ECU 송신 상태 | ✅ 3개 ECU 모두 overruns=0, errors=0 (10ms 메시지 2110개, 20ms 메시지 1055개 송신) |

**쉽게 말하면:** 10ms마다 보내라고 했더니 평균은 정확히 10ms였고, 가장 늦어도 약 1ms 차이였다. 메시지 하나도 빠지거나 깨지지 않았다.

---

## 5. 실행 방법

```bash
# 빌드와 단위 시험 (root 불필요)
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure

# 송신 주기 시험 (root, Phase 1 연습 구성 hj_ 사용, 약 25초 — 중간에 Ctrl+C 금지)
su -
/home/ghwls/work/practice/scripts/setup.sh
/home/ghwls/work/scripts/run/ecu-period-test.sh
exit
# 결과: docs/results/<시각>_ecu-period/report.json
```

---

## 6. 주장할 수 있는 것 / 없는 것

✅ 주장 가능
- 신호 정의는 YAML 한 곳에 있고, C와 Python이 같은 결과를 만든다 (교차 확인)
- 정의된 범위·형식·CRC·counter를 벗어난 데이터는 수신 측 codec이 거부한다
- 무부하 VM에서 엔진·브레이크·조향 ECU는 10ms/20ms 주기를 평균 오차 0.002% 이내, 간격 편차 약 1ms 이내로 송신했다 (1000 간격 1회 측정)

❌ 아직 주장 불가
- 부하가 걸린 상태나 장시간(30분) 주기 안정성 → 장시간 시험(§15.3)
- 변속기 ECU
- Gateway가 실제로 이 검사를 하는 것 → Phase 3
- 고장 주입(fault injection) 시나리오 → 이후 추가

⚠️ 실차와 다른 점
- 일반 리눅스 프로세스라 실제 MCU/RTOS의 실시간성 없음
- vxcan은 실제 CAN 속도·충돌 처리 없음 → 주기는 "프로세스 스케줄링" 정확도만 의미
- 시나리오는 정해진 패턴이지 실제 차량 물리 모델이 아님
- CRC/counter는 인증이 아님 (위조 방지 X)
