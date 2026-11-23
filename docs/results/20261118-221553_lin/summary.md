# Phase 6 LIN 차체 도메인 시나리오 시험 (연습 구성 hj_*)

- 결과: **FAIL**
- 기준 6/38 통과

## 명령·응답 (0/19)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| AP가 BodyControl 서비스 발견 | ❌ | 10030 ms |
| 초기 상태: 문 닫힘·중앙 잠금, 창문 0, 실내등 꺼짐, 모두 fresh | ❌ | None |
| SetWindowPosition(50) → accepted(0) | ❌ | None |
| 창문이 5%씩 움직여 50 도달 (50 ms마다 5% → 약 0.5 s) | ❌ | None s |
| event에서 창문 위치가 증가만 함 (100 ms event 사이 최대 10%) | ❌ | [] |
| SetInteriorLight(1) → accepted(0), 실내등 켜짐 | ❌ | None, None s |
| Door ECU 입력(앞좌 문 열림)이 event에 반영 | ❌ | None s |
| 잘못된 인자 (범위 밖 101) → invalid argument(1) | ❌ | None |
| 잘못된 인자 (payload 2 byte) → invalid argument(1) | ❌ | None |
| 잘못된 인자 (실내등 2) → invalid argument(1) | ❌ | None |
| 장애물 주입: 창문 정지·obstruction·fault 표시 | ❌ | None |
| 초당 5회 초과 요청 거부 (state rejected 2) | ❌ | [] |
| 엔진 메시지 없음(점화 정보 stale) → 창문 명령 거부(2) | ❌ | None |
| 같은 상황에서 실내등 명령은 허용(0) | ❌ | None |
| 엔진 재개 후 창문 명령 허용(0) | ❌ | None |
| 다른 OS 주체가 실행 중인 신뢰 라우팅 매니저를 대리 사용하지 못함 | ❌ | probe=no security rejection evidence, response=None, window=None->None |
| 신뢰 OS 주체는 대체 client ID 0x2FFF로도 정상 요청 | ❌ | None |
| 승인 client ID 0x2002를 사용해도 비신뢰 OS 주체는 거부 | ❌ | probe=no security rejection evidence, response=None, window=None->None |
| BodyControl veth traffic은 ESP로 보호되고 clear TCP/30512가 없음 | ❌ | esp=False, clear=0 |

## 응답 누락 (0/4)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| Window 응답 중단 → 3회 누락 후 통신 오류·not fresh 표시 | ❌ | None s, None |
| 통신 오류 중 창문 명령 → LIN timeout(3) | ❌ | None |
| 다른 Slave(Door·Light)는 정상 유지 | ❌ | None |
| Light checksum 오류 응답 → 3회 후 통신 오류 | ❌ | None s |

## 복구 (0/4)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| Window 응답 재개 → 정상 3회 후 복구·fresh | ❌ | None s |
| 복구 후 창문 명령 accepted(0)·동작 | ❌ | None |
| Light checksum 정상화 → 복구 | ❌ | None s |
| 복구 후 실내등 명령 accepted(0) | ❌ | None |

## LIN 프로토콜 (6/11)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 5초간 명령·상태 변화 없음 → LIN sleep | ❌ | None s |
| sleep 중 명령 → wakeup 후 accepted(0) | ❌ | None |
| 모든 header의 PID parity가 표준 표와 일치 (주입한 1건 제외) | ✅ | 358 headers |
| 스케줄 순서 0x11→0x12→0x13→0x14→0x10→0x11→0x12 반복 | ✅ | ['0x11', '0x92', '0xd3', '0x14', '0x50', '0x11', '0x92', '0x11'] |
| slot 간격 10 ms (±3), idle 구간 40 ms (±3) | ✅ | min 9.0 max 41.3 ms |
| checksum 오류는 손상 주입 구간에만 (enhanced checksum) | ❌ | 오류 0 / 347 응답, 구간 밖 0 |
| parity가 틀린 header(0x10, 올바른 값 0x50)에는 Door가 응답하지 않음 | ✅ | 주입 header 있음, 다음 frame None |
| Door Slave가 parity 오류 header를 기록 | ✅ | [lin_door] headers=357 bad_parity=1 responses=51 commands=0 bad_checksum=0 wakeups=0 |
| sleep 동안 LIN 버스 header 0개 | ✅ | 0 headers in 1.00 s |
| wakeup 신호 후 100 ms 이상 지나 0 ms slot(0x11)부터 재시작 | ❌ | [] ms, PID None |
| 실내등 fade: 100 ms마다 10%씩 밝아짐 | ❌ | [] |

## Gateway 보고

```json
{"component": "gateway", "frames_read": 8432, "diag_frames": 0, "lost_frames": 0, "unattributed_errors": 0, "events_sent": 4428, "snapshot_requests": 0, "malformed_requests": 0, "final_state": 3, "pdus": [{"id": "0x100", "name": "PT_EngineStatus", "accepted": 8431, "rejected": 1}, {"id": "0x110", "name": "PT_VehicleDynamics", "accepted": 0, "rejected": 0}, {"id": "0x180", "name": "CH_BrakeStatus", "accepted": 0, "rejected": 0}, {"id": "0x200", "name": "CH_SteeringStatus", "accepted": 0, "rejected": 0}], "lin": {"sleeps": 1, "slot_overruns": 0, "unexpected_frames": 1, "requests": 0, "security_events": 0, "events_sent": 886, "door": {"ok": 51, "missed": 0, "bad_checksum": 0, "recoveries": 0, "degraded": false}, "window": {"ok": 102, "missed": 0, "bad_checksum": 0, "recoveries": 0, "degraded": false}, "light": {"ok": 51, "missed": 0, "bad_checksum": 0, "recoveries": 0, "degraded": false}}}
```
