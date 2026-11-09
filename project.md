# Adaptive Vehicle Cybersecurity SIL

문서 기준일: 2026-10-13

설계 기준: AUTOSAR Adaptive Platform R25-11 참고

실행 환경: Ubuntu 26.04.1 LTS 가상 머신

문서 상태: 구현 전 확정 기준서

## 1. 프로젝트 정의

이 프로젝트는 무료 소프트웨어와 한 대의 Ubuntu 가상 머신으로 차량의 도메인 ECU, 중앙 Gateway, Adaptive 애플리케이션, CAN·LIN·SOME/IP 통신, UDS·DoIP 진단, 침입 탐지 및 로그 분석을 재현하는 격리형 Software-in-the-Loop(SIL)이다.

목표는 실차의 전기·기계적 동작을 복제하는 것이 아니라 다음 소프트웨어 구조와 업무 흐름을 가능한 한 유사하게 경험하는 것이다.

- 여러 ECU가 독립 프로세스로 동작하는 분산 구조
- CAN과 LIN으로 구성된 하위 차량 네트워크
- Ethernet/SOME-IP 기반 중앙 컴퓨팅 노드
- CAN·LIN과 Ethernet 사이의 차량 Gateway
- 서비스 탐색, 상태 관리, 진단, 로깅 및 장애 복구
- 정상 주행 기준선과 제한된 공격 실험을 이용한 IDS 검증
- 요구사항, 구현, 시험 결과와 주장 범위의 추적

이 프로젝트의 정확한 표현은 **“AUTOSAR Adaptive 구조를 참고한 무료 Linux 격리형 차량 사이버보안 SIL”**이다.

다음 표현은 사용하지 않는다.

- AUTOSAR Adaptive 완전 준수
- AUTOSAR 공식 인증
- 양산 차량 적용 가능
- 실제 차량 제어 검증 완료
- 기능안전 또는 실시간성 검증 완료
- vcan 결과로 실제 CAN 버스 성능 입증

## 2. 설계 근거와 값의 분류

모든 요구사항과 수치는 다음 다섯 종류 중 하나로 표시한다.

| 분류 | 의미 | 문서에서의 처리 |
| --- | --- | --- |
| 표준 근거 | AUTOSAR 또는 Linux 공식 문서에서 확인한 구조·동작 | 출처 링크와 적용 범위를 기록한다. |
| 공개 구현 근거 | vSomeIP, DLT 등 공개 구현이 실제 제공하는 기능 | 사용 버전과 검증 결과를 기록한다. |
| 프로젝트 선택 | 무료 SIL을 구성하기 위해 이 프로젝트가 정한 ID·주기·임계값 | 제조사 요구사항이 아님을 명시한다. |
| 측정 결과 | 동일 VM과 동일 설정에서 시험으로 얻은 값 | 날짜, 버전, 부하, 표본과 명령을 함께 기록한다. |
| 확인 불가 | 제조사 DBC, ECU 사양, 안전 목표처럼 제공되지 않은 정보 | 추정하지 않고 확인 불가로 남긴다. |

특정 제조사나 차종의 DBC, 타이밍 요구사항, 보안키, 진단 데이터, 기능안전 분석은 제공되지 않았다. 따라서 아래 CAN ID, 신호 배치, 주기, 진단 주소, IDS 임계값과 합격 시간은 모두 **프로젝트 선택값**이며 실차 제조사 요구사항으로 표현하지 않는다.

이 문서에서 “무료”는 기존 PC와 가상화 환경을 보유했다는 전제에서 신규 소프트웨어 라이선스와 차량 통신 하드웨어 구매 비용이 0원이라는 뜻이다. 기존 PC, 전력, 인터넷과 저장장치 비용까지 0원이라는 뜻은 아니다.

아키텍처, 프로토콜 또는 합격 기준을 변경하려면 먼저 이 문서의 기준일과 해당 절을 수정하고 변경 이유, 근거 출처, 영향받는 configuration과 재실행할 시험을 기록한다. 구현 코드만 먼저 바꾸거나 `tmp/legacy/` 자료를 근거 없이 복원하지 않는다. 물리 CAN/LIN 또는 HIL을 추가하는 요청은 이 무료 SIL과 별도의 확장 단계로 다룬다.

## 3. 실행 환경

### 3.1 가상 머신

- Ubuntu 26.04.1 LTS 64-bit
- 4 vCPU
- RAM 8GB
- 가상 디스크 60GB
- 외부 네트워크는 패키지 설치와 업데이트에만 사용
- 차량 네트워크는 Linux network namespace 안에 격리
- 호스트와 VM 사이의 공유 폴더는 소스 전달에만 사용하고 차량 통신 경로로 사용하지 않음

Ubuntu 공식 최소 사양보다 여유 있게 잡은 이유는 vSomeIP 빌드, 다중 ECU 프로세스, 패킷 캡처, DLT, SQLite와 대시보드를 동시에 실행하기 위해서다. 이는 성능 보장이 아니라 프로젝트 운영 기준이다.

### 3.2 언어와 빌드

- Adaptive 애플리케이션과 Gateway: C++20
- CAN ECU 제어 코어와 PDU codec: C11
- 시험 시나리오, 공격 도구, 결과 분석과 대시보드 API: Python 3
- 빌드: CMake
- C/C++ 단위 시험: CTest
- Python 시험: pytest
- 정적 분석: compiler warning, clang-tidy, cppcheck 중 무료로 실행 가능한 항목

C++20은 현재 vSomeIP 공개 저장소가 요구하는 컴파일러 기준에 맞춘다. 하위 ECU는 작은 상태기계와 codec을 분리해 MCU형 코드 구조를 연습하기 위해 C11로 작성한다. Linux 프로세스와 일반 PC 컴파일러를 사용하므로 실제 MCU 메모리, 인터럽트, RTOS 태스크와 동일하지 않다.

## 4. E/E 아키텍처

### 4.1 논리 노드

`Adaptive Central Node`

- Execution Manager
- State Manager
- Vehicle State Consumer
- Diagnostic Client
- Security Event Manager
- DLT Client
- Dashboard API

`Vehicle Gateway`

- SOME/IP Service Provider
- CAN Signal Gateway
- LIN Master 및 LIN Gateway
- DoIP Entity
- UDS Router
- PDU 유효성 검사
- 네트워크 경계 정책

`CAN Domain`

- Engine ECU
- Brake ECU
- Steering ECU
- Transmission ECU

`LIN Body Domain`

- Door ECU
- Window ECU
- Interior Light ECU

### 4.2 Linux 격리

- `ns_ap`: Adaptive Central Node
- `ns_gateway`: Vehicle Gateway
- `ns_vehicle`: CAN ECU 프로세스와 차량측 CAN 인터페이스

`ns_ap`와 `ns_gateway`는 `veth` 가상 Ethernet으로 연결한다. `ns_gateway`와 `ns_vehicle`은 `vxcan` 쌍으로 연결한다. AP 애플리케이션은 차량 CAN 인터페이스를 직접 열 수 없어야 하며 반드시 Gateway를 거친다.

패킷 캡처는 별도 observer가 모든 namespace를 볼 수 있다고 가정하지 않는다. 시험 실행기가 `ip netns exec`로 각 대상 namespace 안에서 제한된 capture 프로세스를 시작하고 공용 결과 디렉터리에 파일을 저장한다. CAN capture는 `ns_vehicle`, SOME/IP·DoIP capture는 `ns_gateway`와 `ns_ap`에서 각각 수행한다.

이 구성은 실차의 물리 ECU 분리를 그대로 재현하지 않는다. 대신 네트워크 장치, IP 주소, 라우팅, 포트와 방화벽을 namespace별로 격리하여 한 VM 안에서도 신뢰 경계를 시험할 수 있게 한다. Linux 공식 문서상 network namespace는 네트워크 장치와 프로토콜 스택 등을 분리하며, `veth`와 `vxcan`은 namespace 간 연결에 사용할 수 있다.

## 5. CAN 네트워크

### 5.1 설계 원칙

