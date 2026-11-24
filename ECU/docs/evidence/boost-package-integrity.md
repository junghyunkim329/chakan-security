# Boost 패키지 무결성 검증 설계

Issue #7의 구현과 재현에 필요한 설계 근거다. 완료된 에이전트 작업계획과 구분해 유지하며 구현의 최종 기준은 `project.md` §7.5다.

## 목적

Issue #7의 공급망 경계를 닫는다. 현재 `apt-get download` 뒤 생성한 checksum과 재사용 디렉터리의 모든 `.deb`를 신뢰하는 동작을, 승인된 정확한 입력만 검증·추출하는 fail-closed 동작으로 바꾼다.

이 설계는 실차 ECU에서 APT를 실행한다는 모델이 아니다. 양산 차량 소프트웨어 빌드 환경에서 사용하는 승인 입력 목록, 사전 digest, 격리 작업 공간, 재현 가능한 검증을 현재 Ubuntu/CMake SIL에 적용하는 것이다.

따라서 주장 범위는 **“실차 탑재 소프트웨어를 만드는 양산 빌드·공급망 통제와 유사”**이다. **“실차 ECU의 패키지 설치 동작을 재현”**하거나 특정 OEM의 승인된 생산 toolchain과 동등하다고 주장하지 않는다.

## 목표

- Boost 패키지명, 버전, 아키텍처, 파일명, 크기와 SHA-256을 버전 관리되는 manifest에 고정한다.
- APT의 서명된 저장소 검증과 별도로 프로젝트 manifest를 검증한다.
- 다운로드 및 추출을 재사용 디렉터리와 분리된 staging 디렉터리에서 수행한다.
- manifest에 없는 패키지, 누락 패키지, checksum·크기·Debian metadata 불일치를 모두 거부한다.
- 검증된 manifest 순서의 파일만 새 Boost root에 추출하고, 성공한 결과만 교체한다.
- checksum 불일치와 stale `.deb` 혼입을 자동 회귀 시험으로 고정한다.

## 비목표와 주장 경계

- Yocto/BitBake, OEM artifact repository, HSM-backed signing 또는 전체 SLSA 체계로 이전하지 않는다.
- Ubuntu archive·서명 키 자체가 침해된 경우까지 이 manifest 하나로 해결했다고 주장하지 않는다.
- 고정된 패키지가 원격 archive에서 제거될 때의 장기 가용성은 보장하지 않는다. 양산 환경에서는 승인된 내부 artifact repository에 같은 digest의 패키지를 보존해야 한다.
- Boost 추출물 전체의 런타임 무결성 감시를 추가하지 않는다. 빌드 준비 시 검증된 패키지에서 새로 추출하는 방식으로 경계를 닫는다.

## 검토한 접근

1. **선택: 고정 manifest와 격리 staging.** 현재 Ubuntu/CMake 구조를 보존하면서 양산형 입력 승인 경계를 구현할 수 있다.
2. **Yocto/BitBake 이전.** Automotive Grade Linux와 유사하지만 전체 빌드 아키텍처 변경이어서 Issue #7 범위를 벗어난다.
3. **`.deb` 저장소 포함.** 오프라인 재현성은 높지만 바이너리 저장소 비대화와 업데이트 관리 부담 때문에 선택하지 않는다.

## 실차 유사성의 근거와 적용 범위

### 규정·가이드 근거

- UN R155 누적본 5.1.1(a)는 공급망을 통해 필요한 정보를 수집·검증하고 supplier-related risk를 식별·관리하도록 요구한다.
- UN R155 누적본 7.2.2.5는 계약 공급자, 서비스 제공자 및 제조사 하위 조직과의 의존성을 CSMS가 관리하도록 요구한다.
- UN R155 누적본 7.3.2는 승인 대상 차량 형식의 supplier-related risk를 식별·관리하도록 요구한다. Amendment 1~3은 이 공급망 조항을 제거하거나 완화하지 않는다.
- `docs/check/ISOSAE_21434_기반_자동차_사이버보안_테스트_및_검증_매뉴얼.pdf` 3-3-2(22쪽)는 공급망 공격 예방을 위해 오픈소스 라이브러리의 출처 및 서명을 검증하도록 제시한다.
- UN R156 7.2.1.1의 software update authenticity·integrity 보호는 차량 배포 단계의 후단 통제다. 빌드 전에 오염된 의존성이 정상 서명되는 위험은 별도의 빌드 입력 통제가 필요하므로, R156만으로 Issue #7이 해결된다고 보지 않는다.

