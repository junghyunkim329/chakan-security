# AutoHack C11 규칙 IDS v1 — 구현·근거·검증

기준일: 2026-11-24 (Asia/Seoul)

이 문서는 2026-11-24 Python/FFI 실행 단계의 기록이다. 현재 독립 C 프로그램과 외부 분석 구조는 [후속 구현](native-c-ids.md)을 따른다. 아래 규칙 수치와 기존 평가 결과는 비교 근거로 보존한다.

당시 상태: C11 탐지 코어, Python 분석·정책 생성·파일 평가·JSONL 기록 도구를 구현했다. 원본 시험 파일 2,711,497 frame과 254,885개 이벤트를 Python 기준 구현과 전수 대조했다. SocketCAN 실시간 수집·대시보드·IDPS 차단과 전체 SIL 공격 시나리오 완료를 뜻하지 않는다.

## 1. 구성과 실행 경계

| 구현 | 책임 |
| --- | --- |
| `IDS/include/canids.h`, `IDS/src/canids.c` | C11 ABI v1. 버스·ID별 DLC, 이전 수신 시각, 1초 window와 네 규칙의 관측 결과 |
| `IDS/canids/c_engine.py` | 검증된 JSON 정책을 C 구조체로 전달하고 반환값을 공통 이벤트 근거로 변환 |
| `IDS/canids/source.py`, `profile.py` | ZIP/CSV 스트리밍, 형식·시간·출처 검증, 학습 Normal 기준 산출·정책 고정 |
| `IDS/canids/evaluate.py`, `__main__.py` | frame별 평가, 원본 위치와 정책을 연결한 JSONL·manifest·요약 기록 |
| `IDS/canids/engine.py` | C 구현 판정을 대조하기 위한 Python 기준 구현 |

사용자가 C11 코어 + Python 분석·평가 구성을 확정했다. `evaluate`와 `detect`의 기본 backend는 C다. Python 분석·평가 도구가 파일을 읽어 C API에 frame을 전달하며, C 코어는 라벨·파일명·공격 payload 사전·송신 권한을 받지 않는다. C API의 data byte는 현재 신호로 해석하지 않는다.

빌드와 명령은 [IDS README](../../README.md)에 있다. root CMake/ECU 실행 경로는 변경하지 않았다. C 코어는 3개 버스와 11-bit ID를 구분하며 단일 stream의 비감소 `int64` microsecond 시각을 사용한다. 같은 시각은 허용하고 역행·형식 오류는 실패 처리한다. 여러 stream/thread는 별도 detector 또는 호출자 동기화가 필요하다.

## 2. 네 가지 규칙과 선택 근거

