# Phase 5. DoIP와 UDS 진단 (호진)

> 상태: 🟢 구현·단위 시험·진단 시나리오 시험 통과 (2026-11-09)
> 날짜: 2026-11-10
> 환경: chakan VM (Ubuntu 26.04.1, kernel 7.0.0-42, g++ 15.2, CMake 4.2.3 + Ninja), OpenSSL 3.5.5 (Ubuntu 패키지, 이미 설치됨)
> 관련: `project.md` §9.1~9.4, §9.5(이번 결정)
> ⚠️ 기준서에 없던 세부값은 **팀 확인 필요**

---

## ✅ 할 일 / 완료 기준

- [x] DoIP → Gateway → ISO-TP → UDS 경로
- [x] Engine·Brake 진단 서버
- [x] DID·DTC·세션·SecurityAccess 구현

**완료 기준**
- [x] 정상·권한 부족·시간 초과·다중 프레임 시험 통과 — 실제 DoIP → ISO-TP 경로 39/39

---

## 1. 한 줄 요약

정비소 진단기 역할(AP의 테스터)이 **DoIP(Ethernet)** 로 Gateway에 접속하면, Gateway가 요청을 **ISO-TP(CAN)** 로 Engine·Brake에 전달하고, 각 ECU의 **UDS 진단 서버**가 값 읽기, 고장 코드(DTC) 읽기·삭제, 보안 접근을 처리함.

```
[AP 테스터] ──DoIP(TCP/UDP 13400)──> [Gateway] ──ISO-TP(CAN 0x7E0/0x7E8, 0x7E1/0x7E9)──> [Engine·Brake UDS 서버]
 주소 0x0E80                          0x1000                                              0x1001 / 0x1002
```

---

## 2. 쉽게 이해하기

| 용어 | 쉬운 뜻 |
| --- | --- |
| UDS | 진단기와 ECU가 쓰는 **질문·답변 규칙**. "엔진 회전수 알려줘(0x22)", "고장 코드 지워(0x14)" 같은 명령 번호가 정해져 있음 |
| ISO-TP | CAN은 한 번에 8바이트만 보낼 수 있어서, 긴 메시지를 **여러 조각으로 나눠 보내고 다시 합치는** 규칙 |
| 다중 프레임 | 긴 메시지 = 첫 조각(FF) → 받는 쪽 "계속 보내"(FC) → 나머지 조각(CF) |
| DoIP | UDS를 **Ethernet(TCP/IP)** 으로 보내는 규칙. 요즘 차는 OBD 포트 뒤에서 이걸로 진단 |
| 세션 | 진단 모드. Default(읽기만) / Extended(위험한 작업 가능) |
| SecurityAccess | 위험한 작업 전 **암호 확인.** ECU가 무작위 문제(seed)를 내면 진단기가 비밀 키로 답(key)을 계산해 제출 |
| NRC | ECU가 거절할 때 붙이는 **거절 이유 번호** (0x33 보안 미해제, 0x7F 이 세션에선 안 됨 등) |

**왜 보안에서 중요한가:** 진단은 고장 코드 삭제, ECU 재시작처럼 **차량 상태를 바꾸는 강력한 기능**이라, 아무나 쓰지 못하게 세션·보안 접근·시도 횟수 제한을 둠. UN R155 Annex 5는 진단 메시지를 통한 공격과 권한 남용을 위협으로 다루고, 21434 테스트 매뉴얼은 "허용되지 않은 세션의 서비스 요청", "인증 전 민감 서비스 요청"을 시험 항목으로 듦.

---

## 3. 구현 내용

### 3.1 UDS 서버 (Engine·Brake, `ECU/src/common/uds_server.c`)
| 서비스 | 내용 | 허용 조건 |
| --- | --- | --- |
| 0x10 세션 | Default(0x01) / Extended(0x03) | 항상 |
| 0x22 값 읽기 | DID 최대 8개 한 번에 | 항상 |
| 0x19 DTC 읽기 | 개수(0x01) / 목록(0x02) | 항상 |
| 0x3E TesterPresent | 세션 유지 신호, 응답 생략(0x80) 지원 | 항상 |
| 0x27 SecurityAccess | seed 16B → key = HMAC-SHA-256 32B | Extended |
| 0x14 DTC 삭제 | 전체 그룹 | Extended + 보안 해제 |
| 0x11 ECUReset | 프로세스 내부 상태만 초기화 | Extended + 보안 해제 |

