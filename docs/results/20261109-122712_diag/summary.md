# Phase 5 진단 시나리오 시험

- 결과: **PASS**
- 기준 39/39 통과

## 정상 (13/13)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 차량 식별 (UDP broadcast) | ✅ | {'type': 4, 'vin': 'SILTEST0000000001', 'logical_address': 4096, 'eid': '02:00:00:00:10:00', 'gid': '02:00:00:00:10:00', 'further_action': 0, 'from': '10.10.0.1'} |
| tester 0x0E80 routing activation | ✅ | code=16 |
| Engine F100 엔진 회전수 | ✅ | 800 rpm, 0.2 ms |
| Brake F101 차량 속도 | ✅ | 0 km/h |
| Engine DTC 읽기 (주입한 2개) | ✅ | ['01170009', '03350009'] |
| TesterPresent | ✅ | ack=ack resp=7e00 |
| Extended 세션 진입 | ✅ | ack=ack resp=5003003201f4 |
| SecurityAccess (16-byte seed, HMAC-SHA-256) | ✅ | ack=ack resp=6702 |
| 보안 해제 후 DTC 삭제 | ✅ | ack=ack resp=54 |
| 삭제 후 DTC 없음 | ✅ | ack=ack resp=590209 |
| ECUReset | ✅ | ack=ack resp=5101 |
| Reset 후 Default 세션·잠김 | ✅ | ack=ack resp=62f1810100 |
| 다른 ECU는 계속 응답 | ✅ | ack=ack resp=7e00 |

## 권한 부족 (12/12)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 등록되지 않은 tester 주소 0x0E81 routing 거부 | ✅ | code=0 closed=True |
| routing activation 전 진단 요청 거부 | ✅ | ack=nack(0x02) resp=None |
| Default 세션 DTC 삭제 거부 (NRC 0x7F) | ✅ | ack=ack resp=7f147f |
| Default 세션 SecurityAccess 거부 (NRC 0x7F) | ✅ | ack=ack resp=7f277f |
| 보안 해제 전 DTC 삭제 거부 (NRC 0x33) | ✅ | ack=ack resp=7f1433 |
| 보안 해제 전 ECUReset 거부 (NRC 0x33) | ✅ | ack=ack resp=7f1133 |
| 지원하지 않는 DID 거부 (NRC 0x31) | ✅ | ack=ack resp=7f2231 |
| 범위 밖 서비스 RequestDownload 거부 (NRC 0x11) | ✅ | ack=ack resp=7f3411 |
| 알 수 없는 대상 주소 0x1003 (DoIP NACK 0x03) | ✅ | ack=nack(0x03) resp=None |
| 잘못된 key 3회: 0x35, 0x35, 0x36 | ✅ | ['7f2735', '7f2735', '7f2736'] |
| 5회 실패 후 잠금 (NRC 0x36, 재시작 전까지) | ✅ | ack=ack resp=7f2736 / ack=ack resp=7f2736 |
| 잠금 중에는 올바른 key도 불가 | ✅ | ack=ack resp=7f2736 |

## 시간 초과 (7/7)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| 10초 지연 중 seed 요청 거부 (NRC 0x37) | ✅ | ack=ack resp=7f2737 |
| TesterPresent 5초 없으면 Default 복귀 (S3) | ✅ | ack=ack resp=62f1810100 |
| S3 이후 DTC 삭제 거부 (NRC 0x7F) | ✅ | ack=ack resp=7f147f |
| TesterPresent 유지 시 Extended·해제 유지 | ✅ | ack=ack resp=62f1810301 |
| 10초 지연 후 seed 요청 다시 허용 | ✅ | ack=ack resp=6701b39c61db002d2de37ecf25c80529e134 |
| 응답 없는 ECU: ack 후 P2* 안에 응답 없음 | ✅ | ack=ack resp=None, 6007 ms |
| 없는 ECU에 다중 프레임 요청: Flow Control 시간 초과 → NACK 0x06 | ✅ | ack=nack(0x06) resp=None, 1302 ms |

## 다중 프레임 (7/7)

| 기준 | 결과 | 값 |
| --- | --- | --- |
| Engine F180 소프트웨어 버전 (응답 > 7 byte) | ✅ | 55 byte |
| Engine DID 4개 요청 (요청 9 byte) | ✅ | 67 byte 응답 |
| Brake DID 6개 요청 (요청 13 byte) | ✅ | 72 byte 응답 |
| CAN 캡처: Engine 요청 0x7E0에 FF·CF, 응답 0x7E8에 FC | ✅ | {'SF': 27, 'FF': 5, 'CF': 14, '0x7E8 FC': 5} |
| CAN 캡처: Engine 응답 0x7E8에 FF·CF, 요청 0x7E0에 FC | ✅ | {'SF': 22, 'FF': 6, 'CF': 23, '0x7E0 FC': 6} |
| CAN 캡처: Brake 0x7E1/0x7E9 다중 프레임 | ✅ | {'0x7e1 CF': 21, '0x7e1 FC': 6, '0x7e1 FF': 7, '0x7e1 SF': 12, '0x7e9 CF': 20, '0x7e9 FC': 6, '0x7e9 FF': 6, '0x7e9 SF': 11} |
| CAN 캡처: 모든 진단 frame 8 byte (padding 0xCC) | ✅ | 197 frames |
