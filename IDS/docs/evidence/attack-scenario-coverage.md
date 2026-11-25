# 공격 시나리오별 IDS 탐지 범위

기준일: 2026-11-24

상태: `project.md`와 사용자 제공 「08. 공격 시나리오 계획」의 전체 범위를 대조한다. 후속 구현한 외부 AutoHack C11 네 규칙과 실제 평가는 [별도 결과](autohack-rule-ids.md)에 연결한다. 이 표 전체가 구현·합격했다는 뜻은 아니다. AutoHack이 주 데이터셋이며 HCRL은 비교 자료다.

## 1. 판단 기준

규칙으로 표현할 수 있는 조건, 입력에서 관측할 수 있는 조건, 실제 공격 표본으로 검증한 조건을 구분한다. 이상 현상을 탐지한 것만으로 공격 의도나 실제 송신 ECU 신원을 확정하지 않는다. 데이터에 공격 클래스가 있다는 사실도 해당 클래스 전체의 탐지를 보장하지 않는다.

현재 SIL에는 `project.md` §5·§6·§7·§9·§11.2의 검사 정의가 있다. 외부 차량에는 §10.2에 따라 차량·버스별 정의와 정상 기준을 별도로 확인해야 한다. HCRL 정상 자료에서 제안한 네 가지 규칙은 전체 SIL IDS를 대체하는 설계가 아니다.

2026-11-24 사용자 확정에 따라 실제 데이터·확인된 차량 동작과 기존 프로젝트 가정이 충돌하면 실제 데이터를 우선한다(`project.md` §2.1.1). 기존 SIL 임계값을 맞추기 위해 원본이나 정답을 바꾸지 않는다. 아래 ‘정의 필요·미확인’ 항목은 실제 근거를 확보할 대상으로 유지하며 관측되지 않은 값을 추정해 채우지 않는다.

## 2. AutoHack에서 확인한 근거

