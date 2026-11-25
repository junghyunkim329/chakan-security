# IDS

Phase 7 작업 위치. **독립 C11 실행 프로그램 + 외부 Python 분석 도구**로 구성한 파일 기반 IDS를 구현했다. AutoHack이 주 데이터셋이다.

CAN A1~A8의 탐지 조건, 기대 이벤트, 시험·기록 기준과 구현 전 미정 항목은 [CAN IDS 제작 기준](IDS_제작_기준.md)에 정리한다.

대시보드 연동과 향후 IDPS 확장 방향은 제작 기준 §6.1~6.2에 포함한다. C가 정책 검증·ZIP/CSV 입력·네 가지 프로파일 이상 규칙·이벤트 기록을 직접 수행한다. Python은 정상 프로파일 생성과 완료된 C 실행의 사후 평가를 담당한다. native SocketCAN 수집·대시보드·예방 기능은 후속 범위다.

CAN 데이터셋 실험의 적용 기준은 제작 기준 §3.3에 둔다. **AutoHack을 제작·평가의 주 데이터셋으로 사용하고, HCRL은 먼저 분석한 비교 자료로 사용한다.** 파일 분석으로 IDS를 검증한 뒤 가상 CAN 재생으로 연동을 확인한다. ROAD는 한계 분석 근거로 사용한다. 외부 차량 데이터에 기존 SIL의 ID·CRC·counter 정의를 자동 적용하지 않는다.

2026-11-24 사용자 확정: 실제 데이터·확인된 차량 동작과 기존 프로젝트 가정이 충돌하면 **실제 데이터를 우선하고 `project.md`를 갱신한다**(§2.1.1). 정상 데이터에서 규칙의 근거를 산출하고 정의가 없는 물리 범위·checksum·counter는 추정하지 않는다.

[공격 시나리오별 탐지 범위](docs/evidence/attack-scenario-coverage.md): `project.md`와 AutoHack 논문을 기준으로 A1~D2의 규칙화 가능 조건, 데이터 적용 한계와 미정 사항을 구분한다.

[독립 C 실행 구조·성능 검증](docs/evidence/native-c-ids.md): Python 없는 실행 경로, 입력·로그 계약, 메모리 재사용, 기존 FFI 방식과 비교 방법 및 한계를 정리한다.

[C11 규칙 IDS 구현·근거·실제 평가](docs/evidence/autohack-rule-ids.md): 이전 Python/FFI 실행 단계의 306개 정상 프로파일, 네 규칙의 출처·산출식과 전체 AutoHack 시험 결과를 정리한다. 경보율·오탐·미탐과 미구현 시나리오를 함께 기록한다.

[AutoHack 원본 파일 분석 결과](docs/evidence/autohack-file-profile.md): 9,634,097 frame과 모든 CSV 표현의 대응·ZIP CRC를 검증했다. `Interface` 표현의 UDS 세부 라벨, 정상 DLC 2~8, 학습 정상 목록에 없는 시험 정상 321 frame, 버스별 수집 공백과 재생률 제한을 정리했다. 후속 정책 산출과 IDS 파일 평가는 위 C11 문서에 연결한다. CAN 재생은 아직 수행하지 않았다.

[HCRL 원본 파일 분석 결과](docs/evidence/hcrl-file-profile.md): 유효 frame 17,558,346개를 집계하고 공통 정상 구간 891,068행, 긴 기록 공백과 불완전한 정상 로그 1행을 확인했다. IDS 성능 시험·CAN 재생은 아직 수행하지 않았다.

[HCRL 룰 설계 근거와 임계값 검토](docs/evidence/rule-rationale.md): 비교용 HCRL의 기존 제안과 실차 연구·ITU 지침을 정리한다. 현재 AutoHack C11 구현과 평가의 근거는 위 C11 문서를 따른다.

## 사용 데이터셋·저작권·라이선스

공식 출처와 이용 조건 확인일: **2026-11-25 (Asia/Seoul)**. 외부 데이터의 저작권은 원저작자에게 있으며, 이 저장소의 코드 라이선스로 데이터의 이용 조건을 대체하지 않는다.