### 공개 자동차 구현 근거

- Automotive Grade Linux는 Yocto Project/OpenEmbedded 호환 layer로 차량용 Linux 배포판을 빌드한다. AGL 문서: <https://docs.automotivelinux.org/en/master/04_Developer_Guides/02_AGL_Platform_Development/01_AGL_Yocto_Layers/01_Overview/>
- Yocto는 remote archive마다 recipe에 사전 SHA-256을 두고, 누락되거나 불일치한 checksum을 build error로 처리하며, recipe별 임시 `WORKDIR`에서 fetch·unpack·build를 수행한다. Yocto 문서: <https://docs.yoctoproject.org/dev-manual/new-recipe.html#fetching-code>
- 이 설계의 version-controlled manifest는 Yocto recipe의 승인 checksum에, 고유 staging과 새 root 생성은 recipe별 격리 work directory에 대응한다. 전체 Yocto toolchain을 모사하는 것이 아니라 Issue #7과 직접 연결된 공급망 통제만 이식한다.

### Ubuntu 패키지 신뢰 체인

- APT는 서명된 Release/Packages metadata에서 package digest를 받아 다운로드 파일을 자동 검증한다. Ubuntu `apt-secure` 문서: <https://manpages.ubuntu.com/manpages/noble/man8/apt-secure.8.html>
- 프로젝트 manifest는 APT를 대체하지 않는다. APT가 제공하는 archive authenticity 위에, 이 프로젝트가 승인한 package identity와 exact bytes를 별도 고정하는 두 번째 통제다.

### 통제 매핑

| 설계 통제 | 양산 환경에서 대응하는 개념 | 근거 | SIL 주장 경계 |
| --- | --- | --- | --- |
| Git 고정 package/version/architecture/SHA-256 | 승인된 third-party component와 immutable build input | R155 5.1.1(a), 7.3.2; KISA 매뉴얼 3-3-2; Yocto `SRC_URI[sha256sum]` | OEM 승인 목록이나 SBOM 전체를 대신하지 않음 |
| APT archive 인증 + project manifest 이중 검증 | upstream repository trust와 product-specific allowlist 분리 | Ubuntu `apt-secure`; Yocto checksum | 내부 OEM artifact repository·PKI는 구현하지 않음 |
| 고유 staging과 exact-set 검사 | clean/hermetic build workspace, undeclared input 차단 | Yocto recipe별 `WORKDIR` | 완전한 hermetic build나 container 격리를 주장하지 않음 |
| 검증된 명시 목록만 추출 | declared dependency만 build graph에 진입 | R155 supplier dependency 관리; Yocto recipe input | 전체 transitive dependency graph 검증은 별도 범위 |
| 새 root 생성 후 성공 시 교체 | 실패한 준비 단계가 승인된 build sysroot를 오염시키지 않음 | 재현 가능한 build와 fail-closed 공급망 통제 | 전원 손실 내구성이나 filesystem rollback 보장은 아님 |
| checksum·stale package 실패 시험 | 공급망 통제의 verification evidence | R155 7.3.6 테스트 및 Annex 1 9.7 증거 | 형식승인 시험이나 OEM CSMS 인증 증거는 아님 |

이 매핑 때문에 선택한 구조는 현재 프로젝트에서 지원 가능한 범위 내에서 실차 지향적이다. 반대로 기존의 다운로드 후 checksum 생성과 wildcard 추출은 승인 전 입력과 승인되지 않은 잔여 입력을 구분하지 못하므로 이 근거에 부합하지 않는다.

