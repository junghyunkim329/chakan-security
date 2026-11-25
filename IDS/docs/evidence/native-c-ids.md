# 독립 C IDS — 실행 구조·검증

기준일: 2026-11-25 (Asia/Seoul). 사용자가 IDS 실행 전체를 독립 C 프로그램으로 옮기고 Python을 외부 분석 도구로 분리하도록 지정했다. 기존 규칙의 출처·수치와 데이터셋 판은 [이전 근거](autohack-rule-ids.md)와 [원본 분석](autohack-file-profile.md)을 유지한다. 원본·실행 결과는 Git에 포함하지 않는다.

## 실행 책임

| 구성 | 책임 |
| --- | --- |
| `IDS/build/canids` | C11 독립 프로세스. 정책 JSON·출처·hash 검증, ZIP/CSV 입력, frame 판정, 버퍼링한 JSONL, 실행 manifest·runtime 기록 |
| `src/canids.c` | `(bus * 2048 + CAN ID)` 직접 인덱스, DLC·직전 수신·1초 window 네 규칙. 실행 파일에 object로 직접 링크 |
| `src/runtime_policy.c` | json-c 정책 검증, 정렬·ASCII escape 기준 canonical SHA-256, 원본·실행 파일·산출물 digest |
| `src/runtime_input.c` | libarchive ZIP stream, 재사용 CSV record, 정확한 microsecond·물리 line·truth-free frame fingerprint |
| `src/runtime_log.c`, `src/main.c` | 재사용 이벤트 formatter·8192-byte 버퍼, 파일 backpressure·기록 지연·실패/중단 증거 |
| `canids/profile.py`, Python `fit` | 외부 정상 프로파일 생성. 기존 JSON 정책 v1과 Normal 산출식 유지 |
| `canids/analysis.py`, Python `evaluate --run-dir` | 완료된 C 실행의 hash·frame 위치·건수와 별도 정답을 대조해 오탐·미탐 계산. 탐지 코어를 호출하지 않음 |

C 실행에는 Python·ctypes·`libcanids.so`·shell subprocess가 필요하지 않다. json-c·libarchive·OpenSSL Crypto native 공유 라이브러리는 필요하다. `engine.py`와 `c_engine.py`는 회귀 검증·기존 FFI 비교용이며 production 명령에서 제외했다. 빌드·세 단계 실행 명령은 [README](../../README.md)를 따른다.

