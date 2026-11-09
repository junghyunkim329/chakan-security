# Phase 4 Execution Manager 시나리오 시험

- 결과: **PASS**
- 기준 16/16 통과

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 시작 후 모든 프로세스 Running | ✅ | 184 ms |
| 의존 순서 시작: CAN ECU 준비 → Gateway → consumer | ✅ | engine → brake → steering → gateway → consumer |
| 5개 프로세스 non-root·capability 0·NoNewPrivs·CPU·netns 확인 | ✅ | brake: uid=1002 CapEff=0000000000000000 CapPrm=0000000000000000 NoNewPrivs=1 cpus=1 netns=hj_veh memory_mb=64; consumer: uid=1002 CapEff=0000000000000000 CapPrm=0000000000000000 NoNewPrivs=1 cpus=3 netns=hj_ap memory_mb=1024; engine: uid=1002 CapEff=0000000000000000 CapPrm=0000000000000000 NoNewPrivs=1 cpus=1 netns=hj_veh memory_mb=64; gateway: uid=1002 CapEff=0000000000000000 CapPrm=0000000000000000 NoNewPrivs=1 cpus=2 netns=hj_gw memory_mb=1024; steering: uid=1002 CapEff=0000000000000000 CapPrm=0000000000000000 NoNewPrivs=1 cpus=1 netns=hj_veh memory_mb=64 |
| 메모리 상한(RLIMIT_AS) 적용 | ✅ | {'engine': True, 'brake': True, 'steering': True, 'gateway': True, 'consumer': True} |
| [종료] brake 장애 감지 0~500 ms | ✅ | 0.175326 ms (unexpected exit: killed by signal 9 (Killed)) |
| [종료] 상태 Degraded 기록 | ✅ | 0.175326 ms |
| [종료] 3초 안에 재시작 시도 (§8.2) | ✅ | 543.392443 ms |
| [종료] 새 pid로 Running 복귀 | ✅ | 복구 544.341208 ms, pid 10226 -> 10300 |
| [응답 정지] steering 장애 감지 1000~2000 ms | ✅ | 1363.321264 ms (heartbeat timeout (3 missed)) |
| [응답 정지] 상태 Degraded 기록 | ✅ | 1363.321264 ms |
| [응답 정지] 3초 안에 재시작 시도 (§8.2) | ✅ | 512.136345 ms |
| [응답 정지] 새 pid로 Running 복귀 | ✅ | 복구 1876.773601 ms, pid 10227 -> 10323 |
| [반복 실패] 60초 안 3회 실패 → 자동 재시작 중지 | ✅ | 3 failures within 60 s: automatic restart stopped, engine=Failed |
| [반복 실패] 상태 Degraded 유지 | ✅ | Degraded |
| 역순 종료: consumer → gateway → steering → brake | ✅ | consumer → gateway → steering → brake |
| 종료 후 남은 프로세스 0, 최종 상태 Shutdown, EM 종료코드 0 | ✅ | alive=[], state=Shutdown, rc=0, 17 ms |

## 측정값

```json
{
  "종료": {
    "process": "brake",
    "old_pid": 10226,
    "new_pid": 10300,
    "detect_ms": 0.175326,
    "degraded_ms": 0.175326,
    "restart_attempt_ms": 543.392443,
    "recovered_ms": 544.341208,
    "failure": "unexpected exit: killed by signal 9 (Killed)"
  },
  "응답 정지": {
    "process": "steering",
    "old_pid": 10227,
    "new_pid": 10323,
    "detect_ms": 1363.321264,
    "degraded_ms": 1363.321264,
    "restart_attempt_ms": 512.136345,
    "recovered_ms": 1876.773601,
    "failure": "heartbeat timeout (3 missed)"
  },
  "반복 실패": {
    "budget_exhausted_ms": 1119.045047,
    "engine": {
      "name": "engine",
      "state": "Failed",
      "pid": 0,
      "restarts": 2
    },
    "state": "Degraded"
  }
}
```
