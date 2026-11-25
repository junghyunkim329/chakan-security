# AutoHack 원본 파일 분석

분석일: 2026-11-24 (Asia/Seoul)

상태: 사용자 제공 ZIP의 전체 파일 분석과 독립 검산 기록이다. 이 분석 단계 이후 수행한 IDS 구현·파일 평가는 [C11 규칙 IDS 근거·결과](autohack-rule-ids.md)에 연결한다. CAN 재생은 아직 수행하지 않았다. AutoHack은 주 데이터셋이며 HCRL은 비교 자료다.

## 1. 원본과 배포 판

- 입력: `IDS/docs/results/20261124_autohack_acquisition/raw/Autohack2025_Dataset.zip`
- 크기: 309,108,946 byte. ZIP 21개 항목, 그중 CSV 12개. 모든 항목의 ZIP CRC 검증 성공.
- SHA-256: `7b6112b98f775e2b66ca3aea229754ab1eb30b11020343a674f4c5723d570a51`
- MD5: `78193f5f4927d7799952d35044657dc2`. 사용자가 앞서 제공한 다운로드 화면의 MD5와 일치한다. MD5 일치는 출처의 진위성을 별도로 인증했다는 뜻이 아니다.
- 내부 README의 DOI는 `10.5281/zenodo.19661007`이며 현재 [제공기관 안내](https://cms.kookmin.ac.kr/mose/AutoHackDataset.do)는 `10.5281/zenodo.20321379`를 연결한다. 두 record의 판 동일성은 확정하지 않고 실제 제공 ZIP의 hash로 입력을 고정한다.

복사 중에는 파일 크기가 증가하고 ZIP 중앙 목록이 없었다. 복사 완료 후 위 크기·hash를 고정하고 분석했으며, 처리 중 원본 크기와 수정 시각이 바뀌지 않았음을 확인했다. 원본을 수정하거나 전체 CSV를 따로 추출하지 않았다.

## 2. 사용할 파일과 라벨 의미

버스 정보가 있는 다음 두 파일을 주 분석 표현으로 사용했다. 나머지 표현을 추가 표본으로 합산하지 않는다.

```text
Autohack2025_Dataset/Interface/train/autohack_train_both_interface.csv
Autohack2025_Dataset/Interface/test/autohack_test_both_interface.csv
```

열은 `Interface,Timestamp,Arbitration_ID,DLC,Data,Label`이다. ID는 `5E0`, `329`처럼 표기한 16진수이며 timestamp는 초 단위 소수다. 전체 timestamp는 소수점 이하 최대 5자리로 정수 microsecond로 정확히 표현할 수 있었다. payload는 공백으로 구분한 HEX byte다.

원본에는 **14개 라벨 문자열**이 있다. Normal·DoS·Fuzzing·Spoofing·Replay와 `Spoofing_UDS_*` 9종이다. 통계에서는 후자의 9종을 `UDS_Spoofing`으로 묶어 6개 범주를 표시하지만 원본 문자열은 보존했다.

모든 행에서 다음 관계를 독립적인 binary-row 검사로 확인했다.

1. `Interface`의 data와 label을 결합하면 같은 행의 both와 일치한다.
2. 버스 정보 없는 `Train/`, `Test/`의 data는 해당 `Interface` data에서 첫 열을 뺀 내용과 행 순서까지 일치한다.
3. 버스 정보 없는 표현의 label은 **UDS 9종을 `Spoofing`으로 합친 값**이다. 다른 라벨 변경은 없었다. 따라서 UDS를 별도 평가하려면 `Interface` 표현을 유지해야 한다.

탐지기에 전달할 때 Label은 분리하고 사후 평가에만 사용해야 한다. 이번 분석에서 test 라벨과 분포를 확인했으므로 이후 결과를 엄격한 blind test라고 부르지 않는다. [논문](https://www.usenix.org/system/files/vehiclesec26-song.pdf) §3.1은 반응성 메시지·jitter를 정상으로 라벨링한다고 설명한다. `Normal` 행의 집합은 공격 영향이 전혀 없는 별도 주행 기록과 같다고 간주하지 않는다.

## 3. 전체 집계와 품질

| 범주 | Train | Test | 합계 |
| --- | ---: | ---: | ---: |
| Normal | 6,593,770 | 2,557,180 | 9,150,950 |
| DoS | 77,104 | 35,629 | 112,733 |
| Fuzzing | 135,602 | 53,164 | 188,766 |
| Spoofing | 58,568 | 22,270 | 80,838 |
| Replay | 55,509 | 42,030 | 97,539 |
| UDS_Spoofing | 2,047 | 1,224 | 3,271 |
| **전체** | **6,922,600** | **2,711,497** | **9,634,097** |

총 공격 라벨 frame은 483,147개다. 버스별 frame 수와 위 집계는 내부 README 및 제공기관 통계와 일치했다. 독립 검산은 모든 CSV 표현의 행 수·내용·라벨 대응과 각 member의 hash를 대조했다.

| 버스 | Train frame | Test frame | Train의 Normal ID 수 | Test의 Normal ID 수 |
| --- | ---: | ---: | ---: | ---: |
| C-CAN | 4,026,488 | 1,678,489 | 129 | 129 |
| P-CAN | 2,655,149 | 883,323 | 97 | 96 |
| B-CAN | 240,963 | 149,685 | 80 | 77 |

- 열 수·필수 필드·HEX ID·DLC 범위·payload 해석 오류: **0건**. DLC와 실제 payload 길이 불일치: **0건**. 유효 ID는 모두 11-bit 범위다.
- DLC는 전체에서 0~8, **Normal 라벨에서는 2~8**이 관측됐다. 모든 정상 메시지를 DLC 8로 제한하면 안 된다.
- timestamp 역행: **0건**. 전체 행의 같은 시각은 train 88,371회, test 34,749회 관측됐다. 같은 시각이라는 이유로 행을 삭제하거나 라벨의 원래 대응 순서를 바꾸지 않는다.
- 전체 파일의 시간 폭은 train 1,400.81818초, test 583.15052초다. 버스별 관측 기간은 같지 않다. B-CAN의 최대 인접 관측 공백은 train 945.40974초, test 324.26603초다.
- 제공기관은 P-CAN을 주행 중, B-CAN을 주차 중 기록했다고 설명한다. 위 버스 공백은 수집 범위를 검토할 근거이며 Suspension 공격 정답으로 삼지 않는다.

## 4. IDS 설계에 직접 영향을 주는 관측

**버스와 ID를 함께 관리해야 한다.** 학습의 Normal 라벨에서 동일 ID가 둘 이상의 버스에 나타나는 경우는 39개다. 버스를 없앤 표현은 이 차이를 잃는다. ID만으로 ECU 신원도 확정할 수 없다.

**정상 관측 목록은 OEM의 완전한 허용 목록이 아니다.** 학습 Normal에 없던 ID가 시험 Normal에서 C-CAN `0x224` 320 frame, P-CAN `0x515` 1 frame, 총 **321 frame** 나타났다. 학습에 없던 정상 ID를 시험 라벨을 보고 자동 추가해 오탐을 없애지 않는다. ‘프로파일 미관측 ID’와 ‘비인가 ECU 확정’을 구분해야 한다. Fuzzing 때문에 학습 전체에는 버스마다 2,048개 ID가 있으므로 모든 학습 라벨을 허용 목록 생성에 쓰면 안 된다.

**기존 SIL 보호 형식은 적용할 수 없다.** C-CAN `0x366`의 학습 Normal 139,290 frame은 모두 DLC 7이다. 정상 같은 ID 수신 간격 중앙값은 10,000µs지만 최소 관측 간격은 400µs다. B-CAN `0x18E`의 정상 간격 중앙값은 200,010µs다. 이 값들은 관측 통계이며 OEM 주기·허용 jitter·탐지 임계값으로 승인된 수치가 아니다. 애플리케이션 checksum·counter·신호 물리 범위 정의는 이 ZIP에 제공되지 않았다.

**ID·DLC 검사만으로 Spoofing·Replay 전체를 잡을 수 없다.** 시험 Replay 42,030 frame은 모두 학습 Normal에서 관측한 해당 버스의 ID·DLC 조합을 사용했다. 시험 Spoofing 중 미관측 ID는 B-CAN의 927 frame이며, 나머지는 해당 프로파일의 ID·DLC 조합 안에 있었다. 이것은 프로파일 비교 결과이며 실행한 IDS의 탐지율이 아니다. 시간·내용·차량 상태 규칙의 효과는 별도 검증해야 한다.

Spoofing target ID는 각 버스에서 train/test가 서로 달랐다. 공격 ID만 외워 평가하면 새 target에 일반화하지 못한다.

| 버스 | Train Spoofing ID | Test Spoofing ID |
| --- | --- | --- |
| C-CAN | `366, 390, 4F4` | `490, 50C` |
| P-CAN | `171, 374, 5CF` | `080, 2B0` |
| B-CAN | `18E, 1BC, 510` | `012, 08C, 102, 103, 107, 1C2, 560` |

**UDS 라벨은 SecurityAccess 실패 정답이 아니다.** UDS 공격 라벨의 payload를 ISO-TP normal-addressing 형식의 후보로 살펴보면 single-frame SID 후보는 train `0x22` 1,190개·`0x3E` 70개, test `0x22` 725개·`0x3E` 55개였다. PCI 상위 nibble `3`인 flow-control 형식 frame도 train 787개·test 444개 포함됐다. 따라서 UDS 라벨 frame 수를 그대로 요청 수로 세면 안 된다. 이 확인은 완전한 ISO-TP 재조립·요청/응답 매칭·권한 검증이 아니며 B2·B3 탐지 합격을 뜻하지 않는다.

**전체 원속도 재생은 기존 한도와 충돌한다.** `(t−1초,t]` 전체 peak는 train 7,003 frame, test 7,070 frame이었다. 버스별 peak도 C-CAN 4,180/4,213, P-CAN 4,180/4,175, B-CAN 2,193/2,285였다(train/test 순). 파일 분석에는 송신이 없으며, 후속 재생은 1,000 frame/s 제한과 속도 변환의 영향을 별도로 다뤄야 한다.

## 5. 시간·분할 통계의 해석과 후속 범위

ID별 통계는 `(버스, ID)`로 구분했다. `id_timing.csv`와 `profile.json`은 전체·범주별 간격과 1초 최대 횟수를 담는다. 범주별 간격은 같은 범주의 직전 frame까지 계산하므로 다른 범주가 사이에 끼면 그 구간도 포함된다. 별도의 `adjacent_normal_same_id_intervals`는 같은 버스·ID의 연속 관측 두 frame이 모두 Normal일 때만 집계한다. 두 통계를 섞어 임계값을 만들지 않는다.

버스별 원래 순서에서 timestamp와 라벨을 제외한 64-frame 비중첩 block의 hash를 비교했을 때 train/test 공통 block은 0개였다. 이 검사는 정해진 block 경계에만 적용되며 모든 중복·세션 의존성의 부재를 증명하지 않는다. 독립 주행 분리는 제공기관의 설명이고, 이번 ZIP에는 이를 별도로 입증하는 logger session metadata가 없다. 기존 train/test 분리는 유지하며 재분할·재라벨링하지 않았다.

A1의 빈도 이상, A2의 프로파일 미관측 ID, A3의 ID별 DLC 이상은 제작 후보다. A4 물리 범위·A5 checksum·A6 counter는 해당 차량의 추가 정의가 필요하다. A7은 간격을 보존하는 Replay의 미탐 한계를 포함해 평가해야 하며 A8의 전용 중단 정답은 확보하지 못했다. SOME/IP·LIN 공격은 이 CAN CSV로 직접 검증하지 않는다. 시나리오별 범위는 [대조표](attack-scenario-coverage.md)를 따른다.

## 6. 결과와 재현

로컬 실행 식별자: `IDS/docs/results/20261124_autohack_profile/`. 실행별 결과·분석 스크립트와 원본은 Git에 포함하지 않는 프로젝트 정책을 따른다. 다음 명령은 이 실행 환경의 로컬 파일을 가리키며 새 checkout에 자동 포함된 실행 도구라는 뜻이 아니다.

```bash
python3 IDS/docs/results/20261124_autohack_profile/analyze.py
python3 IDS/docs/results/20261124_autohack_profile/verify.py
python3 IDS/docs/results/20261124_autohack_profile/audit.py
```

`manifest.json`은 원본·스크립트·결과 hash, 환경과 명령을 기록한다. `verification.json`은 모든 표현의 독립 대조와 ZIP CRC, `audit.json`은 두 분석의 집계·hash 일치를 기록한다. 원본 전체 분석은 약 60초, 독립 파일 대조는 약 30초에 완료했다. 이는 실행 환경의 처리 시간이며 공격 탐지 지연이 아니다.

필수 근거 자료는 [제작 기준](../../IDS_제작_기준.md) §11의 여섯 파일과 판을 다시 확인했다. R155 누적 §7.3.6·7.3.7 및 KISA 매뉴얼 7·17쪽은 시험·증거 추적의 근거이며 이 데이터의 수치나 탐지율을 지정하지 않는다. R156 §7.2.1.1은 업데이트 보호 범위로 구분한다.