- Classical CAN, 11-bit identifier, 최대 8-byte payload
- 논리 bitrate: 500kbit/s
- 실제 통신 API: Linux SocketCAN
- 실제 인터페이스: `vcan`과 `vxcan`
- 한 신호마다 CAN ID를 배정하지 않고 관련 신호를 하나의 8-byte PDU에 묶음
- 모든 주기 PDU에 alive counter, validity와 CRC를 포함
- 데이터 해석 전 CAN ID, DLC, counter, CRC, validity와 물리 범위를 검사
- 주기 측정에는 monotonic clock, 사용자 로그에는 timezone 포함 시각 사용

500kbit/s는 계산과 설정 문서에 사용하는 프로젝트 값이다. `vcan`은 물리 bitrate와 arbitration을 구현하지 않으므로 실제 버스 점유율을 측정했다고 주장하지 않는다.

### 5.2 공통 보호 형식

각 PDU의 byte 6과 byte 7은 다음처럼 사용한다.

- Byte 6 상위 4-bit: alive counter, `0~15` 순환
- Byte 6 bit 3: data valid
- Byte 6 bit 2: degraded
- Byte 6 bit 1: fault present
- Byte 6 bit 0: reserved, 송신 시 `0`
- Byte 7: CRC-8

CRC-8 프로젝트 프로파일:

- polynomial: `0x1D`
- initial value: `0xFF`
- final XOR: `0xFF`
- 입력: CAN ID의 2-byte big-endian 표현과 payload byte 0~6

이는 누락, 중복, 순서 변경과 단순 손상을 실습하기 위한 **AUTOSAR E2E 개념 참고 프로파일**이다. AUTOSAR E2E Profile 적합성을 주장하지 않는다. 정식 적합성을 주장하려면 적용 Profile의 전체 data ID, counter, CRC, 상태기계와 시험 요구사항을 그대로 구현하고 추적해야 한다.

### 5.3 PDU 명세

| CAN ID | PDU | 주기 | 주요 데이터 | DLC |
| --- | --- | --- | --- | --- |
| `0x100` | `PT_EngineStatus` | 10ms | RPM, throttle, coolant, engine state | 8 |
| `0x110` | `PT_VehicleDynamics` | 20ms | speed, longitudinal acceleration, direction | 8 |
| `0x180` | `CH_BrakeStatus` | 10ms | pressure, pedal, brake switch, ABS state | 8 |
| `0x200` | `CH_SteeringStatus` | 10ms | angle, angle rate, assist torque, EPS state | 8 |
| `0x280` | `PT_TransmissionStatus` | 20ms | actual gear, selected range, shaft speed, state | 8 |
| `0x600` | `GW_NetworkHealth` | 100ms | domain validity, error counters, gateway state | 8 |

#### `0x100 PT_EngineStatus`

- Byte 0~1: RPM, uint16 big-endian, 1rpm/bit, 0~8000rpm
- Byte 2: throttle, uint8, 0.5%/bit, 0~100%
- Byte 3~4: coolant, int16 big-endian, 0.1°C/bit, -40.0~150.0°C
- Byte 5: ignition, running, degraded, fault bitfield
- Byte 6: alive counter와 validity
- Byte 7: CRC-8

#### `0x110 PT_VehicleDynamics`

- Byte 0~1: speed, uint16 big-endian, 0.01km/h/bit, 0~250.00km/h
- Byte 2~3: longitudinal acceleration, int16 big-endian, 0.01m/s²/bit
- Byte 4: direction, `0=standstill`, `1=forward`, `2=reverse`
- Byte 5: signal state와 reserved bits
- Byte 6: alive counter와 validity
- Byte 7: CRC-8

#### `0x180 CH_BrakeStatus`

- Byte 0~1: requested pressure, uint16 big-endian, 0.1bar/bit, 0~200.0bar
- Byte 2: pedal position, uint8, 0.5%/bit, 0~100%
- Byte 3: brake switch, ABS active, degraded와 fault bitfield
- Byte 4~5: reserved, 송신 시 `0x00`
- Byte 6: alive counter와 validity
- Byte 7: CRC-8

#### `0x200 CH_SteeringStatus`

- Byte 0~1: steering angle, int16 big-endian, 0.1°/bit, -780.0~780.0°
- Byte 2~3: steering angle rate, int16 big-endian, 0.1°/s/bit
- Byte 4: assist torque, int8, 0.1Nm/bit
- Byte 5: EPS active, degraded와 fault bitfield
- Byte 6: alive counter와 validity
- Byte 7: CRC-8

#### `0x280 PT_TransmissionStatus`

- Byte 0: actual gear, int8, `1=R`, `0=N`, `1~8=forward gear`
- Byte 1: selected range, `0=P`, `1=R`, `2=N`, `3=D`
- Byte 2~3: input shaft speed, uint16 big-endian, 1rpm/bit
- Byte 4~5: transmission state와 reserved
- Byte 6: alive counter와 validity
- Byte 7: CRC-8

각 ID, 신호 범위와 주기는 공개 제조사 DBC에서 가져온 값이 아니다. 서로 다른 제어 주기, packed PDU, alive counter와 CRC를 다루기 위한 프로젝트 선택이다. 구현 단계에서 이 표는 하나의 machine-readable 신호 정의 파일로 옮기고 송신자, Gateway, IDS와 시험이 같은 정의를 사용해야 한다.

### 5.4 Freshness 기준

- 첫 메시지를 받기 전: `UNKNOWN`
- 정상 주기의 3배 동안 새 메시지 없음: `DEGRADED`
- 정상 주기의 5배 동안 새 메시지 없음: `STALE`
- CRC 오류 또는 counter 역행: 현재 PDU 폐기
- 연속 3회 오류: 해당 ECU 상태를 `DEGRADED`로 전환
- 마지막 정상값은 화면에 표시할 수 있지만 `stale=true`를 반드시 함께 표시

이 배수와 연속 횟수는 SIL 시험을 위한 프로젝트 정책이며 제조사 고장 허용 시간 요구사항이 아니다.

### 5.5 Phase 2 세부 결정 (2026-11-02 제안, 팀 확인 필요)

§5.2~5.4가 정하지 않은 값을 다음처럼 정했다. 모두 프로젝트 선택값이며 단일 원본은 `config/signals/can.yaml`이다.

| 항목 | 결정 | 근거 |
| --- | --- | --- |
| `0x110` 송신 ECU | Brake (ABS/ESC). 이름 `PT_VehicleDynamics`는 유지 | 실차에서 차량 속도는 ABS/ESC가 바퀴 속도로, 종방향 가속도는 ESC 센서로 계산 |
| 상태 bitfield 위치 | bit0~1 기능별, bit2 degraded, bit3 fault, bit4~7 reserved. `0x100`·`0x110`·`0x200` byte 5, `0x180` byte 3 | 기준서가 이름만 정함. 전 PDU에 같은 패턴 |
| byte 6 degraded/fault | 기능 bit의 요약(OR) | §5.2 공통 형식과 기능 bit의 관계 정의 |
| 범위 미정 신호 | 종방향 가속도 ±15.00m/s², 조향각 속도 ±2000.0°/s, 보조 토크 ±12.0Nm, direction 0~2 | 일반 승용차·조향각 센서 수준, int 표현 범위 안 |
| reserved | 어떤 signal에도 쓰이지 않는 byte 0~5의 bit와 byte 6 bit 0. 송신 0, 수신 시 1이면 거부 | §5.2, §11.2 reserved 불일치 |
| CRC 반전 | 반전 없음, CRC-8/SAE-J1850 check `0x4B` | §5.2 다항식·초기값·final XOR이 SAE J1850 프로파일과 일치 |
| 수신 검사 순서 | ID → DLC → CRC → reserved → counter → valid → 범위 | 손상 frame이 counter 상태를 바꾸지 않도록 CRC를 counter보다 먼저 |
| counter 판정 | delta 1 정상, 2~3 수락+손실 기록, 0 중복 거부, 4~15 점프·역행 거부 후 재동기화 | §11.2 "비정상 점프"의 기준값 |
| 송신 주기 합격 | PDU별 연속 1000 간격: 평균 ±1%, 99%가 ±2ms 이내, 2배 초과 0건, 모든 frame 검사 통과 | 무부하 VM 기준 프로젝트 합격 기준 |
| 정상 시나리오 | 30초 결정적 반복: 정지 → 0~60km/h 가속 → 정속 → 제동 → 정지 | 재현 가능한 기준선. 차량 동역학 모델 아님 |