**보안 규칙 (§9.4)**
- seed는 한 번만 유효 (같은 답을 재사용하는 재전송 공격 차단)
- 답 = HMAC-SHA-256(비밀 키, **ECU 주소** + seed) → Engine용 답을 Brake에 쓸 수 없음
- 3번 틀리면 **10초 동안** seed 요청 거부, 5번 틀리면 **ECU 재시작 전까지 잠금**
- 세션이 바뀌면 다시 잠김, **5초 동안 요청이 없으면** Default로 돌아가며 잠김
- 비밀 키는 소스가 아닌 `~/.config/vehicle-sil/diag_key` (32B, 본인만 읽기 0600). 권한이 다르면 진단 기능을 켜지 않음

### 3.2 DID와 DTC
| ECU | DID | DTC (시험용 주입) |
| --- | --- | --- |
| Engine | F100 회전수, F102 냉각수 온도, F180 SW 버전, F181 상태, F182 고장 bitmap | P0117 냉각수 온도 회로 Low, P0335 크랭크 위치 센서 |
| Brake | F101 차량 속도, F110 브레이크 압력, F111 ABS, F180, F181, F182 | C0035 왼쪽 앞바퀴 속도 센서 |

### 3.3 Gateway DoIP (`ECU/src/gateway/doip.cpp`, `doip_main.cpp`)
- UDP 13400: 차량 식별 요청 → VIN·Gateway 주소·EID 응답
- TCP 13400: **routing activation**(등록된 진단기 0x0E80만 허용) → 진단 메시지 전달
- 활성화 전 진단 요청, 다른 주소를 사칭한 요청 → 거부하고 연결 종료
- 알 수 없는 대상 주소 → NACK 0x03
- 요청은 ISO-TP로 보내고 바로 ack, ECU 응답은 오는 대로 전달 (응답 없는 요청이 다음 요청을 막지 않음)
- 진단기는 한 번에 1개, 활성화 없이 2초 지나면 연결 종료

---

## 4. 시험 결과

### 4.1 단위 시험 (2026-11-10, 직접 실행)

| 시험 | 결과 | 내용 |
| --- | --- | --- |
| `test_uds_server` | ✅ 70 checks | HMAC 값이 Python과 일치, ECU 주소별로 답이 다름, Default 권한, 보안 해제 전 거부, 해제 후 삭제·리셋, 3회 실패 10초 지연·5회 잠금, seed 재사용 차단, S3 5초, DID 4개 다중 응답, 잘못된 요청 |
| `test_doip` | ✅ 35 checks | 헤더 형식, routing activation 성공·거부, 전달·ack·응답 포장, 활성화 전·사칭 거부, 알 수 없는 대상, 3바이트씩 쪼개 온 스트림 재조립, 잘못된 버전·너무 큰 메시지·모르는 타입·잘못된 길이, UDP 차량 식별 |
| 이전 Phase 시험 | ✅ | 전체 11개 통과 (회귀 없음) |
| cppcheck / ASan·UBSan | ✅ | 경고 0, 메모리 오류 0 |

### 4.2 진단 시나리오 시험 (실행 대기)

`ECU/scripts/run/diag-scenario-test.sh`: Engine(고장 코드 주입)·Brake·Gateway를 띄우고 AP 방의 테스터가 실제 DoIP → ISO-TP 경로로 시험. 차량 쪽 CAN선을 캡처해서 다중 프레임 조각(FF/CF/FC)이 실제로 오갔는지도 확인.

