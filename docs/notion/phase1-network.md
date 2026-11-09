# Phase 1. 네트워크 격리 (연습 구현 · 호진)

> 상태: 🟢 연습 구성(`hj_`)에서 직접 구현·확인 완료 / 공식 구성(`ns_`)은 김정현님 구현 사용
> 날짜: 2026-11-02
> 환경: chakan VM (Ubuntu 26.04.1 LTS, kernel 7.0.0-38-generic)
> 관련: `project.md` §4.2, §7.1, §11.1, §16 Phase 1

---

## ✅ 할 일 / 완료 기준

**할 일**
- [x] AP·Gateway·차량 영역의 namespace 구성
- [x] 가상 Ethernet/CAN 연결
- [x] 방화벽과 반복 실행 가능한 설치·정리 스크립트 작성

**완료 기준**
- [x] AP의 직접 CAN 접근 실패
- [x] Gateway 통신 성공

> ⚠️ 위 체크는 **연습용 이름(`hj_ap`, `hj_gw`, `hj_veh`)** 으로 만든 구성에서 확인한 결과입니다. 팀 공용 `ns_ap`/`ns_gateway`/`ns_vehicle`은 김정현님이 사용 중이라 건드리지 않았습니다.

---

## 1. 한 줄 요약

컴퓨터 한 대 안에 **서로 못 보는 방 3개**를 만들고, **필요한 곳에만 선을 연결**한 뒤, **정해진 통신만 통과**하도록 방화벽을 걸었다.

```
[hj_ap] ════ Ethernet(veth) ════ [hj_gw] ──── CAN(vxcan) ──── [hj_veh]
 중앙 컴퓨터                       관문                          엔진·브레이크 ECU
 10.10.0.10                      10.10.0.1
 ❌ CAN선 없음                    ✅ 양쪽 다 연결                ❌ IP 없음
```

## 2. 왜 이렇게 하나 (쉽게)

| 자동차 | 학교 비유 |
| --- | --- |
| 차량 영역 (엔진·브레이크) | 교무실 — 중요한 시험지가 있음 |
| Gateway | 행정실 — 요청을 확인하고 전달 |
| AP (중앙 컴퓨터) | 교실 — 학생(앱)들이 있음 |

- 중앙 컴퓨터는 인터넷·앱과 연결돼 있어서 **해킹당하기 가장 쉽다.**
- 거기서 브레이크에 **바로** 명령을 보낼 수 있으면 위험 → **반드시 Gateway를 거치게** 한다.
- 근거: **UN R155 Annex 5 위협 29.2** "보호되지 않은 게이트웨이로 네트워크 분리를 우회해 임의의 CAN 메시지를 보내는 것", ISO/SAE 21434 테스트 매뉴얼 "외부 네트워크와 차량 네트워크 분리, 화이트리스트 기반 라우팅"

---

## 3. 단계별로 한 일

### 1단계: 방(network namespace) 만들기
- Linux network namespace = 네트워크가 완전히 따로 노는 방. 다른 방의 장치는 보이지도 않음
- 만들면 `lo`(방 안 내선 전화)만 있고 꺼져 있음 → 켜 줌

```bash
ip netns add hj_ap
ip -n hj_ap link set lo up
```

**확인한 것**
- 방 안에서는 본체의 `ens33`, `tailscale0`이 안 보임 → 격리 ✅
- 방 안에서 인터넷(`8.8.8.8`) ping 실패 → 밖으로 나갈 길 없음 ✅
- `lo` 켜기 전 `127.0.0.1` ping 실패 → 켠 뒤 성공 ✅

### 2단계: AP ↔ Gateway 랜선(veth)
- veth = 양쪽 끝이 있는 가상 랜선. 실제 차의 차량용 Ethernet에 해당

```bash
ip link add veth_ap netns hj_ap type veth peer name veth_gw netns hj_gw
ip -n hj_ap addr add 10.10.0.10/24 dev veth_ap
ip -n hj_gw addr add 10.10.0.1/24 dev veth_gw
ip -n hj_ap link set veth_ap up
ip -n hj_gw link set veth_gw up
```

**확인한 것**
- AP ↔ Gateway ping 양방향 성공 ✅
- 차량 방 → Gateway ping 실패 (선 없음) ✅

### 3단계: Gateway ↔ 차량 CAN선(vxcan)
- vxcan = veth의 CAN 버전. 실제 차의 CAN 배선에 해당
- CAN은 IP 주소가 없고 **메시지 ID**(예: `0x100` 엔진 상태)로 구분
- **AP에는 CAN선을 꽂지 않는다** ← 핵심

```bash
ip link add vxcan_gw netns hj_gw type vxcan peer name vxcan_veh netns hj_veh
ip -n hj_gw link set vxcan_gw up
ip -n hj_veh link set vxcan_veh up
```

**확인한 것**

| 시험 | 결과 |
| --- | --- |
| 차량 → Gateway CAN `0x100` | `vxcan_gw 100 [8] 11 22 33 44 55 66 77 88` 수신 ✅ |
| Gateway → 차량 CAN `0x7E0` | `vxcan_veh 7E0 [4] 02 22 F1 00` 수신 ✅ |
| AP(일반 사용자)에서 CAN선 열기 | `SIOCGIFINDEX: No such device` → 접근 실패 ✅ |
| AP(일반 사용자)에서 모든 CAN 엿듣기 5초 | 아무것도 안 들림, 시간 초과(종료코드 124) ✅ |