보류: `0x280` actual gear의 R 인코딩이 §5.3에서 1단과 같은 `1`로 적혀 있어 Transmission ECU와 `0x280` 정의는 팀 확인 후 추가한다(제안: `-1=R`). `0x600`은 Gateway 송신이며 Phase 3에서 정의한다.

구현: `tools/sil_can/`(YAML 해석과 Python 기준 codec), `tools/codegen/gen_can_signals.py`(C table 생성), `src/common/`(CRC·codec·시나리오·주기 송신), `src/ecu/{engine,brake,steering}/`, 시험 `tests/unit/`(CTest·pytest), `tests/integration/test_ecu_period.py`(`scripts/run/ecu-period-test.sh`, root 필요).

## 6. LIN Body Domain

### 6.1 구조

Gateway의 LIN Master 프로세스가 schedule table을 소유한다. Door, Window와 Interior Light는 독립 Slave 프로세스다. Slave가 임의로 버스를 점유하지 않고 Master가 요청한 슬롯에서만 응답한다.

### 6.2 프레임과 스케줄

| LIN ID | Frame | Publisher | 주기 |
| --- | --- | --- | --- |
| `0x10` | `Body_DoorStatus` | Door ECU | 100ms |
| `0x11` | `Body_WindowCommand` | Gateway Master | 50ms |
| `0x12` | `Body_WindowStatus` | Window ECU | 50ms |
| `0x13` | `Body_LightCommand` | Gateway Master | 100ms |
| `0x14` | `Body_LightStatus` | Interior Light ECU | 100ms |
- 논리 bitrate: 19.2kbit/s
- Master schedule tick: 10ms
- protected identifier parity 검사
- unconditional frame에 enhanced checksum 적용
- 연속 3회 응답 누락 시 통신 오류
- 5초간 명령과 상태 변화가 없으면 sleep
- Master wakeup event로 schedule 복귀

모든 payload 외부의 LIN protected identifier와 checksum은 LIN broker가 처리한다. frame별 payload는 다음처럼 고정한다.

6-bit frame ID의 bit를 `ID0~ID5`라 할 때 protected identifier parity는 `P0 = ID0 XOR ID1 XOR ID2 XOR ID4`, `P1 = NOT(ID1 XOR ID3 XOR ID4 XOR ID5)`로 계산한다. enhanced checksum은 protected identifier와 data byte를 carry-around 8-bit 합산한 뒤 반전한다. 현재 사용하지 않는 진단 frame `0x3C/0x3D`의 classic checksum은 향후 UDS-on-LIN 범위를 별도 승인할 때만 추가한다.

- `0x10 Body_DoorStatus`, DLC 2: byte 0 bit 0~3은 FL/FR/RL/RR door open, byte 1 bit 0은 central lock, bit 1은 child lock, bit 7은 valid, 나머지는 0
- `0x11 Body_WindowCommand`, DLC 2: byte 0은 목표 위치 `0~100`이며 `0=완전 닫힘`, `100=완전 열림`, byte 1 bit 0은 execute, bit 1은 cancel, 나머지는 0
- `0x12 Body_WindowStatus`, DLC 3: byte 0은 실제 위치 `0~100`, byte 1은 `0=stopped`, `1=opening`, `2=closing`, byte 2 bit 0은 obstruction injection, bit 1은 fault, bit 7은 valid
- `0x13 Body_LightCommand`, DLC 2: byte 0은 목표 밝기 `0~100`, byte 1 bit 0은 fade enable, 나머지는 0
- `0x14 Body_LightStatus`, DLC 2: byte 0은 실제 밝기 `0~100`, byte 1 bit 0은 active, bit 1은 fault, bit 7은 valid

100ms major schedule은 다음 10ms 슬롯 순서를 반복한다.

| Offset | Frame |
| --- | --- |
| 0ms | `0x11 Body_WindowCommand` |
| 10ms | `0x12 Body_WindowStatus` |
| 20ms | `0x13 Body_LightCommand` |
| 30ms | `0x14 Body_LightStatus` |
| 40ms | `0x10 Body_DoorStatus` |
| 50ms | `0x11 Body_WindowCommand` |
| 60ms | `0x12 Body_WindowStatus` |
| 70ms | idle/reserved |
| 80ms | idle/reserved |
| 90ms | idle/network management |

LIN 정의의 단일 원본은 `config/lin/network.yaml`이다. ID, publisher, subscriber, DLC, signal bit 위치, checksum mode, schedule과 timeout을 담고 LIN Master, Slave, Gateway와 시험 fixture가 같은 파일을 사용한다.

### 6.3 Slave 상태와 복구

- 초기 상태: 모든 door closed, central lock on, window 0, interior light 0, 모든 신호 valid
- Window ECU: valid execute command마다 목표를 갱신하고 50ms 응답 주기마다 최대 5% 이동
- cancel 또는 obstruction injection: 즉시 정지하고 현재 위치 유지; obstruction은 fault bit 설정
- Interior Light ECU: fade off면 즉시 목표 적용, fade on이면 100ms마다 최대 10% 이동
- Door ECU: 시험 시나리오가 door/lock 입력을 변경하고 Slave는 다음 자신의 슬롯에 상태 응답
- 연속 3회 응답 누락: Gateway가 해당 Slave를 degraded 처리
- 장애 제거 뒤 연속 3회의 checksum-valid 응답: normal로 복구
- sleep 중에는 주기 schedule 중지; Master wakeup 뒤 다음 major cycle의 0ms부터 재시작

이 이동률, 초기값과 복구 횟수는 UI·상태 전이를 반복 시험하기 위한 프로젝트 값이며 실제 window motor, anti-pinch 또는 body controller calibration이 아니다.

AUTOSAR의 LIN Driver와 LIN Interface는 Classic Platform 영역에 속한다. 따라서 LIN Slave를 Adaptive 애플리케이션에 직접 연결했다고 표현하지 않는다. 이 프로젝트에서는 Gateway가 Classic형 CAN/LIN 도메인과 Adaptive Ethernet 도메인을 연결한다.

Linux 프로세스 기반 LIN broker는 header, protected ID, publisher, response, checksum, schedule, sleep/wakeup와 timeout을 논리적으로 재현한다. UART break, sync field 파형, dominant/recessive 전압, oscillator 오차, 실제 baud-rate와 EMC는 재현하지 못한다.

## 7. Ethernet과 SOME/IP

### 7.1 네트워크

- `ns_gateway`: `10.10.0.1/24`
- `ns_ap`: `10.10.0.10/24`
- SOME/IP Service Discovery: UDP `30490`
- SOME/IP-SD multicast: `224.244.224.245`
- cyclic offer delay: 1000ms
- service TTL: 3초
- initial delay: 10~100ms 사이의 결정적 seed 기반 값
- repetition: 3회, base delay 200ms
- 방화벽 기본 정책: deny
- 허용 트래픽: 명시한 SOME/IP, DoIP, DLT 및 관리용 loopback만

물리 Ethernet 장비는 사용하지 않지만 `veth`를 사용해 IP 주소, UDP/TCP, multicast, 서비스 탐색, 포트 필터링과 장애를 실습한다. Adaptive 구조에서 Ethernet을 완전히 제거하면 SOME/IP와 DoIP를 통한 중앙 노드 경험이 사라지므로 가상 Ethernet은 필수다.

### 7.2 VehicleState 서비스

- Service ID: `0x1000`
- Instance ID: `0x0001`
- Major version: `1`
- Minor version: `0`
- Method `0x0001`: `GetSnapshot`
- Event `0x8001`: `VehicleStateChanged`
- Event Group: `0x0001`
- Event 주기: 20ms
- UDP event port: `30501`
- TCP method port: `30502`

Event에는 Gateway가 검증한 최신 차량 상태, 각 신호의 validity/freshness, source timestamp와 event sequence를 포함한다. 소비자는 invalid 또는 stale 값을 정상 제어값처럼 사용하지 않는다.

`VehicleStateChanged`와 `GetSnapshot` response는 동일한 28-byte network byte order payload를 사용한다.