현재는 AutoHack 파일 입력 경로다. [Linux SocketCAN 공식 API](https://docs.kernel.org/networking/can.html)는 후속 native 수집기의 근거이며, 실제 인터페이스·버스 매핑과 소유 marker 결정을 이번에 만들지 않았다. SocketCAN·대시보드·IDPS는 구현하지 않았다.

## 판정과 기록 계약

정책 v1의 네 규칙·306개 측정 프로파일·엄격한 `<`/`>`·`(t−1초,t]`·버스별 상태와 이상 frame의 상태 반영을 유지한다. C 입력은 Label을 판정에 사용하지 않는다. 이벤트 v1의 rule·bus·ID·원본 위치·timestamp·DLC·data·관측값·threshold·window·policy/experiment identity와 log-only 의미를 보존했다. 기록 측정을 위한 `received_monotonic_ns`를 추가했다.

CSV는 기존 5/6열 Interface 형식을 읽고 quoted field·CRLF·UTF-8·정수 범위를 검사한다. ZIP은 정확한 member를 스트리밍하고 선택 member의 read/CRC 오류·중복 member·EOF 실패를 거부한다. 원본 ZIP 전체를 추출하거나 제공 스크립트를 실행하지 않는다. 원본 전체·선택 member·라벨 없는 frame의 hash를 보존하며, 종료 시 입력 file identity·size·mtime와 정책 파일 hash를 확인한다.

C policy loader는 Python 정책의 완료된 학습 출처와 건수·ID/DLC·간격 지원 수·중복·hash를 검증한다. 한 번 읽은 정책 byte를 parse·digest에 함께 사용한다. literal 한글을 linked json-c strict 모드가 거부한 회귀 사례 때문에 유효 UTF-8을 동일한 ASCII JSON escape로 변환한 뒤 strict parse한다. 원본 file digest는 변환 전 byte를 사용한다.

실제 실행 파일은 `/proc/self/exe`로 hash를 고정하고 native 라이브러리 버전과 build 시 source hash·commit을 manifest에 남긴다. CMake는 소스 변경 시 build identity를 재생성한다. hash는 동일성 검사이며 서명·인증이 아니다. source 수치는 정책 생성 결과이고 ELF 해시는 OEM 승인 또는 공급망 검증을 뜻하지 않는다.

외부 분석은 C의 complete manifest, 모든 산출물 digest, 입력·frame hash·행 수, event identity·순서·중복·frame/원본 위치 및 건수를 확인한다. 실패·중단 실행이나 변조·잘못 연결한 결과에는 성공 report를 만들지 않는다. 분석은 새로운 탐지를 실행하거나 C의 이벤트를 변경하지 않는다.

## 메모리·로그·성능의 경계

수신마다 새 객체를 만들지 않고 CSV record와 이벤트 formatting/output buffer를 재사용한다. per-ID ring 초기 용량은 학습 Normal의 최대 1초 관측 횟수다. 알려진 ID의 공격 부하가 이를 넘으면 ring을 늘리고 `window_grows`, capacity와 실제 최대 ID window를 기록한다. 데이터에 없는 OEM cap을 만들거나 초과 frame을 버리지 않는다. peak capacity는 실행 종료까지 유지하며 메모리 부족은 오류다.

로그 버퍼는 이 C 환경의 `BUFSIZ=8192`를 사용한 프로젝트 구현값이며 차량의 부하 한도가 아니다. 큰 record는 별도 전체 write로 처리하고 `oversized_records`를 기록한다. 버퍼 포화 시 로컬 파일 write backpressure를 적용하며 write 실패는 실행 실패다. 화면·네트워크·여러 worker thread는 도입하지 않았다. [Suricata 공식 출력 문서](https://docs.suricata.io/en/latest/output/eve/eve-json-output.html)의 buffering·threaded output은 설계 참고이며 그 구현을 이 IDS가 재현했다는 뜻은 아니다.

`read_to_write_*`는 process가 record 읽기를 시작한 시각부터 그 이벤트를 담은 batch가 kernel write를 마친 시각까지다. `recorded_at`·`processing_monotonic_ns`는 event serialization 시각이므로 flush 완료로 해석하지 않는다. EOF·가득 찬 버퍼·종료 때 flush한다. 별도 flush deadline은 없어 sparse 이벤트는 다음 flush까지 기다릴 수 있다. disk fsync, 실제 CAN 수신, attack 시작 지연이나 hard real-time을 측정한 수치가 아니다.

## 실제 검증 결과

원래 AutoHack 시험 2,711,497 frame을 고정 정책으로 독립 C에 입력하고 별도 Python 평가에서 TP 144,840·FP 62,583·TN 2,494,597·FN 9,477을 확인했다. 기존 결과와 같고 탐지 규칙 수치는 재조정하지 않았다. 전체 event 대조와 반복 성능 측정은 로컬 `IDS/docs/results/20261125_native_ids/`에 기록한다.

두 경로에 같은 원본·고정 정책·네 규칙·전체 JSONL 이벤트를 사용하며 외부 사후 평가 시간은 실행 성능에 합치지 않는다. 기존 Python/FFI 비교 harness에도 같은 8192-byte buffering과 record-read→write 측정을 적용했다. 양쪽 모두 **254,885개 이벤트, 208,144,249 byte, 26,551회 flush**로 기록량이 같았다. 기존 FFI reference는 변경 전 실행 코드·C library를 로컬 snapshot으로 고정했다.

같은 VM에서 순서를 교대해 각각 3회 측정했다. 아래 wall time·CPU·peak RSS는 `/usr/bin/time`과 외부 monotonic timer로 측정한 **프로세스 전체** 값이며, 내부 `runtime.json`의 최종 산출물 hash 계산 전 구간과 구분한다. 입력은 이미 읽은 상태의 파일 cache이며 CPU affinity를 고정한 WCET 측정이 아니다.

| 항목 | Python→C FFI reference | 독립 C |
| --- | ---: | ---: |
| 전체 처리 시간 중앙값 | 19.111초 | 2.895초 |
| 처리량 중앙값 | 141,880 frame/s | 936,547 frame/s |
| user+system CPU 시간 중앙값 | 19.09초 | 2.88초 |
| 3회 중 최대 RSS | 31,448 KiB | 10,204 KiB |
| 3회 중 최대 process read→kernel write 지연 | 1,252.492ms | 140.571ms |
| 입력 frame / 기록 event | 2,711,497 / 254,885 | 2,711,497 / 254,885 |
| skip / event drop / write failure | 0 / 0 / 0 | 0 / 0 / 0 |

이 조건에서 처리량 약 6.60배, CPU 시간 약 84.9% 감소, 관측 최대 RSS 약 67.6% 감소였다. parser·FFI·event formatting·기록을 포함한 실행 전체의 비교이며 C 코어 하나만의 효과로 분리하지 않는다. 실제 장치 I/O나 live CAN에 같은 개선율을 보장하지 않는다.

Native 실행에서 ring의 초기 capacity 합계는 8,114 slot, 부하 후 16,486 slot(131,888 byte)이었고 237회 확장했다. 최대 단일 ID active window는 298 frame이었다. 입력 record buffer는 재사용하며 이 부하에서 메시지마다 메모리를 할당하지 않았다. 이 측정값을 미래 공격 부하의 고정 상한으로 만들지 않았다.

독립 audit에서 6회 실행의 **모든 254,885개 event 관측값**이 이전 기준 구현과 일치했고, C build source·실행 파일·산출물 hash도 일치했다. 고정 policy hash는 `79454d080974df903c5c0e92c929e4fa7a6faa638053e15b7a2328601b00896d`, 측정 실행 파일 SHA-256은 `df5225d9e0b39adda6a2f35bec3b8de98f40868056aeaad89a8e2d89c5e82950`이다.

최종 테스트는 **77개 통과**, native CTest 1개 통과다. 독립 검토의 non-finite 정책 값과 유효 DEL 문자의 정책 hash 호환성을 실패 테스트로 재현해 같은 검증 수정 단계에서 해결했다. NaN·Infinity 거부와 정상·DEL 정책 실행 및 C core는 ASan·UBSan·LeakSanitizer 검사를 통과했다. LeakSanitizer는 ptrace 제약 때문에 승인된 sandbox 외부 실행으로 검사했다. 미뤄둔 검토 항목은 없다.

로컬 결과는 `IDS/docs/results/20261125_native_ids/`의 `benchmark-final.json`, `native-final-1/`~`native-final-3/`, `ffi-final-1/`~`ffi-final-3/`, `analysis-verified/`, `audit.json`, `sanitizers.json` 및 실행 스크립트에 보관했다. 이 경로는 새 checkout에 포함된 파일 링크가 아니라 실행 식별자다. 원본·binary·실행별 결과와 스크립트는 로컬 전용 보관 정책을 따른다.

파일 입력의 모든 frame 수와 이벤트가 일치하고 skip/write failure가 없다는 것은 파일 처리 검증이다. SocketCAN kernel 수신 유실은 `null`로 남기며 ‘실제 CAN frame 유실 0’으로 표현하지 않는다. 기존 1초 SIL 합격, 모든 공격 탐지 또는 양산·실차 실시간성을 주장하지 않는다. R155 누적 §7.3.6·7.3.7과 지정 KISA 매뉴얼 7·17쪽은 시험·증거 추적 근거이고, 이 프로그램의 버퍼 수치나 CPU/RSS 목표를 정한 규정은 아니다.
