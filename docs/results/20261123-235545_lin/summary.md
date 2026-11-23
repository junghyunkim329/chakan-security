# Phase 6 LIN 차체 도메인 시나리오 시험 (연습 구성 hj_*)

- 결과: **PASS**
- 기준 44/44 통과

## 명령·응답 (19/19)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| AP가 BodyControl 서비스 발견 | ✅ | 202 ms |
| 초기 상태: 문 닫힘·중앙 잠금, 창문 0, 실내등 꺼짐, 모두 fresh | ✅ | {'type': 'event', 'seq': 26, 't_us': 986962085784, 'door_bits': 16, 'window': 0, 'light': 0, 'flags': 0, 'fresh': 7} |
| SetWindowPosition(50) → accepted(0) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 67.598, 't_us': 986962246257} |
| 창문이 5%씩 움직여 50 도달 (50 ms마다 5% → 약 0.5 s) | ✅ | 0.607 s |
| event에서 창문 위치가 증가만 함 (100 ms event 사이 최대 10%) | ✅ | [5, 10, 10, 10, 10, 5] |
| SetInteriorLight(1) → accepted(0), 실내등 켜짐 | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 0, 'rtt_ms': 23.641, 't_us': 986962815033}, 0.094 s |
| Door ECU 입력(앞좌 문 열림)이 event에 반영 | ✅ | 0.091 s |
| 잘못된 인자 (범위 밖 101) → invalid argument(1) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 1, 'rtt_ms': 10.625, 't_us': 986962998503} |
| 잘못된 인자 (payload 2 byte) → invalid argument(1) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 1, 'rtt_ms': 10.886, 't_us': 986963019839} |
| 잘못된 인자 (실내등 2) → invalid argument(1) | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 1, 'rtt_ms': 11.183, 't_us': 986963041157} |
| 장애물 주입: 창문 정지·obstruction·fault 표시 | ✅ | {'type': 'event', 'seq': 41, 't_us': 986963585731, 'door_bits': 16, 'window': 50, 'light': 1, 'flags': 3, 'fresh': 7} |
| 초당 5회 초과 요청 거부 (state rejected 2) | ✅ | [2, 2, 0, 0, 0, 0, 0] |
| 엔진 메시지 없음(점화 정보 stale) → 창문 명령 거부(2) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 2, 'rtt_ms': 11.024, 't_us': 986966517281} |
| 같은 상황에서 실내등 명령은 허용(0) | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 0, 'rtt_ms': 88.857, 't_us': 986966615857} |
| 엔진 재개 후 창문 명령 허용(0) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 23.123, 't_us': 986967145560} |
| 신뢰 OS 주체는 대체 client ID 0x2FFF로도 정상 요청 | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 124.446, 't_us': 986989523229} |
| 경계 시험 후에도 신뢰 principal의 명령 accepted(0) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 123.967, 't_us': 986999142905} |
| 정상 구간 eth_capture: ESP 관찰·clear TCP/30512 없음 | ✅ | esp=True, clear=0 |
| 정상 구간 eth_capture_after: ESP 관찰·clear TCP/30512 없음 | ✅ | esp=True, clear=0 |