| Offset | Type | Field | Scale |
| --- | --- | --- | --- |
| 0 | uint32 | event sequence | 1 |
| 4 | uint64 | Gateway monotonic timestamp | microsecond |
| 12 | uint16 | RPM | 1rpm/bit |
| 14 | uint16 | vehicle speed | 0.01km/h/bit |
| 16 | uint8 | throttle | 0.5%/bit |
| 17 | int16 | coolant | 0.1°C/bit |
| 19 | uint16 | brake pressure | 0.1bar/bit |
| 21 | uint8 | ABS state | 0/1 |
| 22 | uint16 | validity bitmap | signal별 bit |
| 24 | uint16 | freshness bitmap | signal별 bit |
| 26 | uint8 | Gateway state | 0=Startup, 1=Running, 2=Degraded, 3=Shutdown |
| 27 | uint8 | reserved | 송신 시 0 |

`GetSnapshot` request payload는 비어 있어야 하며 non-empty request는 거부한다. Consumer는 response 길이가 28이 아니거나 reserved byte가 0이 아니면 상태를 갱신하지 않고 보안 이벤트를 남긴다.

### 7.3 BodyControl 서비스

- Service ID: `0x1100`
- Instance ID: `0x0001`
- Method `0x0001`: `SetWindowPosition`
- Method `0x0002`: `SetInteriorLight`
- Event `0x8001`: `BodyStatusChanged`
- Event Group: `0x0001`
- UDP event port: `30511`
- TCP method port: `30512`

Method 요청은 allowlist, 범위, 차량 상태와 rate limit을 통과한 뒤에만 LIN command로 변환한다. 서비스 ID, method ID와 포트는 프로젝트 선택값이다.

`BodyStatusChanged` payload는 network byte order로 `sequence(uint32)`, `timestamp_us(uint64)`, `door_bits(uint8)`, `window_position(uint8, 0~100)`, `light_state(uint8, 0/1)`, `status_flags(uint8)`, `freshness(uint8)`, `reserved(3-byte, 0)` 순서의 20-byte다.

`SetWindowPosition` request는 1-byte 목표 위치 `0~100`, `SetInteriorLight` request는 1-byte `0/1`이다. response는 1-byte 결과로 `0=accepted`, `1=invalid argument`, `2=state rejected`, `3=LIN timeout`을 사용한다. 범위를 벗어나거나 payload 길이가 다르면 LIN 명령을 만들지 않는다.

### 7.4 구현 범위

vSomeIP을 사용해 SOME/IP message, Service Discovery, request/response와 event를 구현한다. 프로젝트 인터페이스 정의에서 C++ 타입과 provider/consumer wrapper를 생성하거나 빌드 시 검증한다.

서비스 정의의 단일 원본은 `config/services/*.yaml`로 정하고, build 도구가 C++20 serializer/deserializer와 golden-byte 시험 fixture를 생성한다. 이 YAML과 생성기는 프로젝트 전용이며 ARXML 또는 AUTOSAR code generator로 표현하지 않는다.

이를 `ara::com` 구현이라고 부르지 않는다. AUTOSAR ARXML 전체 모델, 공식 generator, Manifest schema와 상용 Adaptive Runtime을 사용하지 않기 때문이다. 공개 구성요소만으로 확인한 기능 범위를 넘어서 AUTOSAR 적합성을 주장하지 않는다.

### 7.5 Phase 3 세부 결정 (2026-11-08 제안, 팀 확인 필요)

| 항목 | 결정 | 근거 |
| --- | --- | --- |
| vSomeIP | 3.7.6 (commit `4187431d`, 2026-09-17). 3.8.0은 공개 5일이라 제외 | §11.1 버전 고정. 3.7 계열 최신 패치 |
| Boost | Ubuntu 패키지 1.90.0-6ubuntu1을 `apt-get download`로 받아 `third_party/boost-root`에 압축만 풂. SHA-256은 `third_party/debs/SHA256SUMS` | 서버 시스템 패키지를 바꾸지 않기 위함 |
| 설치 위치 | `third_party/vsomeip` (작업 폴더 안, root 불필요). `scripts/setup/build-vsomeip.sh` | 같은 이유 |
| 이번 범위 | VehicleState(0x1000)만. BodyControl(0x1100)은 LIN(Phase 6)과 함께 | BodyControl 명령은 LIN Master가 있어야 동작 |
| 수신 검사 위치 | Gateway가 Phase 2 codec을 그대로 사용 (`CanInputValidator`). 29-bit ID·RTR·error frame은 `ERR_FRAME_FORMAT`으로 먼저 거부 | §5.1 Classical 11-bit만 사용 |
| validity/freshness bitmap | bit0 RPM, bit1 speed, bit2 throttle, bit3 coolant, bit4 brake pressure, bit5 ABS, 나머지 0 | §7.2가 "signal별 bit"만 정함 |
| validity bit | 값을 한 번 이상 받았고, 해당 PDU 연속 오류 < 3, 마지막 정상 frame의 fault bit = 0 | §5.4 연속 3회 오류 |
| freshness bit | 해당 PDU가 `FRESH`(마지막 정상 수신 후 주기의 3배 미만)일 때만 1. UNKNOWN·DEGRADED·STALE은 0 | §5.4 3배/5배 기준을 1-bit로 표현 |
| 오류 처리 | 거부된 frame은 마지막 정상값을 바꾸지 않음. 연속 3회 오류면 해당 PDU degraded, 정상 1회로 회복 | §5.4 |
| Gateway 상태 | 모든 PDU를 한 번 이상 정상 수신 전 `Startup`, 하나라도 freshness가 FRESH가 아니거나 연속 오류 ≥ 3이면 `Degraded`, 아니면 `Running`, 종료 시 `Shutdown` | §8.1 상태 목록 |
| payload timestamp | Gateway가 payload에 쓰인 PDU 중 가장 최근 정상 frame을 `read()`한 시각 (CLOCK_MONOTONIC, µs) | §7.2 "source timestamp" |
| 지연 측정 | AP consumer가 event를 받은 시각 − payload timestamp. 같은 VM이라 monotonic clock 공유. "발행"까지가 아니라 "수신"까지이므로 §15.2보다 엄격 | §15.2 p99 50ms |
| 실행 계정 | Gateway·consumer·ECU 모두 작업 폴더 주인 계정(non-root). `nobody`는 작업 폴더의 vSomeIP 라이브러리를 읽을 수 없음 | §11.1 non-root |
| vSomeIP local socket | namespace마다 다른 `network` 이름(`vsomeip-sil-gateway`, `vsomeip-sil-ap`) | namespace는 `/tmp` 파일을 공유하므로 routing manager 충돌 방지 |
| SD initial delay | 10~100ms 범위만 설정. vSomeIP가 범위 안에서 난수로 고르며 seed 고정 불가 | §7.1 "결정적 seed"는 vSomeIP 설정으로 구현 불가 → 한계로 기록 |
| 잘못된 요청 | 비어 있지 않은 `GetSnapshot` 요청은 `E_MALFORMED_MESSAGE`로 거부하고 security event 기록 | §7.2 |

알려진 한계: CRC와 counter를 맞춘 범위 안의 위조 frame은 Gateway를 통과한다(시험 `test_forged_frame_is_accepted_known_limit`). 진위성 검증(R155 Annex 5 M10, AUTOSAR SecOC 수준)은 구현하지 않는다.

구현: `src/gateway/`(입력 검증, state cache, payload, `vehicle_gateway`), `src/adaptive/vehicle-state/`(`vehicle_state_consumer`), `config/services/vehicle_state.yaml`, `tools/codegen/gen_services.py`, `config/vsomeip/`, 시험 `tests/unit/test_{can_input_validator,vehicle_state_cache,vehicle_state_payload}.cpp`, 통합 `scripts/run/gateway-someip-test.sh`.

## 8. Adaptive 실행 및 상태 관리

### 8.1 실행 관리자

교육용 Execution Manager가 version-controlled manifest를 읽어 다음을 수행한다.

- 실행 파일과 인자 확인
- 프로세스별 사용자와 capability 확인
- 의존 순서에 따른 시작과 역순 종료
- heartbeat와 process exit 감시
- restart budget 적용
- 상태 전이와 감사 로그 기록