| 분류 | 주요 시험 |
| --- | --- |
| 정상 | 차량 식별, routing activation, 회전수·속도 읽기, DTC 읽기, 보안 해제 후 DTC 삭제·ECUReset, 다른 ECU 계속 응답 |
| 권한 부족 | 등록 안 된 진단기 주소, 활성화 전 요청, Default에서 삭제·보안 접근(0x7F), 해제 전 삭제·리셋(0x33), 없는 DID(0x31), 범위 밖 서비스(0x11), 없는 대상(NACK 0x03), 잘못된 key 3회·5회, 잠금 중 올바른 key도 거부 |
| 시간 초과 | 10초 지연 중 거부(0x37)와 지연 후 허용, S3 5초 후 Default, TesterPresent로 세션 유지, 멈춘 ECU 응답 없음, 없는 ECU에 다중 프레임 요청 시 NACK 0x06 |
| 다중 프레임 | SW 버전(긴 응답), DID 4개·6개(긴 요청), CAN 캡처에서 FF·CF·FC 확인, 8바이트 padding |

**결과: PASS (39/39), 2026-11-09 12:29, `docs/results/20261109-122907_diag/`**

| 분류 | 통과 |
| --- | --- |
| 정상 | 13/13 |
| 권한 부족 | 12/12 |
| 시간 초과 | 7/7 |
| 다중 프레임 | 7/7 |

**정상**
| 시험 | 결과 |
| --- | --- |
| 차량 식별 (UDP broadcast) | ✅ VIN `SILTEST0000000001`, Gateway 주소 0x1000 |
| routing activation (0x0E80) | ✅ 응답 코드 0x10 |
| Engine 회전수 F100 | ✅ 800 rpm, 0.5 ms |
| Brake 차량 속도 F101 | ✅ 0 km/h (정지 구간) |
| Engine DTC 읽기 | ✅ P0117·P0335, status 0x09 |
| TesterPresent / Extended 진입 | ✅ `7E 00` / `50 03 00 32 01 F4` (P2 50ms, P2* 5000ms) |
| SecurityAccess | ✅ 16B seed → HMAC-SHA-256 key → `67 02` |
| 보안 해제 후 DTC 삭제 → 다시 읽기 | ✅ `54` → DTC 없음 |
| ECUReset → 상태 확인 | ✅ `51 01` → Default·잠김 |
| 다른 ECU 장애 중 Engine 응답 | ✅ |

**권한 부족**
| 시험 | 결과 |
| --- | --- |
| 등록 안 된 진단기 주소 0x0E81 | ✅ routing 거부(0x00) + 연결 종료 |
| routing activation 전 진단 요청 | ✅ NACK 0x02 + 연결 종료 |
| Default 세션 DTC 삭제 / SecurityAccess | ✅ NRC 0x7F |
| 보안 해제 전 DTC 삭제 / ECUReset | ✅ NRC 0x33 |
| 지원하지 않는 DID / 범위 밖 서비스(RequestDownload) | ✅ NRC 0x31 / 0x11 |
| 알 수 없는 대상 주소 0x1003 | ✅ DoIP NACK 0x03 (연결 유지) |
| 잘못된 key 3회 | ✅ 0x35, 0x35, 0x36 |
| 5회 실패 후 잠금, 잠금 중 올바른 key | ✅ NRC 0x36, 올바른 key도 거부 |

**시간 초과**
| 시험 | 결과 |
| --- | --- |
| 10초 지연 중 seed 요청 | ✅ NRC 0x37 |
| 10초 지연 후 seed 요청 | ✅ 다시 허용 |
| TesterPresent 5초 없음 (S3) | ✅ Default·잠김으로 복귀, 이후 DTC 삭제 거부 |
| TesterPresent 2초마다 8초 | ✅ Extended·해제 유지 |
| 멈춘 ECU (SIGSTOP) | ✅ ack 후 응답 없음, 6006 ms 대기 |
| 없는 ECU에 다중 프레임 요청 | ✅ Flow Control 시간 초과 → NACK 0x06 (1302 ms) |