| 데이터셋 | 이 프로젝트에서 사용한 범위 | 공식 출처·이용 조건 |
| --- | --- | --- |
| **AutoHack** — 주 데이터셋 | 사용자 제공 `Autohack2025_Dataset.zip`의 C-CAN·P-CAN·B-CAN 자료. 학습 6,922,600 frame 중 Normal로 정책을 생성하고 시험 2,711,497 frame으로 IDS를 평가 | [국민대학교 MOSE 공식 배포 안내](https://cms.kookmin.ac.kr/mose/AutoHackDataset.do), [배포 DOI: 10.5281/zenodo.20321379](https://doi.org/10.5281/zenodo.20321379). **공식 배포 안내에 CC BY 4.0 명시** |
| **HCRL Car-Hacking** — 비교 자료 | DoS·Fuzzy·RPM/gear Spoofing CSV와 정상 주행 로그의 파일·품질 분석. 현재 C11 IDS의 정책 생성·성능 평가에는 사용하지 않음 | [HCRL 공식 배포 안내](https://sites.google.com/hksecurity.net/hcrl/Datasets/car-hacking-dataset). **학술 목적 공개·논문 인용 요청**. 확인한 공식 페이지에는 별도 표준 라이선스가 명시돼 있지 않아 CC BY 등으로 임의 표기하지 않음 |

AutoHack의 [CC BY 4.0 조건](https://creativecommons.org/licenses/by/4.0/)에 따라 제작자·출처·라이선스 링크와 처리 내역을 표시한다. 원본 ZIP·CSV와 원본 라벨은 수정하지 않았으며, 정상 프로파일·통계·IDS 평가 결과를 별도로 산출했다. UDS 세부 라벨은 원문을 보존하고 통계 표시에서만 하나의 범주로 묶었다. 이 프로젝트를 데이터 제공기관이 보증하거나 승인했다는 뜻은 아니다.

실제 사용한 AutoHack ZIP의 내부 README는 DOI `10.5281/zenodo.19661007`을 연결한다. 위 현재 배포 DOI와 같은 판이라고 단정하지 않으며, 사용 파일은 SHA-256과 내부 파일명으로 고정했다. 판 식별과 전체 처리 내역은 [AutoHack 원본 분석](docs/evidence/autohack-file-profile.md), HCRL 입력 식별은 [HCRL 원본 분석](docs/evidence/hcrl-file-profile.md)을 따른다.

### 제작자·인용 문헌

- **AutoHack**: Yuchan Song, Sejun Ahn, Seungjin Baek, Hyeonseong Kim, Huy Kang Kim, Sanghoon Jeon. [AutoHack 논문·서지 정보](https://www.usenix.org/conference/vehiclesec26/presentation/song), VehicleSec 2026, pp. 237–252. 공식 배포 안내는 국민대학교 Mobility Cybersecurity Lab(MOSE)에서 제공한다.
- **HCRL Car-Hacking**: [제공기관이 지정한 인용 문헌](https://sites.google.com/hksecurity.net/hcrl/Datasets/car-hacking-dataset)은 Song, Woo & Kim (2020), *Vehicular Communications* 21, 100198 및 Seo, Song & Kim (2018), *GIDS*, PST 2018이다.

ROAD는 [논문](https://doi.org/10.1371/journal.pone.0296879)을 통한 탐지 한계 검토에만 참고했다. ROAD 원본 데이터는 이번 분석·정책 생성·평가에 사용하지 않았다.

### 원본 데이터는 GitHub에 포함하지 않음

사용한 데이터셋 ZIP·CSV·정상 주행 로그와 실행별 정책·이벤트·결과는 로컬 `IDS/docs/results/`에만 보관하고 `.gitignore`로 제외한다. 저장소에는 구현·테스트·재현 명령과 선별한 분석 근거를 남긴다. 원본은 위 공식 출처에서 별도로 확보해야 한다. 이 보관 방침은 원저작자의 라이선스 조건을 변경하지 않는다.

## 빌드·실행

저장소 루트에서 실행한다. C 실행 프로그램은 Python이나 `libcanids.so`를 호출하지 않고 탐지 코어를 직접 링크한다. native 의존성은 json-c·libarchive·OpenSSL Crypto다. Python 3 표준 라이브러리는 외부 프로파일 생성·평가에만 필요하며 pytest는 개발 테스트에 사용한다.

```bash
sudo apt-get install -y build-essential cmake pkg-config libjson-c-dev libarchive-dev libssl-dev
cmake -S IDS -B IDS/build -DCMAKE_BUILD_TYPE=Release
cmake --build IDS/build
ctest --test-dir IDS/build --output-on-failure
python3 -m pytest -q IDS/tests
```

원본 ZIP을 별도로 준비하고 `/path/to/Autohack2025_Dataset.zip`을 실제 경로로 바꾼다. policy와 결과 경로는 새 경로여야 한다. 아래 세 단계는 별도 프로세스이며, 이미 고정 정책이 있으면 C 탐지 실행에 Python이 필요하지 않다.

**1. 외부 Python 도구로 학습 Normal 기준 정책 생성**

```bash
python3 -m IDS.canids fit \
  --input /path/to/Autohack2025_Dataset.zip \
  --member Autohack2025_Dataset/Interface/train/autohack_train_both_interface.csv \
  --output IDS/docs/results/demo/policy.json
```

**2. 독립 C 프로그램으로 탐지**

```bash
./IDS/build/canids \
  --input /path/to/Autohack2025_Dataset.zip \
  --member Autohack2025_Dataset/Interface/test/autohack_test_both_interface.csv \
  --policy IDS/docs/results/demo/policy.json \
  --output-dir IDS/docs/results/demo/native
```

**3. 외부 Python 도구로 C 이벤트와 정답을 비교**

```bash
python3 -m IDS.canids evaluate \
  --run-dir IDS/docs/results/demo/native \
  --input /path/to/Autohack2025_Dataset.zip \
  --member Autohack2025_Dataset/Interface/test/autohack_test_both_interface.csv \
  --policy IDS/docs/results/demo/policy.json \
  --output-dir IDS/docs/results/demo/analysis
```

C 입력은 `Interface,Timestamp,Arbitration_ID,DLC,Data` 또는 마지막에 `Label`이 있는 CSV다. ZIP에는 정확한 `--member`를 지정한다. C는 Label을 판정에 사용하지 않는다. 외부 `fit`·`evaluate`는 Label이 함께 있는 `both_interface.csv`를 사용한다.

| 결과 폴더 | 산출물·확인 항목 |
| --- | --- |
| C `native/` | `manifest.json`, `runtime.json`, `ids-events.jsonl`, `summary.md`. frame/event 수, CPU·RSS·처리량, window 저장 공간 증가, 로그 기록 지연·write 실패·pending 건수 |
| Python `analysis/` | 별도 `manifest.json`, `report.json`, `summary.md`. 완료된 C 산출물과 원본 hash·event 위치를 검증한 후 frame별 오탐·미탐 계산 |

`manifest.status=complete`인 결과만 사용한다. 실행 폴더 생성 뒤의 입력 오류·중단·기록 실패는 실패/중단 manifest와 부분 기록을 남긴다. 초기 정책 검증이나 기존 출력 경로 때문에 시작을 거부하면 stderr와 실패 종료 코드로 알린다. 이벤트의 `warning`·`log_only`는 이상 관측 상태이며 차단 명령이 아니다. Python의 기존 `detect`, `--backend`, `--library` 실행 옵션은 제거했다. `engine.py`와 `c_engine.py`는 테스트·이전 방식 비교용으로만 남긴다.

## C API와 메모리·로그 경계

[canids.h](include/canids.h)의 `canids_create` → `canids_observe` → `canids_destroy`를 C 실행 파일이 직접 사용한다. 프로파일은 복사되며 frame에 라벨을 넣지 않는다. bus는 C/P/B 순서의 0/1/2이고 timestamp는 공통 시계의 비감소 int64 microsecond다. 버스·11-bit ID로 상태에 바로 접근한다.

입력·이벤트 버퍼와 per-ID window 저장 공간을 재사용한다. window 초기 용량은 학습 Normal 최대 1초 관측 횟수이며, 공격 부하가 넘으면 공간 증가와 peak를 기록한다. 부족한 메모리나 파일 쓰기 오류를 조용한 유실로 처리하지 않는다. 고정 메모리 또는 hard real-time 보장은 아니다.

단일 스레드에서 로컬 파일로 버퍼를 비우며 화면·네트워크 전송은 실행 경로에 없다. buffer가 차면 파일 backpressure가 생긴다. 기록 지연은 process의 record 읽기 시작부터 batch의 kernel write 완료까지이며, 디스크 영구 저장·실제 SocketCAN 수신·공격 시작 후 지연과 구분한다. 현재 파일 경로에는 주기적 flush deadline이 없으며 SocketCAN 연동 전에 별도 검증이 필요하다.

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
