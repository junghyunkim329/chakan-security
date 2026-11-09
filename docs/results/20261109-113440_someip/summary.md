# Phase 3 SOME/IP 통합 시험

- 결과: **PASS**
- 실행 시간 30s, 워밍업 3s 제외

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 서비스 발견 5초 이내 | ✅ | 61.7 ms |
| CAN 수신→event 수신 p99 ≤ 50 ms | ✅ | p50 12.758 / p99 13.691 / max 14.62 ms |
| 측정 event 수 ≥ 예상의 90% | ✅ | 1353 / 예상 1350 |
| event 순번 누락 0 | ✅ | 0 |
| 잘못된 event 0 | ✅ | 0 |
| 워밍업 뒤 validity 전부 1 | ✅ | 예외 0건 |
| 워밍업 뒤 freshness 전부 1 | ✅ | 예외 0건 |
| 워밍업 뒤 Gateway 상태 Running | ✅ | 1353 / 1353 |
| CAN 값이 event에 반영 (rpm 800~2500) | ✅ | rpm 800~2498 |
| CAN 값이 event에 반영 (속도 60 km/h) | ✅ | speed max 60.00 km/h |
| GetSnapshot 응답 정상 | ✅ | True |
| 비어 있지 않은 GetSnapshot 요청 거부 | ✅ | True |
| Gateway가 거부한 정상 CAN frame 0 | ✅ | rejected 0, unattributed 0, lost 0 |

## Gateway CAN 입력

| ID | 메시지 | 통과 | 거부 |
| --- | --- | --- | --- |
| 0x100 | PT_EngineStatus | 3300 | 0 |
| 0x110 | PT_VehicleDynamics | 1650 | 0 |
| 0x180 | CH_BrakeStatus | 3300 | 0 |
| 0x200 | CH_SteeringStatus | 3301 | 0 |

- Gateway가 보낸 event: 1649, GetSnapshot 요청: 2 (그중 거부 1)
