# Phase 6 LIN 차체 도메인 시나리오 시험 (연습 구성 hj_*)

- 결과: **FAIL**
- 기준 35/38 통과

## 명령·응답 (18/19)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| AP가 BodyControl 서비스 발견 | ✅ | 151 ms |
| 초기 상태: 문 닫힘·중앙 잠금, 창문 0, 실내등 꺼짐, 모두 fresh | ✅ | {'type': 'event', 'seq': 26, 't_us': 552603853728, 'door_bits': 16, 'window': 0, 'light': 0, 'flags': 0, 'fresh': 7} |
| SetWindowPosition(50) → accepted(0) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 23.137, 't_us': 552603913526} |
| 창문이 5%씩 움직여 50 도달 (50 ms마다 5% → 약 0.5 s) | ✅ | 0.563 s |
| event에서 창문 위치가 증가만 함 (100 ms event 사이 최대 10%) | ✅ | [10, 10, 10, 10, 5] |
| SetInteriorLight(1) → accepted(0), 실내등 켜짐 | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 0, 'rtt_ms': 23.545, 't_us': 552604483158}, 0.093 s |
| Door ECU 입력(앞좌 문 열림)이 event에 반영 | ✅ | 0.099 s |
| 잘못된 인자 (범위 밖 101) → invalid argument(1) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 1, 'rtt_ms': 10.964, 't_us': 552604669314} |
| 잘못된 인자 (payload 2 byte) → invalid argument(1) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 1, 'rtt_ms': 11.008, 't_us': 552604689783} |
| 잘못된 인자 (실내등 2) → invalid argument(1) | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 1, 'rtt_ms': 10.903, 't_us': 552604710627} |
| 장애물 주입: 창문 정지·obstruction·fault 표시 | ✅ | {'type': 'event', 'seq': 40, 't_us': 552605253536, 'door_bits': 16, 'window': 50, 'light': 1, 'flags': 3, 'fresh': 7} |
| 초당 5회 초과 요청 거부 (state rejected 2) | ✅ | [2, 2, 0, 0, 0, 0, 0] |
| 엔진 메시지 없음(점화 정보 stale) → 창문 명령 거부(2) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 2, 'rtt_ms': 11.273, 't_us': 552608187447} |
| 같은 상황에서 실내등 명령은 허용(0) | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 0, 'rtt_ms': 86.344, 't_us': 552608283491} |
| 엔진 재개 후 창문 명령 허용(0) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 69.719, 't_us': 552608863105} |
| 다른 OS 주체가 실행 중인 신뢰 라우팅 매니저를 대리 사용하지 못함 | ✅ | probe=routing lock permission denied, response=None, window=0->0 |
| 신뢰 OS 주체는 대체 client ID 0x2FFF로도 정상 요청 | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 125.061, 't_us': 552626101889} |
| 승인 client ID 0x2002를 사용해도 비신뢰 OS 주체는 거부 | ✅ | probe=routing lock permission denied, response=None, window=60->60 |
| BodyControl veth traffic은 ESP로 보호되고 clear TCP/30512가 없음 | ❌ | esp=True, clear=1 |

## 응답 누락 (4/4)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| Window 응답 중단 → 3회 누락 후 통신 오류·not fresh 표시 | ✅ | 0.186 s, {'type': 'event', 'seq': 90, 't_us': 552610252988, 'door_bits': 16, 'window': 10, 'light': 0, 'flags': 16, 'fresh': 5} |
| 통신 오류 중 창문 명령 → LIN timeout(3) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 3, 'rtt_ms': 11.237, 't_us': 552610265692} |
| 다른 Slave(Door·Light)는 정상 유지 | ✅ | {'type': 'event', 'seq': 90, 't_us': 552610252988, 'door_bits': 16, 'window': 10, 'light': 0, 'flags': 16, 'fresh': 5} |
| Light checksum 오류 응답 → 3회 후 통신 오류 | ✅ | 0.279 s |

## 복구 (4/4)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| Window 응답 재개 → 정상 3회 후 복구·fresh | ✅ | 0.196 s |
| 복구 후 창문 명령 accepted(0)·동작 | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 57.157, 't_us': 552610812728} |
| Light checksum 정상화 → 복구 | ✅ | 0.292 s |
| 복구 후 실내등 명령 accepted(0) | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 0, 'rtt_ms': 119.223, 't_us': 552611383553} |

## LIN 프로토콜 (9/11)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 5초간 명령·상태 변화 없음 → LIN sleep | ✅ | 4.15 s |
| sleep 중 명령 → wakeup 후 accepted(0) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 122.745, 't_us': 552618486381} |
| 모든 header의 PID parity가 표준 표와 일치 (주입한 1건 제외) | ✅ | 1788 headers |
| 스케줄 순서 0x11→0x12→0x13→0x14→0x10→0x11→0x12 반복 | ✅ | ['0x11', '0x92', '0xd3', '0x14', '0x50', '0x11', '0x92', '0x11'] |
| slot 간격 10 ms (±3), idle 구간 40 ms (±3) | ✅ | min 9.2 max 41.0 ms |
| checksum 오류는 손상 주입 구간에만 (enhanced checksum) | ❌ | 오류 8 / 1648 응답, 구간 밖 1 |
| parity가 틀린 header(0x10, 올바른 값 0x50)에는 Door가 응답하지 않음 | ❌ | 주입 header 있음, 다음 frame 0x2 |
| Door Slave가 parity 오류 header를 기록 | ✅ | [lin_door] headers=1788 bad_parity=1 responses=255 commands=0 bad_checksum=0 wakeups=2 |
| sleep 동안 LIN 버스 header 0개 | ✅ | 0 headers in 1.00 s |
| wakeup 신호 후 100 ms 이상 지나 0 ms slot(0x11)부터 재시작 | ✅ | 100.1 ms, PID 0x11 |
| 실내등 fade: 100 ms마다 10%씩 밝아짐 | ✅ | [10] |

## Gateway 보고

```json
{"component": "gateway", "frames_read": 2864, "diag_frames": 0, "lost_frames": 0, "unattributed_errors": 0, "events_sent": 1449, "snapshot_requests": 0, "malformed_requests": 0, "final_state": 3, "pdus": [{"id": "0x100", "name": "PT_EngineStatus", "accepted": 2863, "rejected": 1}, {"id": "0x110", "name": "PT_VehicleDynamics", "accepted": 0, "rejected": 0}, {"id": "0x180", "name": "CH_BrakeStatus", "accepted": 0, "rejected": 0}, {"id": "0x200", "name": "CH_SteeringStatus", "accepted": 0, "rejected": 0}], "lin": {"sleeps": 2, "slot_overruns": 0, "unexpected_frames": 1, "requests": 21, "security_events": 3, "events_sent": 290, "door": {"ok": 255, "missed": 0, "bad_checksum": 0, "recoveries": 0, "degraded": false}, "window": {"ok": 501, "missed": 10, "bad_checksum": 0, "recoveries": 1, "degraded": false}, "light": {"ok": 248, "missed": 7, "bad_checksum": 7, "recoveries": 1, "degraded": false}}}
```