## 권한 경계 (6/6)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 비신뢰 UID의 직접 TCP socket은 AP UID 방화벽에서 거부 | ✅ | {'probe': 'direct_socket', 'pass': True, 'principal_and_result': {'uid': 1001, 'gid': 1001, 'groups': [], 'pid': 321162, 'attempted': True, 'connected': False, 'errno': 110, 'source_port': 48917}, 'be |
| 대체 client ID의 실제 proxy UDS 연결은 권한 경계에서 거부 | ✅ | {'probe': 'proxy_alt_id', 'pass': True, 'principal': {'type': 'probe_principal', 'uid': 1001, 'gid': 1001, 'groups': [], 'pid': 321172}, 'proxy_mode': True, 'routing_socket': '/tmp/vsomeip-sil-apbody- |
| 승인 client ID를 사용한 비신뢰 실제 proxy도 UDS 연결 거부 | ✅ | {'probe': 'proxy_allowed_id', 'pass': True, 'principal': {'type': 'probe_principal', 'uid': 1001, 'gid': 1001, 'groups': [], 'pid': 321221}, 'proxy_mode': True, 'routing_socket': '/tmp/vsomeip-sil-apb |
| unprotected_tcp Gateway 입력 거부 | ✅ | {'probe': 'unprotected_tcp', 'pass': True, 'principal_and_result': {'uid': 0, 'gid': 0, 'groups': [0], 'pid': 321252, 'test_only_root_injection': True, 'protocol': 'tcp', 'source_port': 44545, 'sent': |
| unprotected_udp Gateway 입력 거부 | ✅ | {'probe': 'unprotected_udp', 'pass': True, 'principal_and_result': {'uid': 0, 'gid': 0, 'groups': [0], 'pid': 321278, 'test_only_root_injection': True, 'protocol': 'udp', 'source_port': 54313, 'sent': |
| 임시 설정 제거·기존 방화벽과 XFRM 정책 원복 확인 | ✅ | {'direct_socket_restoration': {'probe': 'direct_socket_restoration', 'pass': True, 'observer_removed': True, 'firewall_unchanged': True, 'xfrm_policy_unchanged': True}, 'proxy_alt_id_restoration': {'p |

## 응답 누락 (4/4)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| Window 응답 중단 → 3회 누락 후 통신 오류·not fresh 표시 | ✅ | 0.231 s, {'type': 'event', 'seq': 91, 't_us': 986968585150, 'door_bits': 16, 'window': 10, 'light': 0, 'flags': 16, 'fresh': 5} |
| 통신 오류 중 창문 명령 → LIN timeout(3) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 3, 'rtt_ms': 10.577, 't_us': 986968603679} |
| 다른 Slave(Door·Light)는 정상 유지 | ✅ | {'type': 'event', 'seq': 91, 't_us': 986968585150, 'door_bits': 16, 'window': 10, 'light': 0, 'flags': 16, 'fresh': 5} |
| Light checksum 오류 응답 → 3회 후 통신 오류 | ✅ | 0.373 s |

## 복구 (4/4)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| Window 응답 재개 → 정상 3회 후 복구·fresh | ✅ | 0.196 s |
| 복구 후 창문 명령 accepted(0)·동작 | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 58.371, 't_us': 986969246808} |
| Light checksum 정상화 → 복구 | ✅ | 0.289 s |
| 복구 후 실내등 명령 accepted(0) | ✅ | {'type': 'response', 'method': 2, 'return_code': 0, 'code': 0, 'rtt_ms': 26.792, 't_us': 986969715613} |

## LIN 프로토콜 (11/11)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 5초간 명령·상태 변화 없음 → LIN sleep | ✅ | 4.15 s |
| sleep 중 명령 → wakeup 후 accepted(0) | ✅ | {'type': 'response', 'method': 1, 'return_code': 0, 'code': 0, 'rtt_ms': 123.935, 't_us': 986976815513} |
| 모든 header의 PID parity가 표준 표와 일치 (주입한 1건 제외) | ✅ | 1944 headers |
| 스케줄 순서 0x11→0x12→0x13→0x14→0x10→0x11→0x12 반복 | ✅ | ['0x11', '0x92', '0xd3', '0x14', '0x50', '0x11', '0x92', '0x11'] |
| slot 간격 10 ms (±3), idle 구간 40 ms (±3) | ✅ | min 8.8 max 40.4 ms |
| checksum 오류는 손상 주입 구간에만 (enhanced checksum) | ✅ | 오류 7 / 1780 응답, 구간 밖 0 |
| parity가 틀린 header(0x10, 올바른 값 0x50)에는 Door가 응답하지 않음 | ✅ | 주입 header 있음, 다음 frame 0x1 |
| Door Slave가 parity 오류 header를 기록 | ✅ | [lin_door] headers=1944 bad_parity=1 responses=278 commands=0 bad_checksum=0 wakeups=3 |
| sleep 동안 LIN 버스 header 0개 | ✅ | 0 headers in 1.00 s |
| wakeup 신호 후 100 ms 이상 지나 0 ms slot(0x11)부터 재시작 | ✅ | 100.5 ms, PID 0x11 |
| 실내등 fade: 100 ms마다 10%씩 밝아짐 | ✅ | [10] |

## Gateway 보고

```json
{"component": "gateway", "frames_read": 3987, "diag_frames": 0, "lost_frames": 2, "unattributed_errors": 0, "events_sent": 2010, "snapshot_requests": 0, "malformed_requests": 0, "final_state": 3, "pdus": [{"id": "0x100", "name": "PT_EngineStatus", "accepted": 3987, "rejected": 0}, {"id": "0x110", "name": "PT_VehicleDynamics", "accepted": 0, "rejected": 0}, {"id": "0x180", "name": "CH_BrakeStatus", "accepted": 0, "rejected": 0}, {"id": "0x200", "name": "CH_SteeringStatus", "accepted": 0, "rejected": 0}], "lin": {"sleeps": 3, "slot_overruns": 0, "unexpected_frames": 1, "requests": 22, "security_events": 3, "events_sent": 402, "door": {"ok": 278, "missed": 0, "bad_checksum": 0, "recoveries": 0, "degraded": false}, "window": {"ok": 542, "missed": 13, "bad_checksum": 0, "recoveries": 1, "degraded": false}, "light": {"ok": 271, "missed": 7, "bad_checksum": 7, "recoveries": 1, "degraded": false}}}
```