주 출처는 Song et al., *AutoHack: A Physically Verified Multi-Bus CAN Dataset for Intrusion Detection System Evaluation*, VehicleSec 2026, pp. 237–252의 [공식 논문](https://www.usenix.org/system/files/vehiclesec26-song.pdf), §3.1~3.4다. [USENIX 서지·정정 안내](https://www.usenix.org/conference/vehiclesec26/presentation/song)도 확인했다.

- C/P/B-CAN을 구분하며 같은 CAN ID도 버스별 상태로 관리해야 한다.
- §3.2.1의 Fuzzing은 ID·DLC·payload를 변형한다. Spoofing은 정상 ID를 사용하며 물리 범위 초과만을 의미하지 않는다.
- Replay는 기존 전송 간격을 보존한다. 빈도·간격 규칙만으로 전체 Replay를 탐지한다고 주장할 수 없다.
- UDS Spoofing은 진단 시퀀스를 모방한다. 이 클래스만으로 B2의 반복 NRC나 B3의 SecurityAccess 실패 정답이 있다고 확정하지 않는다.
- §3.3의 독립 주행 train/test 분리를 유지해야 한다. 실제 배포 파일·라벨 정렬과 무결성 확인은 별도 작업이다.

제공기관의 배포 DOI는 [10.5281/zenodo.20321379](https://doi.org/10.5281/zenodo.20321379)다. 네트워크로 배포 페이지에 접근하지 못한 뒤 사용자가 ZIP을 추가해 원본 전체 분석·독립 검산을 완료했다. 내부 README는 `19661007` record를 연결하므로 판 동일성을 추정하지 않고 실제 ZIP hash로 고정한다. [파일 분석 결과](autohack-file-profile.md)에 관측값, [C11 v1 평가](autohack-rule-ids.md)에 파일 입력 기반 초기 규칙의 결과를 연결한다.

## 3. 시나리오 대조

아래 판정은 관측 조건과 정의가 확보됐을 때의 설계 가능성이다. AutoHack 열의 ‘미확인’은 원본에 없다고 증명했다는 뜻이 아니다.

| 시나리오 | 규칙으로 탐지할 조건 | AutoHack 주 데이터 적용 범위 |
| --- | --- | --- |
| A1 Flooding | 버스·ID별 정상 기준 대비 빈도 초과 | DoS 클래스 평가 가능성 있음. 정상 목록에 없는 공격 ID는 A2와 별도로 빈도 규칙 적용 정책이 필요하며, 미등록 ID 이벤트만으로 A1 통과 처리하지 않음 |
| A2 Unauthorized ID | 실행 구성의 등록 목록 밖 ID | 학습 Normal에서 미관측인 시험 Normal 321 frame을 확인했다. OEM 전체 허용 목록이 아니므로 ‘정상 프로파일 미관측 ID’로 표현 |
| A3 Fuzzing | ID별 DLC 또는 정의된 payload reserved bit 위반 | 정상 DLC 2~8과 Fuzzing DLC 0~8을 확인했다. 정상 ID별 DLC와 대조하며 reserved 정의는 별도 필요. 정상 형식의 임의 payload 변형이 항상 걸리지는 않음 |
| A4 Out-of-range spoofing | 신호를 해석한 물리값이 허용 범위 밖 | 해당 차량·버스의 신호 배치·단위·허용 범위 필요. Spoofing 클래스 전체를 범위 초과로 간주하지 않음 |
| A5 CRC 변조 | 정의된 애플리케이션 checksum 불일치 | 차량별 계산식·위치·보호 범위 필요. SIL byte 7 CRC를 외부 차량에 적용하지 않음 |
| A6 Counter 변조 | 정의된 counter 전이 위반 | 차량별 위치·순환·허용 손실 정책 필요. SIL byte 6 규칙을 외부 차량에 적용하지 않음 |
| A7 Replay | 재생 결과가 counter·빈도·검증된 상태 일관성 조건 등을 위반 | 간격 보존 Replay에는 시간 규칙만으로 보장 불가. CRC와 counter도 인증이 아니며 정상처럼 맞춘 위조에는 한계 |
| A8 Suspension | 주기 송신이 기대되는 상태에서 3주기 이상 누락 | B-CAN 최대 관측 공백 train 945.40974초·test 324.26603초 확인. 주기·awake/sleep·수집 범위를 구분해야 하며 전용 중단 정답은 미확인 |
| B1 진단 flooding | 재조립한 UDS 요청 수가 초당 20회를 초과한 상태로 2초 이상 지속 | UDS 클래스가 flooding 정답이라는 보장 없음. CAN frame 수와 UDS 요청 수를 구분하며 ISO-TP 재조립 필요 |
| B2 세션/리셋 남용 | 민감 서비스 요청과 실제 세션·권한 위반, 반복 거부 등을 결합 | UDS 분석 대상. 정확한 SID·응답·시퀀스와 해당 ECU 정책 확인 필요. UDS-on-CAN으로 DoIP 경로를 검증했다고 주장하지 않음 |
| B3 SecurityAccess 실패 | 동일 대상에서 60초 안에 확인된 실패 5회 | 해당 실패 요청·응답 정답은 미확인. UDS_Spoofing 클래스명으로 대체 불가 |
| C1 Unknown service/method | 설정에 없는 SOME/IP service/method | CAN 데이터로 검증하지 못함. 별도 Ethernet 관측·fixture 필요 |
| C2 Malformed payload | SOME/IP 길이·버전·서비스 payload 정의 위반 | 별도 Ethernet 관측·fixture 필요 |
| C3 SD flooding | 정상 SD 동작을 넘는 과다 전송 | 별도 Ethernet 관측과 SD 빈도 정책 확정 필요 |
| D1 LIN 응답 누락 | 활성 schedule에서 연속 3회 응답 없음 | LIN Master/broker의 schedule·timeout 관측 필요. CAN 데이터로 직접 검증하지 못함 |
| D2 LIN 응답 변조 | PID parity 또는 checksum 위반 | LIN 원본 필드 또는 broker 오류 이벤트 필요. 정상 CAN 변환 결과만으로 검증하지 못함 |

A5는 SIL payload의 애플리케이션 CRC이며 물리 CAN frame CRC가 아니다. CRC와 reserved bit의 구분은 [제작 기준](../../IDS_제작_기준.md) §4를 따른다. A8과 D1은 정상 sleep·시작·종료 및 관측기 중단을 고려해야 하며 단순 패킷 유무만으로 공격 원인을 확정하지 않는다.

## 4. 기준서와 시나리오의 미해결 충돌

1. **B1 탐지 시간:** `project.md` §11.2의 2초 지속 조건은 §15.2의 일괄적인 ‘공격 시작 후 1초 안에 이벤트 기록’보다 늦게 성립한다. 사전 경보와 확정 이벤트를 구분하거나 규칙별 평가 시간을 정하는 결정이 필요하다. 이번 문서는 어느 기준도 변경하지 않는다.
2. **B3 실행 시간:** §9.4·§9.5는 세 번째 실제 key 실패 뒤 10초 지연을 둔다. 단일 ECU의 실제 실패 5회를 §11.3의 최대 10초 실행 안에서 새로 채우는 시험은 이 조건과 양립하지 않는다. 지연 중 거부 응답을 새 key 검증 실패로 조용히 바꿔 세지 않는다. 진단 시험의 시간·실패 정의를 먼저 확정해야 한다.
3. **B2 정상 세션 전환:** §9.4·§9.5는 reset·DTC clear의 SecurityAccess 권한을 요구하지만 Extended 진입 자체를 곧바로 공격으로 정하지 않는다. 인증 없는 Extended 전환 한 번만으로 공격 판정하면 정상 진단을 오탐할 수 있다. 남용 반복 기준도 추가 확정 대상이다.
4. **외부 차량 정의:** 프로젝트 SIL의 ID·주기·CRC/counter·범위를 AutoHack 현대차에 복사하지 않는다. §2.2의 RAV4 참고 방향과 AutoHack 현대차 평가도 별개다.

이 결정들은 `AGENTS.md`와 `project.md` §2.1에 따라 종속 구현 전에 사용자 확인이 필요하다. 따라서 이 문서는 판정 범위를 설명하고 충돌을 기록하며 미정 정책을 승인하거나 임시 임계값을 추가하지 않는다.

## 5. 근거와 주장 경계

- `project.md` §5·6·7·9·10.2·11.2·15: 현재 SIL 검사·관측·합격 정의의 근거. 수치는 프로젝트 선택값이다.
- [ROAD 논문](https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0296879), §2.2.3~2.2.4: 시간 특성을 바꾸는 주입과 시간 특성이 드러나지 않는 위장의 구분 근거다.
- 지정 `docs/check/`의 R155 원문(2021-03-04), Amendment 1(2022-11-25)·2(2024-03-04)·3(2025-03-12) 누적 §7.3.6·7.3.7: 보안 조치 시험·모니터링·분석 근거다. 모든 공격의 완전 탐지나 특정 숫자를 규정한 것으로 사용하지 않는다.
- 지정 R156 원문(2021-03-04) §7.2.1.1은 업데이트 진위성·무결성 범위이며 CAN 탐지 임계값 근거가 아니다.
- 지정 KISA 매뉴얼(2025-12-31), 7쪽·17쪽: 요구사항별 검증과 CAN/UDS 길이·범위·세션·민감 서비스 권한·시퀀스 시험 범위 참고다.

여섯 필수 자료의 존재·SHA-256을 다시 확인하고, 문서 판 정보는 기존 [IDS 제작 기준](../../IDS_제작_기준.md) §11과 대조했다. 데이터 원본 분석은 [AutoHack 파일 분석](autohack-file-profile.md)에 연결하며 실제 IDS 실행 결과는 후속 evidence로 구분한다.