## 구조

- `ECU/config/dependencies/boost-debs.manifest`: 탭 구분 고정 manifest. 각 행은 `package`, `version`, `architecture`, `filename`, `size`, `sha256`을 가진다. 주석과 빈 줄만 부가적으로 허용하며 중복 필드나 잘못된 형식은 거부한다.
- `ECU/scripts/setup/verify-boost-packages.py`: manifest와 지정된 패키지 디렉터리를 검증하는 표준 라이브러리 전용 도구. 예상 파일 집합의 완전 일치, 파일 크기와 SHA-256, `dpkg-deb -f`의 Package/Version/Architecture를 확인한다.
- `ECU/scripts/setup/build-vsomeip.sh`: 실제 구현 위치. 고유 staging 디렉터리에 명시적 package spec을 다운로드하고 검증 도구를 통과한 파일만 새 Boost root에 추출한다. 실패 시 기존 설치를 보존하고 staging을 정리한다.
- `scripts/setup/build-vsomeip.sh`: 기존 사용자와 자동화 호환을 위한 얇은 실행 위임자만 유지한다.
- `ECU/tests/setup/test_boost_package_integrity.py`: 실제 검증 도구를 임시 fixture에 실행해 정상 입력, checksum 변조, 추가 `.deb`, 누락 파일, metadata 불일치를 검증한다.

## 보안 불변조건과 데이터 흐름

공격자 또는 비정상 상태가 제어할 수 있는 입력은 APT가 내려받은 파일, 이전 실행에서 남은 파일, 로컬 download cache의 파일이다. 보안 sink는 `dpkg-deb -x`이다.

`dpkg-deb -x`에 도달하는 모든 파일은 다음 조건을 모두 만족해야 한다.

1. Git에 고정된 manifest의 정확한 파일명이다.
2. 디렉터리의 `.deb` 집합이 manifest 집합과 정확히 일치한다.
3. 파일 크기와 SHA-256이 manifest와 일치한다.
4. Debian control metadata의 package, version, architecture가 manifest와 일치한다.
5. 추출 loop는 glob이 아니라 검증 완료된 manifest 목록만 사용한다.

어느 조건이든 실패하면 추출과 설치 교체 전에 비정상 종료한다.

## 정상 동작과 호환성

- 기존 명령 `scripts/setup/build-vsomeip.sh`와 `--status`는 유지한다.
- Boost 1.90.0-6ubuntu1과 vSomeIP 3.7.6 고정값은 변경하지 않는다.
- 기존 non-root 실행, 백그라운드 빌드, CPU·메모리 제한 동작은 유지한다.
- 이미 존재하는 `third_party/debs`는 신뢰 입력으로 재사용하지 않는다. 새 staging에서 검증한 패키지만 사용한다.
- 기존 `boost-root`는 새 추출물이 완전히 검증·생성되기 전까지 변경하지 않는다.
- `dpkg-deb`가 header timestamp를 보존하므로 vSomeIP은 `--clean-first`로 재컴파일하여 검증 전 입력에서 생성된 object가 재사용되지 않게 한다. 매 실행 전체 컴파일 비용을 허용한다.

## 시험과 완료 조건

- 회귀 시험은 기존 구현에서 checksum 변조와 stale `.deb`가 거부되지 않아 RED가 되어야 한다.
- 구현 후 정상 5개 패키지는 통과하고 다음 입력은 모두 비정상 종료해야 한다: checksum 불일치, 크기 불일치, 추가 `.deb`, 누락 `.deb`, package/version/architecture 불일치, malformed·중복 manifest.
- shell syntax 검사, Python 시험, 실제 현재 패키지 검증, 관련 기존 시험과 전체 repository 시험을 실행한다.
- `project.md`에 R155 5.1.1(a), 7.2.2.5, 7.3.2와 KISA 매뉴얼 3-3-2를 근거로 기록하고, 내부 artifact repository와 서명된 build provenance는 SIL 밖의 양산 경계로 명시한다.