**다중 프레임 (차량 쪽 CAN선 캡처 197 frame)**
| 시험 | 결과 |
| --- | --- |
| Engine SW 버전 응답 | ✅ 55 byte |
| Engine DID 4개 (요청 9 byte) / Brake DID 6개 (요청 13 byte) | ✅ 67 / 72 byte 응답 |
| Engine 요청 0x7E0 | ✅ FF 5, CF 14, ECU의 FC 5 |
| Engine 응답 0x7E8 | ✅ FF 6, CF 23, Gateway의 FC 6 |
| Brake 0x7E1 / 0x7E9 | ✅ FF·CF·FC 양방향 |
| 모든 진단 frame 8 byte (padding 0xCC) | ✅ |

**쉽게 말하면:** 진단기가 Ethernet으로 Gateway에 접속해 엔진·브레이크 값을 읽고, 고장 코드를 읽고 지우는 데 성공했다. 등록 안 된 진단기, 보안 해제 전 위험 명령, 틀린 비밀번호 반복은 모두 막혔고, 5초 동안 신호가 없으면 진단 모드가 자동으로 풀렸다. 긴 메시지는 CAN에서 여러 조각으로 나뉘어 오가는 것까지 확인했다.

**시험 중 발견해서 고친 문제**
1. 첫 실행(12:15)에서 1건 실패: 리눅스 ISO-TP 기본 설정은 첫 조각만 보내고 "보냈음"으로 돌아와서, 죽은 ECU에 보낸 다중 프레임 요청에도 Gateway가 ack를 보냄 → 전송 완료까지 기다리는 옵션(`CAN_ISOTP_WAIT_TX_DONE`) 적용 후 NACK 0x06 정상 동작
2. 시험 정리 단계가 멈춤: ECU를 일시정지하면 그 ECU를 띄운 `runuser`도 따라 멈춰서 정리가 끝나지 않음 → `setpriv`로 직접 실행하고, 정리 시 깨운 뒤 종료·3초 후 강제 종료하도록 변경
3. (실행 전 코드 검토) Gateway가 응답을 5초간 붙잡고 기다려서 응답 없는 TesterPresent가 다음 요청을 막을 수 있었음 → 응답을 비동기로 전달하도록 변경

## 5. 실행 방법

```bash
# 빌드 + 단위 시험 (ghwls)
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

# 진단 시나리오 시험 (root, 약 1분 — 중간에 Ctrl+C 금지)
su -
/home/ghwls/work/practice/scripts/setup.sh
/home/ghwls/work/scripts/run/diag-scenario-test.sh
exit
```

처음 실행하면 실습 키(`~/.config/vehicle-sil/diag_key`)를 자동으로 만듦.

---

## 6. 주장할 수 있는 것 / 없는 것

✅ 주장 가능
- AP 테스터 → DoIP → Gateway → 커널 ISO-TP → Engine·Brake UDS 서버 경로가 단일·다중 프레임 모두 동작 (시나리오 1회, 39/39)
- UDS 세션·보안 접근·시도 제한·S3 시간 초과가 §9.4 규칙대로 동작 (단위 시험 + 실제 경로)
- DoIP 접속 통제(등록된 진단기만, 활성화 후에만)와 잘못된 메시지 거부
- 응답 없는 ECU와 사라진 ECU를 진단기가 구분해서 알 수 있음 (응답 없음 / NACK 0x06)

❌ 아직 주장 불가
- 반복 실행에 따른 시간 분포 (시나리오 1회)
- 진단 공격 탐지 (SecurityAccess 반복 실패 IDS 규칙) → Phase 7
- Programming 세션, 다운로드, 플래시 → 범위 밖 (§9.2)

⚠️ 한계
- 비밀 키가 ECU와 같은 VM 파일에 있음 → 파일 권한으로만 보호 (HSM·secure element 아님)
- Gateway는 UDS 내용을 검사하지 않음 (진단 방화벽 없음)
- DoIP 암호화(TLS) 없음
- DTC는 시험용 주입으로만 생기며 실제 고장 판단 로직이 아님
- 시험용 Python 테스터이며 운영용 진단기가 아님