> AP 시험은 실제 앱처럼 **관리자 권한 없는 사용자(`nobody`)** 로 실행. 관리자는 다른 방으로 이동할 수 있어서, 일반 앱 입장에서 시험해야 의미가 있음.

### 4단계: 방화벽 (nftables)
- 원칙: **기본은 전부 차단**, 출입 명단에 있는 것만 통과
- 방화벽은 **IP 통신(Ethernet)만** 걸러냄. CAN은 대상 아님

| 방 | 출입 명단 |
| --- | --- |
| `hj_ap` | Gateway로 요청 보내기만: SOME/IP-SD(UDP 30490), SOME/IP 메서드(TCP 30502·30512), DoIP(TCP/UDP 13400) + 받은 요청의 답장 |
| `hj_gw` | AP 요청 받기 + 상태 방송(UDP 30501·30511) + SD·DoIP 응답 |
| `hj_veh` | IP 전부 차단 (CAN만 쓰는 방) |

**확인한 것**

| 시험 | 결과 | 의미 |
| --- | --- | --- |
| AP → GW ping | 차단 ✅ | 명단에 없는 건 막힘 |
| AP → GW TCP 30502 | `succeeded!` ✅ | SOME/IP 메서드는 통과 |
| AP → GW TCP 2222 | `timed out` ✅ | 아무 포트나 못 씀 |
| GW → AP TCP 8080 (먼저 연결) | `timed out` ✅ | 반대 방향 침투 불가 (양쪽 방화벽 이중 방어) |
| 차량 → GW CAN | 수신 ✅ | 방화벽은 CAN에 영향 없음 |
| 본체 `nft list tables`에 `hj_fw` 없음 | ✅ | 본체 방화벽은 안 건드림 |

### 5단계: 설치·정리 스크립트
- `practice/scripts/setup.sh`: 1~4단계를 한 번에. 시작할 때 `cleanup.sh`를 먼저 불러서 **몇 번 돌려도 같은 결과**
- `practice/scripts/cleanup.sh`: `hj_` 방만 삭제 (공용 `ns_`는 절대 안 건드림)
- `practice/firewall/hj_*.nft`: 방별 방화벽 규칙 (줄마다 설명 주석)
- 방화벽 적용에 안전장치: **그 방 안이 맞을 때만** 적용 (실수로 본체에 걸리면 SSH 끊김)

**확인한 것**
- `setup.sh` 2회 연속 실행 → 둘 다 `설치 완료`, 결과 동일 ✅
- 세 방 장치 구성과 `hj_fw` 방화벽 존재, 본체에는 `hj_fw` 없음 ✅
- 스크립트로 만든 구성에서 기능 시험(CAN·방화벽) 재확인: ⬜ (진행 예정)

---

## 4. 실행 방법

```bash
su -
/home/ghwls/work/practice/scripts/setup.sh     # 설치 (재실행 가능)
/home/ghwls/work/practice/scripts/cleanup.sh   # 정리
exit
```

---

## 5. 주장할 수 있는 것 / 없는 것

✅ 주장 가능
- 한 VM 안에서 AP·Gateway·차량 영역이 네트워크적으로 분리되어 있다.
- 일반 권한 AP 프로세스는 차량 CAN 장치를 열거나 엿들을 수 없다.
- Gateway만 Ethernet과 CAN 양쪽에 연결되어 있다.
- AP↔Gateway 사이에는 정해진 포트만 통과한다.

❌ 아직 주장 불가
- AP 요청이 Gateway **프로그램**을 거쳐 실제 CAN 메시지로 변환·검사되는 것 → Phase 3 (Gateway 프로그램)
- 실제 SOME/IP·DoIP 프로토콜 동작 → 이번엔 포트 통과 여부만 확인

⚠️ 실차와 다른 점
- 실제 차는 ECU가 **물리적으로 다른 칩·전선**. 여기선 소프트웨어로 나눈 방
- 관리자(root) 프로세스는 다른 방으로 들어갈 수 있음 → 앱은 반드시 일반 권한으로 실행해야 격리가 유효
- vxcan은 실제 CAN의 전압·속도·충돌 처리(arbitration)를 재현하지 않음
- DLT 로그 통신은 포트·방향이 미정이라 방화벽에 아직 없음

---

## 6. 용어

| 용어 | 뜻 |
| --- | --- |
| ECU | 자동차 부품 하나를 담당하는 작은 컴퓨터 |
| AP (Adaptive Platform) | 고성능 중앙 컴퓨터용 소프트웨어 구조 (AUTOSAR) |
| Gateway | 서로 다른 차량 네트워크 사이에서 메시지를 검사·전달하는 관문 |
| network namespace | 리눅스에서 네트워크를 따로 쓰는 독립된 방 |
| `lo` (loopback) | 같은 방 안 프로그램끼리 쓰는 내선 전화 |
| veth | 양쪽 끝이 있는 가상 랜선 |
| vxcan | 양쪽 끝이 있는 가상 CAN선 |
| CAN | 차 안 ECU들이 짧은 메시지를 주고받는 통신 방식 |
| nftables | 리눅스 방화벽 |
| `policy drop` | 기본은 전부 차단 |
| `ct state established` | 내가 먼저 건 대화의 답장은 허용 |
| SOME/IP | 차량용 서비스 통신 (Gateway → AP 상태 전달) |
| DoIP | Ethernet으로 진단 메시지 전달 |
| UN R155 | 자동차 사이버보안 국제 규정 |