상태는 다음으로 제한한다.

- `Startup`
- `Running`
- `Degraded`
- `Shutdown`

프로세스는 각각 독립 실행하고 CPU와 메모리 제한을 설정한다. `CAP_NET_RAW`는 CAN 또는 packet capture가 필요한 최소 프로세스에만 부여한다.

AUTOSAR Execution Management가 configuration에 따라 애플리케이션 수명주기, 의존 시작·종료와 상태 관리 지원을 담당한다는 공식 구조를 참고한다. 그러나 본 구현은 AUTOSAR Manifest와 Execution Management API를 완전 구현하지 않는 교육용 supervisor다.

### 8.2 프로젝트 시간 목표

- service discovery 완료: 시작 후 5초 이내
- 500ms heartbeat 3회 누락 시 장애 선언: 약 1.5초
- restart budget이 남아 있으면 3초 이내 재시작 시도
- 60초 안에 같은 프로세스가 3회 실패하면 자동 재시작 중지 후 `Degraded`

이 시간은 VM에서 장애 상태를 관찰하고 자동 시험하기 위한 프로젝트 합격 기준이다. 특정 차량 기능의 FTTI, OEM timing budget 또는 ISO 26262 안전 요구사항이 아니다. 제동·조향의 실시간 제어 경로에 이 수치를 적용해서는 안 된다.

### 8.3 Phase 4 세부 결정 (2026-11-09 제안, 팀 확인 필요)

| 항목 | 결정 | 근거 |
| --- | --- | --- |
| manifest | `config/manifests/execution.json`. `${VAR}`는 실행 시 `--var`로 채움(연습 `hj_*` / 공식 `ns_*` 공용). 이름·절대 경로·인자·limits·cpu 목록을 로드 시 검사 | §8.1, §11.1 configuration schema 검증. JSON은 고정한 Boost.PropertyTree(header-only)로 읽음 |
| 관리 대상 | engine, brake, steering, gateway, consumer 5개 | Phase 2·3 프로세스 |
| 시작 순서 | CAN ECU 3개 → 셋 다 준비되면 Gateway → Gateway 준비되면 consumer. "준비" = 첫 heartbeat 수신 | §8.1 의존 순서. Gateway는 CAN 입력이 있어야 의미 있는 서비스를 제공 |
| 종료 순서 | 시작의 역순, 한 번에 하나씩 SIGTERM, 2초 안에 안 끝나면 SIGKILL | §8.1 역순 종료 |
| heartbeat | 각 프로세스가 주 작업 루프에서 500ms마다 unix datagram(`HB1 <name> <seq>`)을 EM socket으로 보냄. EM은 커널이 붙여 주는 송신 pid(SO_PASSCRED)로 신원을 확인하고, 관리 pid가 아니거나 이름이 다르면 무시하고 security 이벤트 기록 | §8.2 500ms·3회. 이름만 믿으면 다른 프로세스가 대신 살아 있다고 속일 수 있음 |
| 장애 판정 | 예상하지 못한 종료, heartbeat 3회 누락(1.5초), 시작 후 5초 안에 첫 heartbeat 없음 | §8.2 |
| 재시작 | 장애 후 500ms 뒤 재시작(이전 프로세스가 완전히 끝난 뒤). 같은 프로세스가 60초 안에 3회 장애면 자동 재시작 중지 | §8.2 3초 이내 재시도, 60초 3회 |
| 상태 전이 | 모든 프로세스가 한 번 Running이 되기 전 `Startup`, 모두 Running이면 `Running`, 하나라도 재시작 중이거나 재시작 중지 상태면 `Degraded`, 종료 요청 후 `Shutdown` | §8.1 상태 4개 |
| 실행 권한 | EM만 root로 실행(namespace 진입과 권한 하강에 필요). 각 프로세스는 manifest 사용자로 setresuid/setresgid, 보조 그룹 없음, capability 0, `no_new_privs`, 자기 process group. 시작 직후 `/proc`에서 uid·CapEff·CapPrm·NoNewPrivs·CPU·netns를 다시 읽어 확인하고 어긋나면 종료 | §8.1 사용자·capability 확인, §11.1 non-root·최소 capability |
| CAP_NET_RAW | 어떤 프로세스에도 주지 않음. manifest에 capability가 있으면 로드 거부 | CAN_RAW socket은 capability 없이 동작함(Phase 2 주기 시험에서 `nobody`로 송수신 확인) |
| 자원 제한 | 메모리 `RLIMIT_AS`(ECU 64MB, Gateway·consumer 1024MB), 파일 수 `RLIMIT_NOFILE`, core 0, CPU는 `sched_setaffinity`(ECU cpu1, Gateway cpu2, consumer cpu3, cpu0은 시스템·SSH용), nice | §8.1 CPU·메모리 제한. cgroup quota는 서버 설정을 건드리므로 쓰지 않음 → CPU는 "사용 코어 제한"이지 "사용률 상한"이 아님 |
| 실행 파일 확인 | 절대 경로, 일반 파일, 실행 권한, 그룹·기타 쓰기 금지, 소유자 root 또는 실행 사용자, 상위 폴더 world-writable 금지. 인자는 셸 없이 배열로 execve | §8.1 실행 파일과 인자 확인 |
| 감사 로그 | JSONL: timezone 포함 시각, monotonic ns, 종류(start·ready·verify·exit·failure·restart_scheduled·budget_exhausted·stop·kill·state·security), 프로세스, pid, 상세 | §8.1 감사 로그, §10 사건 필드 |

한계: AUTOSAR Execution Management API·Function Group·State Management를 구현하지 않는다. heartbeat는 "주 루프가 돌고 있다"만 보여 주며 기능이 올바른지는 보장하지 않는다. EM 자신은 감시받지 않는다(실차의 watchdog 하드웨어 없음).

구현: `src/adaptive/execution-manager/`(`supervisor` 판단 로직, `manifest`, `execution_manager`), `src/common/heartbeat.c`, 시험 `tests/unit/test_supervisor.cpp`, `tests/unit/test_manifest.cpp`, 시나리오 `scripts/run/em-scenario-test.sh`.

## 9. 진단

### 9.1 경로

`Adaptive Diagnostic Client → DoIP → Gateway → ISO-TP → UDS ECU Server`

- DoIP TCP/UDP port: `13400`
- Tester logical address: `0x0E80`
- Gateway logical address: `0x1000`
- Engine logical address: `0x1001`
- Brake logical address: `0x1002`
- Engine ISO-TP request/response: `0x7E0` / `0x7E8`
- Brake ISO-TP request/response: `0x7E1` / `0x7E9`

논리 주소와 CAN ID는 프로젝트 선택값이다. Linux kernel ISO-TP socket을 사용해 Single Frame뿐 아니라 First Frame, Flow Control와 Consecutive Frame을 포함한 다중 프레임 경로를 시험한다.

### 9.2 UDS 서비스

- `0x10 DiagnosticSessionControl`: Default와 Extended session
- `0x11 ECUReset`: ECU 프로세스 내부 상태 초기화만 수행
- `0x14 ClearDiagnosticInformation`
- `0x19 ReadDTCInformation`
- `0x22 ReadDataByIdentifier`
- `0x27 SecurityAccess`
- `0x3E TesterPresent`

Programming Session, download/upload, flash memory와 부트로더 업데이트는 초기 범위에서 제외한다. 서명 검증, rollback, 전원 손실 복구가 없는 가짜 업데이트를 구현하고 양산 업데이트 경험으로 주장하지 않기 위해서다.

### 9.3 프로젝트 DID

- `0xF100`: Engine speed
- `0xF101`: Vehicle speed
- `0xF102`: Coolant temperature
- `0xF110`: Brake pressure
- `0xF111`: ABS state
- `0xF180`: Software version
- `0xF181`: ECU state
- `0xF182`: Active fault bitmap

DID는 프로젝트 내부 값이며 OEM DID가 아니다.

### 9.4 권한과 timeout

- Default Session: 상태와 DTC 읽기만 허용
- Extended Session: SecurityAccess 성공 후 reset과 DTC clear 허용
- TesterPresent가 5초 동안 없으면 Default Session으로 복귀
- SecurityAccess 실패 3회 후 10초 지연
- 같은 실행 주기에서 5회 실패하면 ECU 재시작 전까지 잠금

