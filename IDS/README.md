# IDS

Phase 7 작업 위치. **C11 탐지 코어 + Python 데이터 분석·정책 생성·평가**로 구성한 파일 기반 IDS v1을 구현했다. AutoHack이 주 데이터셋이다.

CAN A1~A8의 탐지 조건, 기대 이벤트, 시험·기록 기준과 구현 전 미정 항목은 [CAN IDS 제작 기준](IDS_제작_기준.md)에 정리한다.

대시보드 연동과 향후 IDPS 확장 방향은 제작 기준 §6.1~6.2에 포함한다. 현재 구현은 네 가지 프로파일 이상 규칙과 이벤트·평가 도구다. native SocketCAN 수집·대시보드·예방 기능은 후속 범위다.

CAN 데이터셋 실험의 적용 기준은 제작 기준 §3.3에 둔다. **AutoHack을 제작·평가의 주 데이터셋으로 사용하고, HCRL은 먼저 분석한 비교 자료로 사용한다.** 파일 분석으로 IDS를 검증한 뒤 가상 CAN 재생으로 연동을 확인한다. ROAD는 한계 분석 근거로 사용한다. 외부 차량 데이터에 기존 SIL의 ID·CRC·counter 정의를 자동 적용하지 않는다.

2026-11-24 사용자 확정: 실제 데이터·확인된 차량 동작과 기존 프로젝트 가정이 충돌하면 **실제 데이터를 우선하고 `project.md`를 갱신한다**(§2.1.1). 정상 데이터에서 규칙의 근거를 산출하고 정의가 없는 물리 범위·checksum·counter는 추정하지 않는다.

[공격 시나리오별 탐지 범위](docs/evidence/attack-scenario-coverage.md): `project.md`와 AutoHack 논문을 기준으로 A1~D2의 규칙화 가능 조건, 데이터 적용 한계와 미정 사항을 구분한다.

[C11 규칙 IDS 구현·근거·실제 평가](docs/evidence/autohack-rule-ids.md): 306개 정상 프로파일, 네 규칙의 출처·산출식, 전체 AutoHack 시험 결과, C/Python 이벤트 전수 일치와 검증 범위를 정리한다. 경보율·오탐·미탐과 미구현 시나리오를 함께 기록한다.

[AutoHack 원본 파일 분석 결과](docs/evidence/autohack-file-profile.md): 9,634,097 frame과 모든 CSV 표현의 대응·ZIP CRC를 검증했다. `Interface` 표현의 UDS 세부 라벨, 정상 DLC 2~8, 학습 정상 목록에 없는 시험 정상 321 frame, 버스별 수집 공백과 재생률 제한을 정리했다. 후속 정책 산출과 IDS 파일 평가는 위 C11 문서에 연결한다. CAN 재생은 아직 수행하지 않았다.

[HCRL 원본 파일 분석 결과](docs/evidence/hcrl-file-profile.md): 유효 frame 17,558,346개를 집계하고 공통 정상 구간 891,068행, 긴 기록 공백과 불완전한 정상 로그 1행을 확인했다. IDS 성능 시험·CAN 재생은 아직 수행하지 않았다.

[HCRL 룰 설계 근거와 임계값 검토](docs/evidence/rule-rationale.md): 비교용 HCRL의 기존 제안과 실차 연구·ITU 지침을 정리한다. 현재 AutoHack C11 구현과 평가의 근거는 위 C11 문서를 따른다.

## 빌드·실행

저장소 루트에서 실행한다. C11 compiler·CMake·Python 3이 필요하며 Python 실행부는 표준 라이브러리만 사용한다. 테스트에는 pytest를 사용한다.

```bash
cmake -S IDS -B IDS/build -DCMAKE_BUILD_TYPE=Release
cmake --build IDS/build
ctest --test-dir IDS/build --output-on-failure
python3 -m pytest -q IDS/tests
```

원본 ZIP을 준비한 뒤 다음 `/path/to/Autohack2025_Dataset.zip`을 실제 경로로 바꾼다. 원본·실행별 policy/result와 build는 Git에 포함되지 않는다. output은 새 경로를 사용해야 한다.

```bash
python3 -m IDS.canids fit \
  --input /path/to/Autohack2025_Dataset.zip \
  --member Autohack2025_Dataset/Interface/train/autohack_train_both_interface.csv \
  --output IDS/docs/results/demo/policy.json

python3 -m IDS.canids evaluate \
  --input /path/to/Autohack2025_Dataset.zip \
  --member Autohack2025_Dataset/Interface/test/autohack_test_both_interface.csv \
  --policy IDS/docs/results/demo/policy.json \
  --output-dir IDS/docs/results/demo/test
```

`evaluate`는 기본적으로 C11 코어를 사용한다. `--backend python`은 대조용 기준 구현이며, `--library`로 별도 C library를 지정할 수 있다. 라벨 없는 `Interface,Timestamp,Arbitration_ID,DLC,Data` CSV는 `detect --input PATH --policy POLICY --output-dir NEW_DIRECTORY`로 처리한다. ZIP에는 `--member`가 필요하다. fit/evaluate는 Label이 함께 있는 `both_interface.csv`를 사용한다.

결과 폴더에는 `manifest.json`, `report.json`, `ids-events.jsonl`, `summary.md`가 생성된다. `manifest.status=complete`인 실행을 평가 결과로 사용한다. 이벤트에는 공격 정답을 넣지 않고 rule·원본 위치·관측값·threshold·policy·experiment ID를 기록한다. `warning`·`log_only`는 이상 관측 상태이며 차단 명령이 아니다.

## C API

[canids.h](include/canids.h)의 `canids_create` → `canids_observe` → `canids_destroy`를 사용한다. 프로파일은 복사되며 frame에 라벨을 넣지 않는다. bus는 C/P/B 순서의 0/1/2이고 timestamp는 공통 시계의 비감소 int64 microsecond다. 출력 배열은 최대 네 finding을 수용해야 하며 오류는 반환값으로 처리한다. native 수집기 연결 전까지 파일 검증에는 Python adapter를 사용한다.

## 쓸 수 있는 공통 자원 (프로젝트 루트)

| 위치 | 내용 |
| --- | --- |
| `config/signals/can.yaml` | CAN PDU 정의: ID, 주기, 신호 범위, 예약 비트 |
| `config/lin/network.yaml` | LIN frame·스케줄·timeout |
| `config/diagnostics/uds.yaml` | 진단 주소·CAN ID·SecurityAccess 규칙 |
| `config/services/*.yaml` | SOME/IP 서비스·payload 정의 |
| `tools/sil_can/` | 위 정의를 읽는 Python 모듈과 기준 codec (`codec.py`: CRC·counter·범위 검사, `lin.py`: parity·checksum) |
| `practice/` | 연습용 네트워크 방(`hj_*`) 구성 |

ECU·Gateway 쪽 구현과 시험은 `ECU/`에 있다.
