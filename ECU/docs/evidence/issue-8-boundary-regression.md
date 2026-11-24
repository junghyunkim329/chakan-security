# Issue #8 BodyControl 경계 회귀시험

기존 시험은 비신뢰 vSomeIP 프로세스의 routing host lock 오류를 확인했으므로 직접 TCP 송신이나 실행 중 routing manager에 대한 실제 proxy 연결을 검증하지 못했다. 이번 변경은 배포 정책을 유지하고 해당 경계에 도달한 증거를 추가한다.

| 시험 | 도달·거부 증거 |
| --- | --- |
| 다른 non-root UID의 직접 TCP/30512 | 실제 socket connect 시도, UID·source port, AP observed counter 증가, UID/authorized counter 불변, 같은 packet trace ID의 AP drop |
| 실제 vSomeIP Proxy, 기존·대체 client ID | Proxy 인스턴스 로그, 신뢰 UID 소유의 기존 routing socket 존재, 해당 AF_UNIX 경로의 connect EACCES |
| 비보호 TCP/30512 및 UDP/30511 method | ns_ap의 시험 전용 root L2 주입, Gateway 수신 packet과 같은 trace ID의 input drop, IPsec verified counter 불변 |
| 정상 통신·정리 | 기존·대체 client ID 성공, 주입 전후 정상 구간 ESP·평문 TCP 부재, 시험 후 정상 명령 성공, 임시 table·설정 제거, ns_fw·XFRM 정책 동일 |

Gateway 주입은 AP 송신 통제를 우회해 Gateway만 독립 시험하기 위한 시험 도구다. `ns_ap/veth_ap`와 Gateway의 veth를 확인하며 물리 인터페이스에는 주입하지 않는다. 배포 방화벽과 XFRM 정책은 제거하거나 완화하지 않는다. 관찰용 table은 nftrace만 설정하며 기존 ns_fw의 accept/drop 판정을 유지한다.

## 실행

현재 setup이 구성한 namespace와 BodyControl principal을 그대로 사용한다. root 터미널에서 다음 명령을 실행한다. `strace` 등 필수 도구가 없으면 runner가 시험 시작 전에 중단한다.

```bash
cd /home/shsh/chakan-security
ECU/scripts/run/lin-scenario-test.sh
```

완료 시 출력한 `docs/results/<시각>_lin/`의 `summary.md`, `verdict.json`, `report.json`을 확인한다. 상세 증거는 같은 폴더의 `*.nft-trace.txt`, `*.connect-trace.txt`, `*.runtime.log`에 남긴다. 모든 판정과 복원 검사가 통과해야 완료다. 도구 오류, 경로 미도달, lock 오류 또는 액추에이터 무변화만으로 통과시키지 않는다.

실행별 결과 파일은 `project.md` §15.2에 따라 로컬 전용이다. 이 문서에는 재현 방법과 주요 검증 요약만 유지한다. 아래 과거 실행 경로는 로컬 기록의 식별자이며 새 checkout에는 포함되지 않는다. 공개된 기존 근거는 [해결 커밋의 최종 결과](https://github.com/junghyunkim329/chakan-security/blob/1b7e1f048cc5777e0e962546aee55edfef196bb0/docs/results/20261123-235545_lin/summary.md)와 [이슈 #8 해결 댓글](https://github.com/junghyunkim329/chakan-security/issues/8#issuecomment-7105027876)에서 확인할 수 있다.

## 검증 상태

- 일반 권한 검증: `test_body_security_config.py` 18개, CTest 16개 통과. shellcheck, Python 구문 검사, diff 공백 검사 통과.
- 첫 root 통합시험: `docs/results/20261123-235144_lin/`에서 40/43 통과. 실제 Proxy mode의 기존·대체 client ID 모두 UDS connect EACCES를 확인했다. 직접 socket과 비보호 TCP는 실제 nft trace에 drop이 기록됐으나 복원 검사 예외로 최종 판정에 반영하지 못했으며 UDP는 실행되지 않았다. 정상 요청·ESP 보호·LIN 검사는 통과했다.
- 실패 원인과 수정: iproute2 XFRM policy 출력은 전역 `-j`에도 일반 텍스트인데 시험 도구가 JSON으로 해석했다. `ip -s xfrm policy list` 텍스트를 전후 파일로 보존하고 사용량·사용 시각만 제외한 정책 설정을 비교하도록 수정했다. lifetime limit 및 required/use 변경은 실패로 판정하는 회귀시험을 추가했다. [iproute2 policy 출력 구현](https://github.com/iproute2/iproute2/blob/main/ip/ipxfrm.c)의 `xfrm_policy_info_print`와 `xfrm_lifetime_print`를 대조했다.
- 수정 후 root 통합시험: 사용자가 원격 root 터미널에서 재실행한 `docs/results/20261123-235545_lin/`은 **44/44 통과**했다. 상세 probe·복원 보고 10개도 모두 통과했으며 실제 trace와 전후 정책 파일을 대조했다. 첫 실패 결과도 로컬에 그대로 보존한다.
- 이 시험은 프로젝트 SIL의 기존 통제를 대상으로 하며 RAV4 양산 ECU의 UID 정책·IPsec 배치나 규정 적합성을 증명하지 않는다.

## 최종 root 실행의 관측값

- 직접 socket: UID/GID 1001/1001, supplementary group 없음, source port 48917, connect timeout(errno 110). AP TCP observed는 7→9, UID matched와 authorized는 각각 5→5였다. 같은 packet trace의 output policy drop을 확인했다.
- 실제 Proxy: 기존·대체 ID 모두 UID 1001로 실행, 신뢰 routing socket `/tmp/vsomeip-sil-apbody-0`에 connect EACCES. Proxy mode 로그와 정확한 UDS 경로를 함께 확인했다. 프로세스 종료코드 137은 시험 timeout의 강제 종료이며 거부 판정의 근거는 connect trace다.
- 비보호 TCP: root 시험 도구가 veth에 54바이트 SYN 1개를 주입했다. Gateway TCP received 7→8, IPsec verified 6→6, 동일 packet trace의 input policy drop을 확인했다.
- 비보호 UDP: root 시험 도구가 기존 service/method ID의 SetWindowPosition(73)을 담은 59바이트 frame 1개를 주입했고 Gateway input policy drop을 확인했다. IPsec verified는 6→6이었다.
- 복원 5건 통과. 정상 기존·대체 client ID 요청, 시험 후 정상 명령, 전후 정상 구간 ESP 보호와 평문 TCP 부재, 기존 LIN 시나리오도 통과했다.