기법을 고른 근거와 수치를 산출한 근거를 구분한다. 정적 ID·DLC와 시간·빈도 검사는 [ITU-T X.1375 (10/2020)](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-X.1375-202010-I%21%21PDF-E&lang=e&type=items) §8.2.3.4·8.3.1·8.3.3의 방향을 참고한다. CAN 수신 간격 특성은 [Song et al. (ICOIN 2016)의 저자 소속기관 초록](https://pure.korea.ac.kr/en/publications/intrusion-detection-system-based-on-the-analysis-of-time-interval/)에서 실차 주입 공격 연구를 확인했다. 전문 알고리즘·논문의 임계값을 재현한 구현으로 표현하지 않는다.

| Rule ID | 판정 | 실제 자료를 적용한 이유와 한계 |
| --- | --- | --- |
| `CAN_ID_UNSEEN` | 해당 버스의 학습 Normal ID 집합에 없음 | CAN ID 검사의 적용. 학습에 없던 시험 정상 321 frame도 있으므로 비인가 ECU 신원 확정이 아니라 프로파일 coverage 경보 |
| `CAN_DLC_UNSEEN` | 알려진 ID의 Normal 관측 DLC 집합 밖 | 정상 DLC 2~8과 C-CAN `0x366` DLC 7을 반영. 일괄 DLC 8이나 SIL reserved 규칙을 사용하지 않음 |
| `CAN_INTERVAL_SHORT` | 같은 버스·ID의 직전 관측과 간격이 정상 최소 간격 미만 | 간격 특성을 적용하되 최소값은 학습 자료에서 산출. 정상 jitter/outlier 때문에 수치가 낮아져 미탐할 수 있고, 주입 다음 정상 frame에도 경보가 생길 수 있음 |
| `CAN_RATE_HIGH` | `(t−1초,t]` 전체 관측 건수가 Normal 관측 최대 건수 초과 | rate 검사 적용. 기존 SIL의 3배 배수를 가져오지 않음. 정상 응답 변화와 공격 인접 구간에서 오탐이 발생할 수 있음 |

최소/최대 관측 경계를 사용한 방식은 **첫 비교 기준으로 선택한 프로젝트 실험 정책**이다. 논문·규정이 AutoHack 차량의 이 숫자를 지정한 것이 아니다. 공격이나 시험 성능에 맞춰 수치를 조정하지 않았다. Normal 라벨에도 공격에 반응한 차량 메시지가 포함될 수 있어 무공격 주행 기준선이라고 간주하지 않는다.

학습은 원래 train의 Normal 6,593,770 frame만 ID/DLC·rate 경계에 사용한다. 간격 경계는 같은 버스·ID의 연속 관측 두 frame이 모두 Normal일 때만 포함하며, 중간에 그 ID의 주입 frame이 있으면 해당 pair를 제외한다. 최초 frame이나 유효 pair가 없으면 간격 규칙만 보류한다. 탐지 중에는 이상으로 표시된 frame도 관측 상태에 반영하며 정책을 자동 확장하지 않는다. 미관측 ID에는 가짜 주기·rate 기준을 만들지 않는다.

306개 `(버스,ID)` 프로파일의 DLC 집합·Normal 건수·간격 pair 수·최소 간격·최대 rate가 이전 [독립 원본 분석](autohack-file-profile.md)의 통계와 모두 일치했다. 예를 들어 C-CAN `0x366`은 DLC 7, 최소 간격 400µs, 최대 1초 102 frame이다. 이 수치는 관측 경계이며 OEM 주기·허용 jitter가 아니다.

## 3. 실제 AutoHack 시험 결과

원래 train/test 분리와 원본 라벨을 유지했다. 시험 라벨은 앞선 EDA에서 이미 확인했으므로 엄격한 blind holdout 성능이라고 부르지 않는다. 평가 단위는 주입 여부를 나타내는 **개별 frame**이며 공격 episode를 임의로 만들지 않았다.

| 항목 | 결과 |
| --- | ---: |
| 시험 frame | 2,711,497 |
| TP / FN | 144,840 / 9,477 |
| FP / TN | 62,583 / 2,494,597 |
| Precision | 69.83% |
| Recall | 93.86% |
| F1 | 0.8008 |
| 정상 frame 경보율 | 2.4473% |
| 경보가 연결된 frame / 개별 rule event | 207,423 / 254,885 |

한 frame에 여러 규칙이 발생할 수 있다. 합집합 평가는 frame을 한 번만 세고, rule별 평가는 별도로 보고한다. 다음 표는 실제 공격 종류를 분류해 맞혔다는 결과가 아니라, 해당 정답 범주에서 하나 이상의 이상 경보가 연결된 비율이다.

| 정답 범주 | Frame | 경보 frame | 경보율 |
| --- | ---: | ---: | ---: |
| Normal | 2,557,180 | 62,583 | 2.45% |
| DoS | 35,629 | 35,629 | 100.00% |
| Fuzzing | 53,164 | 52,984 | 99.66% |
| Spoofing | 22,270 | 22,087 | 99.18% |
| Replay | 42,030 | 33,967 | 80.82% |
| UDS_Spoofing | 1,224 | 173 | 14.13% |

DoS의 `0x000`은 학습 Normal에서 미관측이어서 ID 규칙으로 경보가 발생했다. 이 결과를 A1 빈도 규칙의 독립 통과로 바꾸지 않는다. UDS는 별도 세션·서비스·권한 분석을 구현하지 않아 1,051 frame을 놓쳤다. Replay 결과도 이 자료의 관측 이상에 대한 결과이며 모든 간격 보존 Replay를 탐지한다는 주장이 아니다.

ID 규칙의 FP는 321, DLC 규칙의 FP는 0, 간격 규칙의 FP는 16,280, rate 규칙의 FP는 60,738이었다. 서로 겹치므로 이 건수를 합쳐 전체 FP로 사용하지 않는다. 현재 정책의 오탐은 후속 정상 상태·수집 범위 검토 대상이며 자동 차단을 활성화하지 않는다.

## 4. 성능 측정의 구분

동일한 2,000,000개 사전 생성 frame을 넣는 단일 합성 코어 loop에서 C11 native API는 0.02467초, Python 기준 코어는 0.97836초였다. 이 시험은 정상 ID 하나, 1초 간격, 경보 없음 조건이며 입력 생성·파싱·FFI·로그·metrics를 제외한다. CPU 부하 조건과 입력이 달라지면 달라지므로 WCET·실차 deadline·실시간성 보장으로 표현하지 않는다.

실제 CSV/ZIP→Python→C FFI→JSONL·metrics 경로는 별개다. 최종 C 경로의 2,711,497 frame 처리는 약 21.62초, 앞선 Python 기준 경로는 약 18.18초였다. 이 단일 관측에서 파일 도구 전체가 C backend로 더 빨라진 결과는 아니다. 파싱·FFI·기록을 포함한 시간을 C 코어만의 성능으로 해석하지 않는다. native SocketCAN 수집 경로 성능은 아직 측정하지 않았다.

## 5. 계약·검증과 남은 범위

정책 버전 1은 원본 ZIP/CSV·member·라벨을 제외한 frame fingerprint, Normal 건수, 수치와 산출 방식, producer 코드 hash를 보유한다. 평가 manifest는 실제 실행 코드·C shared library의 hash, command, Python/kernel, CPU 가용 수와 처리 상태를 기록한다. 같은 전체 frame을 다시 평가하면 라벨을 바꿔도 `calibration_reuse`로 표시한다. 부분 중복의 부재를 증명하지는 않는다.

C library는 고유한 private read-only snapshot으로 로드해 실제 실행 byte와 hash를 연결한다. 같은 경로의 library를 교체해도 이전 `dlopen` handle을 새 hash로 기록하지 않는다. SHA-256은 artifact 동일성 검사이며 비밀키 인증·서명 검증이 아니다.

이벤트 버전 1은 원본 위치·상대 timestamp·관측값·threshold/window·policy hash·experiment ID와 `warning`/`log_only`를 기록한다. 위험 등급이나 차단 명령이 아니다. 입력 오류·timestamp 역행·중단은 실패/interrupted manifest와 부분 기록을 남기며 성공한 평가로 합산하지 않는다. 출력 경로는 덮어쓰지 않는다.

검증: Python/adapter/CLI 테스트 51개, native CTest 1개 통과. C ring 성장·wrap·expiry와 오류 시 상태, ABI·입력 경계, strict CSV, 정책 출처·교체, 라벨 분리, 중단·overwrite를 확인했다. ASan·UBSan·LeakSanitizer native 검사를 통과했다. LeakSanitizer의 ptrace 제약은 승인된 외부 실행으로 확인했다. 독립 검토의 Python 입력·출처 검증 2건과 C library hash 연결 1건, 자체 검토의 보고서 조기 발행 문제는 재현 테스트 RED→GREEN으로 수정했다.

전체 시험 frame에서 C와 Python의 metrics가 같고 **모든 254,885개 이벤트의 rule·원본 위치·관측값·threshold·window·payload가 일치**했다. C 원본·policy·library와 결과 hash도 대조했다. 실행 결과는 로컬 `IDS/docs/results/20261124_autohack_ids_v1_verified/`의 `policy.json`, `test/`, `c-final/`, `audit.json`, `core-benchmark.json`에 있다. 이전 실험도 보존했다. 원본·산출물·분석 스크립트와 build는 Git에 포함하지 않는다.

물리 신호 범위·checksum·counter·Suspension, 진단 권한 상태, SOME/IP·LIN, native SocketCAN 수집, SQLite/DLT·대시보드 및 IDPS는 후속 범위다. C window는 active timestamp를 저장하는 growable heap ring이며 peak capacity를 유지한다. 메모리 부족은 오류로 반환하고 유실을 숨기지 않는다. hard real-time·양산 적합성은 검증하지 않았다. 지정 R155 누적 §7.3.6·7.3.7과 KISA 매뉴얼 7·17쪽은 시험·분석 근거 추적의 취지로 연결하고, R156은 업데이트 보호 범위로 구분한다.