교육용 SecurityAccess는 16-byte seed와 HMAC-SHA-256 기반 응답을 사용한다. 실습키는 소스에 넣지 않고 권한 `0600`의 로컬 secret 파일에 저장한다. ECU와 키가 같은 VM에 있으므로 HSM, secure element 또는 실제 OEM key provisioning과 동등하지 않다.

### 9.5 Phase 5 세부 결정 (2026-11-10 제안, 팀 확인 필요)

단일 원본은 `config/diagnostics/uds.yaml`이며 C(ECU), C++(Gateway), Python(테스터)이 같은 파일을 쓴다.

| 항목 | 결정 | 근거 |
| --- | --- | --- |
| DoIP 버전 | ISO 13400-2:2012 (protocol version `0x02`). 차량 식별 요청만 `0xFF`도 허용 | 공개 표준의 널리 쓰이는 판 |
| 차량 식별 값 | VIN `SILTEST0000000001`(실제 VIN 아님), EID·GID `02:00:00:00:10:00`(로컬 관리 MAC 형식) | 기준서에 없음. 실제 차량·제조사 값으로 오인되지 않게 함 |
| routing activation | tester `0x0E80`·activation type `0x00`만 허용, 그 밖은 거부 후 연결 종료. activation 전 진단 메시지는 NACK `0x02` 후 종료 | ISO 13400-2, §9.1 |
| 연결 | tester 연결 1개만, 두 번째 연결은 즉시 종료. activation 없이 2초, 무통신 5분이면 종료 | ISO 13400-2 T_TCP_Initial/General_Inactivity |
| Gateway 자신 | `0x1000`은 차량 식별에만 사용, Gateway 자체 UDS는 없음(진단 메시지는 NACK `0x03`) | §9.1이 Gateway UDS 서비스를 정하지 않음 |
| 전달 방식 | Gateway는 UDS 내용을 해석하지 않고 ISO-TP로 전달한 뒤 positive ack, ECU 응답은 오는 대로 비동기 전달. Flow Control을 받지 못하면 NACK `0x06` | 응답 없는 요청(TesterPresent `0x80`)이 다음 요청을 막지 않게 함 |
| ISO-TP | 커널 CAN_ISOTP socket, 8-byte padding `0xCC`, 나머지(BS·STmin·N_Bs)는 커널 기본값 | §9.1 커널 ISO-TP |
| ECU별 DID | Engine `F100 F102 F180 F181 F182`, Brake `F101 F110 F111 F180 F181 F182`. 값은 CAN 신호와 같은 raw 단위 | §9.3, 차량 속도는 Brake 송신(§5.5) |
| F181 / F182 | F181 = [세션, 보안 상태(0 잠김, 1 해제, 2 지연, 3 잠금)], F182 = DTC 순번별 testFailed bitmap | §9.3 "ECU state", "Active fault bitmap"의 형식 정의 |
| DTC | Engine `P0117`(0x011700), `P0335`(0x033500), Brake `C0035`(0x403500). ISO 15031-6/SAE J2012 공통 코드 형식, status bit0 testFailed + bit3 confirmed. 시험용 `--inject-dtc`로만 발생 | 기준서에 없음. OEM 고유 코드가 아닌 공통 코드 |
| 0x19 | `0x01` 개수, `0x02` 목록만 | §9.2 범위 |
| 0x14 | 전체 그룹 `FFFFFF`만 | 그룹별 삭제는 범위 밖 |
| 0x11 | hardReset `0x01`만. 세션·보안 상태만 초기화, DTC 기억과 SecurityAccess 실패 횟수·잠금은 유지 | §9.2 "프로세스 내부 상태 초기화", §9.4 "ECU 재시작 전까지 잠금"은 프로세스 재시작으로 해석 |
| 0x27 | level `0x01/0x02`. 응답 = HMAC-SHA-256(key, ECU 논리 주소 2B ∥ seed 16B), 32B. seed는 한 번만 유효. 해제 상태에서 seed 요청 시 0 seed | §9.4. 논리 주소를 넣어 한 ECU의 응답을 다른 ECU에 재사용 못 하게 함 |
| 실패 처리 | 3번째 실패에 NRC `0x36` + 10초 지연(지연 중 seed 요청 `0x37`), 5번째 실패에 `0x36` + 프로세스 재시작 전까지 잠금 | §9.4 |
| 세션 | 모든 세션 전환은 보안을 다시 잠금. S3 5초 동안 요청이 없으면 Default로 | ISO 14229-1, §9.4 |
| 시간 값 | P2 50ms, P2* 5000ms(응답에 표기), S3 5000ms | ISO 14229-2 기본값 |
| 키 파일 | `~/.config/vehicle-sil/diag_key`, 32B, 실행 사용자 소유 `0600`이 아니면 진단 기능을 켜지 않음 | §9.4 |
| 테스터 | 시험용 Python 테스터(`tools/diag/doip_tester.py`)가 AP 방에서 Diagnostic Client 역할 | §3.2 시험 도구는 Python. 운영용 C++ Diagnostic Client는 범위 밖 |

한계: HMAC 키가 ECU 프로세스와 같은 VM의 파일에 있으므로 키 보호는 파일 권한뿐이다(HSM 없음). Gateway는 UDS 내용을 검사하지 않는다(진단 방화벽은 범위 밖). DoIP는 TLS(ISO 13400-2:2019 보안 연결)를 쓰지 않는다.

## 10. 로깅, 저장과 대시보드

- C/C++ 런타임 로그: COVESA DLT
- 보안·진단 사건 저장: SQLite
- Dashboard API: FastAPI
- UI 갱신: WebSocket 또는 Server-Sent Events 중 구현 시 선택
- 패킷 근거: `candump`, `tcpdump`와 Wireshark capture

모든 사건은 다음 필드를 가진다.

- timezone 포함 발생 시각
- monotonic 관측 시각
- experiment ID
- source node와 interface
- rule ID
- severity
- CAN/LIN/SOME-IP/UDS 식별자
- 관측값
- threshold와 window
- 처리 결과

대시보드는 모니터링 경로이며 차량 제어권을 갖지 않는다. Dashboard가 정지해도 ECU와 Gateway 통신은 계속되어야 한다.

## 11. 보안 구조와 공격 실험

### 11.1 기본 통제

- namespace 기반 네트워크 경계
- nftables allowlist와 기본 deny
- non-root 실행
- 최소 Linux capability
- version-pinned dependency와 checksum 기록
- source 밖 local secret 파일
- 입력 길이, ID, DLC와 범위 검증
- 로그와 dashboard의 비동기 처리
- configuration schema 검증

### 11.2 IDS 규칙

- 등록되지 않은 CAN ID
- DLC 또는 reserved byte 불일치
- 신호 범위 초과
- CRC 오류
- alive counter 중복, 역행 또는 비정상 점프
- 3주기 이상 메시지 누락
- 1초 window에서 ID별 예상 횟수의 3배 초과
- 알 수 없는 SOME/IP service/method
- SOME/IP payload 길이 또는 version 불일치
- Service Discovery 과다 전송
- 초당 20회를 넘는 진단 요청이 2초 이상 지속
- 60초 안에 SecurityAccess 실패 5회
- LIN protected ID, checksum 또는 schedule 위반
- LIN 응답 연속 3회 누락

임계값은 `config/security-policy.yaml`에서 관리하고 로그에 함께 저장한다. 정상 시나리오에서도 오탐이 없는지 먼저 확인한다.

### 11.3 공격 도구 제한

- 프로젝트가 생성한 `vcan` 또는 `vxcan`만 허용
- 물리 CAN 인터페이스 거부
- 최대 1000 frame/s
- 최대 10초
- 최대 10,000 frame
- Ctrl+C와 timeout 중단 처리
- 실행 전 namespace, interface type와 소유 marker 확인
- 캡처 파일의 ID, DLC, 길이와 시간 간격 검증

