# IDS

Phase 7 (공격과 IDS, project.md §11.2·§11.3·§16) 작업 위치. 아직 구현 없음.

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