setup script는 `/run/vehicle-sil/owned-interfaces.json`을 만들고 `run_id`, namespace, interface, `vcan`/`vxcan` kind와 생성 시각을 기록한다. 공격 도구는 이 파일의 현재 `run_id`와 `ip -details -json link show`의 `linkinfo.info_kind`가 모두 일치할 때만 실행한다. `vcan` 또는 `vxcan`이 아니거나 marker가 없으면 종료한다. frames/s와 payload bytes/s만 보고하며 이를 실제 CAN bus utilization으로 환산하지 않는다.

시나리오는 flooding, out-of-range spoofing, counter/CRC 변조, replay, unauthorized ID, UDS session/reset 남용, SecurityAccess 반복 실패와 LIN 응답 누락을 포함한다.

payload에 인증 정보가 없으면 범위 안의 spoofing을 확정할 수 없다. counter와 CRC는 우발적 오류와 단순 replay 탐지에는 도움이 되지만 비밀키 기반 인증이 아니므로 공격자가 다시 계산할 수 있다. 이 한계를 IDS 결과에 기록한다.

## 12. TARA 범위

주요 자산:

- CAN ECU가 제공하는 차량 상태의 무결성과 가용성
- Gateway 변환 정책과 라우팅
- SOME/IP 서비스와 Service Discovery
- UDS 진단 세션과 권한
- LIN body command와 상태
- 보안 로그와 실험 근거
- local secret과 configuration

주요 공격 경로:

- 가상 CAN frame injection
- SOME/IP service impersonation 또는 malformed request
- DoIP/UDS 진단 남용
- Gateway 또는 ECU 프로세스 종료
- configuration 변조
- log flooding

TARA는 자산, 위협 시나리오, 공격 경로, 영향, 가능성 근거, 대응과 잔여 위험을 연결한다. SIL의 실험 결과와 실제 차량에 대한 가정은 분리한다. H/M/L 등급은 제조사 위험 기준이 없으므로 프로젝트 내부 우선순위로만 사용한다.

## 13. 실차와 유사한 부분 및 근거

### 13.1 도메인 Gateway 구조

CAN·LIN 하위 네트워크를 Gateway가 Ethernet 서비스로 변환하고 Adaptive 중앙 노드가 직접 하위 버스를 소유하지 않게 한다. 이는 Classic형 ECU와 Adaptive 컴퓨팅 노드가 공존하는 E/E 구조를 학습하기 위한 선택이다. AUTOSAR는 Classic Platform에서 여러 차량 버스 통합을, Adaptive Platform에서 POSIX 기반 virtual machine, service/API와 동적 서비스 연결을 설명한다.

### 13.2 서비스 지향 통신

provider/consumer, service discovery, request/response와 event를 분리한다. vSomeIP은 SOME/IP, configuration, Service Discovery와 E2E 모듈을 제공하므로 무료 환경에서 실제 IP stack과 packet capture를 사용해 이 구조를 검증할 수 있다.

### 13.3 독립 프로세스와 수명주기

각 ECU와 Adaptive 애플리케이션을 독립 프로세스로 실행하고 manifest, dependency, state, heartbeat와 restart budget으로 관리한다. 이는 하나의 스크립트 안에서 모든 ECU를 함수로 호출하는 것보다 프로세스 장애와 격리를 실험하기 적합하다. AUTOSAR Execution Management의 application lifecycle, 의존 순서와 상태 관리 책임을 참고한다.

### 13.4 진단 경로

DoIP에서 Gateway를 거쳐 ISO-TP/UDS ECU server로 이어지는 다단 경로를 사용한다. Linux kernel은 SocketCAN과 ISO-TP socket API를 제공하므로 단일 CAN frame 흉내가 아니라 다중 프레임 transport를 시험할 수 있다.

### 13.5 통신 보호와 상태 품질

PDU에 counter, validity와 CRC를 두고 Gateway가 freshness와 오류 상태를 함께 서비스로 전달한다. 단순 값 전달보다 누락·중복·손상과 stale data 처리 경험을 제공한다. 이 구현은 개념적 유사성만 주장하며 AUTOSAR E2E 적합성은 주장하지 않는다.

## 14. 실차와 다른 부분, 이유와 검증 경계

| 항목 | 실차 환경 | 이 프로젝트 | 다른 이유 | 여기서 검증 가능한 것 | 검증할 수 없는 것 |
| --- | --- | --- | --- | --- | --- |
| ECU 하드웨어 | 차량용 MCU/SoC, watchdog, 전원·reset 회로 | x86-64 VM의 Linux 프로세스 | 무료이며 전용 ECU가 없음 | 프로세스 분리, 상태기계, 오류 처리 | 전원 변동, MCU interrupt, watchdog 회로 |
| 운영체제 | RTOS 또는 차량용 POSIX OS, 결정적 scheduling | 일반 Ubuntu guest kernel | 무료 범용 환경 우선 | lifecycle, resource limit, 평균·분포 측정 | hard real-time, WCET, FTTI 보장 |
| CAN | controller, transceiver, 배선, arbitration, error frame | SocketCAN `vcan`/`vxcan` | CAN 장비가 없음 | frame API, ID/DLC/payload, filtering, routing | 전압, bitrate 오차, arbitration 지연, bus-off, EMC |
| LIN | Master header, 물리 transceiver, break/sync와 전압 | 프로세스 broker와 schedule timer | LIN 장비가 없음 | schedule, publisher, checksum, timeout, sleep/wakeup 논리 | wire timing, oscillator tolerance, dominant/recessive 전압 |
| Ethernet | 차량용 switch, PHY, VLAN/TSN 가능 | `veth`와 Linux IP stack | 물리 switch가 없음 | IP 주소, TCP/UDP, multicast, firewall, SOME/IP/DoIP | PHY 장애, switch queue, TSN timing |
| Adaptive Runtime | ARA API, ARXML, 정식 Manifest와 통합 toolchain | vSomeIP과 자체 wrapper·manifest | 무료 공개 구성만 사용 | service lifecycle과 packet-level 동작 | AUTOSAR 전체 API 및 적합성 |
| Execution Management | 표준 요구에 따른 platform 구현과 안전 논증 | 교육용 supervisor | 전체 AP 구현 범위가 아님 | 시작 순서, 종료, heartbeat, restart policy | 양산 안전성, authenticated boot chain |
| E2E 보호 | 선택 Profile의 정확한 규칙과 상태기계 | 프로젝트 CRC/counter profile | 학습 범위에 맞춘 단순화 | 손상, 누락, 중복, 순서 오류 주입 | AUTOSAR E2E Profile 적합성, 악의적 위조 방지 |
| 진단 | OEM DID/DTC, 권한, workshop와 backend 연계 | 프로젝트 DID/DTC와 교육용 SecurityAccess | OEM 자료와 HSM이 없음 | ISO-TP, UDS session, NRC, 권한 상태 | OEM 진단 적합성, 실제 key provisioning |
| 보안키 | HSM/secure element, secure boot, 제조 provisioning | mode `0600` local secret | 무료 VM에 보안 하드웨어가 없음 | key 미노출, 권한과 실패 제한 | 물리 공격 저항, hardware-backed key |
| 차량 동역학 | 센서, actuator, powertrain과 chassis plant | 재현 가능한 소프트웨어 시나리오 | 실제 차량과 plant model이 없음 | 메시지 일관성, 상태 전이, fault injection | 제동거리, 조향 안정성, 엔진 토크 |
| IDS 성능 | 실제 traffic, ECU jitter, 다양한 차량 상태 | 합성 baseline과 라벨된 공격 | 실차 데이터가 없음 | 규칙 동작, 오탐·미탐과 탐지 지연의 SIL 측정 | 실차 탐지율 일반화 |
| 안전·인증 | ISO 26262 프로세스, 안전 목표, HIL·실차 시험 | 교육용 요구사항과 자동 시험 | 조직, 대상 차량과 인증 증거가 없음 | 요구사항-시험 추적과 실패 증거 | ASIL, homologation, 양산 승인 |

실차와 다른 이유는 대부분 “무료라서”만이 아니다. 대상 차량의 요구사항, 하드웨어, DBC, 안전 목표와 승인된 시험 환경이 없기 때문이다. 소프트웨어 구조를 유사하게 만드는 것과 차량 성능을 입증하는 것은 별개의 문제다.

## 15. 합격 기준

### 15.1 빌드와 단위 시험

- C11/C++20 build가 warning을 error로 처리한 상태에서 성공
- PDU codec의 정상값, 경계값, reserved, endian, CRC와 counter 시험 통과
- UDS positive response와 지원하지 않는 service/subfunction의 negative response 시험 통과
- LIN parity, checksum, schedule와 timeout 시험 통과
- IDS 정상 trace에서 오탐 0건, 각 공격 fixture에서 기대 event 생성
- 정상 시나리오 30분 1회와 각 공격 시나리오 seed 고정 5회에서 결과 수집

### 15.2 통합 시험

- `ns_ap`에서 CAN device 직접 접근 실패
- Gateway만 CAN과 Ethernet 양쪽에 접근
- 시작 후 5초 안에 VehicleState service 발견
- CAN PDU가 Gateway 검사를 거쳐 SOME/IP event에 반영
- CAN 수신부터 SOME/IP event 발행까지 무부하 VM 기준 p99 50ms 이하
- DoIP → Gateway → ISO-TP → UDS DID 읽기 성공
- 인증 전 reset과 DTC clear 거부
- ECU 종료 후 heartbeat 3회 누락 안에 `Degraded` 기록
- restart budget 범위에서 3초 안에 재시작 시도
- LIN Slave 중단 후 응답 3회 누락 안에 오류 기록
- 공격 시작 후 1초 안에 대응 IDS event 기록
- DLT와 SQLite event가 동일 experiment ID를 보유

각 실험은 `docs/results/YYYYMMDD-HHMMSS_<scenario>_<run-id>/`에 `manifest.json`, `labels.jsonl`, `ids-events.jsonl`, `summary.md`를 저장한다. packet capture와 대용량 raw log는 같은 이름으로 생성하되 Git에는 포함하지 않는다. `manifest.json`은 VM·kernel·dependency version, commit, seed, 시작·종료 시각, namespace/interface, rate·duration·frame count와 실행 명령을 포함한다. `labels.jsonl`은 ground truth 시작·종료와 attack type을 기록한다.

50ms, 1초와 3초는 실차 제조사 요구사항이 아니라 VM에서 반복 가능한 자동 시험을 위한 프로젝트 합격 기준이다. 시험 결과에는 VM 자원, kernel, dependency version, 부하, 표본 수와 측정 명령을 기록한다.

### 15.3 장시간 시험

- 정상 시나리오 30분 실행
- process crash 없음
- 정상 PDU에서 CRC/counter 오류 없음
- event queue 무제한 증가 없음
- SQLite와 DLT 기록 건수 차이 설명 가능
- CPU와 RAM peak 기록

이는 내구성 인증이 아니라 memory leak, queue 적체와 주기 흔들림을 찾는 개발 시험이다.

## 16. 구현 순서

각 단계는 독립적으로 실행하고 증거를 남긴 뒤 다음 단계로 진행한다.

### Phase 0. 재현 가능한 Ubuntu 기준선

- 패키지와 dependency version 고정
- VM 사양, kernel과 설치 명령 기록
- build와 test 기본 명령 확정
- 완료 증거: 새 VM에서 동일 명령으로 build 환경 재현

### Phase 1. 네트워크 격리

- namespace, veth, vxcan와 nftables 구성
- idempotent setup/cleanup script
- 완료 증거: AP의 직접 CAN 접근 실패와 Gateway 양방향 접근 성공

### Phase 2. CAN PDU와 ECU

- 공통 signal definition과 codec
- Engine, Brake, Steering, Transmission 독립 프로세스
- 정상 시나리오와 fault injection
- 완료 증거: 알려진 payload, CRC/counter, 주기 분포 시험

### Phase 3. Gateway와 SOME/IP

- CAN validation과 state cache
- VehicleState와 BodyControl service
- service discovery와 consumer
- 완료 증거: CAN 입력부터 SOME/IP event까지 자동 통합 시험

### Phase 4. 실행 및 상태 관리

- manifest, dependency, heartbeat, restart budget
- Startup/Running/Degraded/Shutdown
- 완료 증거: crash, hang와 반복 실패 시나리오

### Phase 5. DoIP와 UDS

- DoIP entity
- Linux ISO-TP socket
- Engine/Brake UDS server
- session, DID, DTC와 SecurityAccess
- 완료 증거: 정상·권한 부족·timeout·multi-frame 시험

### Phase 6. LIN Body Domain

- Master schedule와 세 Slave
- checksum, parity, sleep/wakeup와 응답 누락
- BodyControl SOME/IP mapping
- 완료 증거: 명령-응답, 오류와 복구 시나리오

### Phase 7. 공격과 IDS

- 정상 baseline 생성
- 제한된 공격 도구
- 규칙 기반 IDS와 결과 집계
- 완료 증거: 오탐·미탐, 탐지 지연과 자원 사용 보고서

### Phase 8. 로그, 대시보드와 TARA

- DLT, SQLite와 read-only dashboard
- TARA와 요구사항-시험 추적표
- 완료 증거: 전체 end-to-end demo와 재현 명령

## 17. 목표 디렉터리 구조

```
.
├── CMakeLists.txt
├── project.md
├── README.md
├── cmake/
├── config/
│   ├── manifests/
│   ├── network/
│   ├── signals/
│   ├── security-policy.yaml
│   └── vsomeip/
├── src/
│   ├── common/
│   ├── ecu/
│   │   ├── engine/
│   │   ├── brake/
│   │   ├── steering/
│   │   └── transmission/
│   ├── gateway/
│   ├── adaptive/
│   │   ├── execution-manager/
│   │   ├── state-manager/
│   │   ├── vehicle-state/
│   │   └── diagnostics/
│   ├── lin/
│   ├── ids/
│   └── logging/
├── dashboard/
├── scripts/
│   ├── setup/
│   ├── run/
│   └── cleanup/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── scenarios/
│   └── fixtures/
├── tools/
│   ├── attacks/
│   ├── capture/
│   └── analysis/
├── docs/
│   ├── evidence/
│   ├── results/
│   └── tara/
└── tmp/
    └── legacy/
```

`tmp/legacy/`는 이전 Wokwi, 발표자료, 과거 문서와 빌드 산출물을 보관하는 격리 폴더다. 새 프로젝트의 빌드, 테스트, 요구사항과 성과 주장에 포함하지 않는다. 필요한 알고리즘을 다시 사용하려면 새 명세와 시험을 기준으로 별도 검토 후 새 소스에 구현한다.

## 18. 완료 시 제시할 증거

- 고정된 dependency와 재현 가능한 설치 절차
- namespace와 firewall 상태
- CTest와 pytest 전체 결과
- CAN/LIN/SOME-IP/DoIP packet capture
- 정상·장애·공격별 experiment manifest
- IDS confusion matrix와 탐지 지연
- CPU, RAM와 queue 사용량
- 요구사항-코드-시험 추적표
- 실차와 다른 점 및 검증하지 못한 항목
- 실패한 시험과 남은 위험

포트폴리오에서는 구현한 기능보다 증거가 우선한다. 실행하지 않은 기능, 통과하지 않은 시험과 제공되지 않은 제조사 요구사항은 완료된 것처럼 표현하지 않는다.

## 19. 공식 및 공개 근거

- AUTOSAR Adaptive Platform R25-11
- AUTOSAR Explanation of Adaptive Platform Design R25-11
- AUTOSAR Requirements on Execution Management R25-11
- AUTOSAR Communication Management R25-11
- AUTOSAR Adaptive Interface Guidelines R25-11
- AUTOSAR Classic Platform
- AUTOSAR LIN Requirements R25-11
- AUTOSAR LIN Interface R25-11
- Linux SocketCAN
- Linux ISO-TP
- Linux network namespaces
- Linux ip-link: veth와 vxcan
- COVESA vSomeIP
- COVESA DLT daemon
- Ubuntu Desktop LTS

외부 링크는 구조와 도구 기능의 근거다. 링크가 존재한다는 사실만으로 이 프로젝트가 표준 적합성, 안전성 또는 실차 성능을 충족하는 것은 아니다. 구현 시 사용 버전과 실제 시험 결과를 별도로 남긴다.
