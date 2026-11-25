# AOSP Fastboot 프로토콜 분석

조사 기준은 AOSP `platform/system/core` main 커밋 `a3b721a32242006b59cb12bd62c9133632af3a2d`입니다. 실제 호스트 동작은 순정 Linux Platform Tools `37.0.3-16887417`을 사용해 별도로 확인했습니다. 파일마다 Apache-2.0 또는 BSD 계열 고지가 있습니다. 프록시 코드는 별도 구현입니다. [고정 커밋의 Fastboot 트리](https://android.googlesource.com/platform/system/core/+/a3b721a32242006b59cb12bd62c9133632af3a2d/fastboot/) · [라이선스 기록](../THIRD_PARTY_NOTICES.md)

| 조사 파일 | 구현에 반영한 동작 |
| --- | --- |
| `README.md`, `constants.h` | 명령 최대 4096, 상태 최대 256바이트, 4바이트 상태 코드. 최신 명세의 `TEXT`는 문자열을 원문 형식으로 표시합니다. |
| `tcp.cpp`, `tcp.h`, `tcp_test.cpp` | 기본 포트 5554, `FB01` 및 2초 클라이언트 응답 대기. 프레임 길이는 8바이트 부호 없는 big-endian이며 일부만 읽은 프레임의 잔여 길이를 기억합니다. |
| `socket.cpp`, `transport.h` | TCP는 연속 스트림이므로 정확한 길이를 반복 읽기. 기본 `WaitForDisconnect()`는 바로 반환하며 TCP가 재정의하지 않습니다. |
| `fastboot_driver.cpp/.h` | `INFO`와 `TEXT` 연속 응답 허용. `fetch`와 `upload`의 `ReadBuffer`는 요청 길이와 반환 길이가 같아야 하며 최대 1MiB씩 요청합니다. |
| `fastboot.cpp`, `task.cpp` | 순정 CLI는 `-s tcp:host:port`를 파싱하고 fastbootd 전환 시 현재 Transport를 닫고 재접속한 뒤 `is-userspace`를 재확인합니다. `update`와 `flashall`은 이 경로를 사용합니다. |
| `usb.h`, `usb_windows.cpp`, `Android.bp` | 클래스 `ff/42/03`와 두 Bulk endpoint. AOSP Windows 클라이언트는 AdbWinApi로 열고 읽기 short packet을 경계로 사용합니다. |
| `device/usb.cpp`, `device/fastboot_device.cpp`, `device/commands.cpp` | Android 사용자 공간 fastbootd는 FunctionFS를 사용합니다. AOSP fastbootd는 명령을 256바이트 버퍼로 읽고, `fetch`에서 DATA → 파티션 데이터 → 최종 상태를 씁니다. |

## TCP와 USB 경계

1. 서버는 클라이언트의 4바이트 버전 문자열을 검증하고 USB가 준비되면 `FB01`을 보냅니다. `FB01` 이상의 상대는 v1으로 협상하며, `FB00`과 형식 오류는 연결을 종료합니다. 순정 호스트는 먼저 자신이 아는 최대 버전을 보냅니다.
2. 명령은 길이 헤더를 제거하고 USB Bulk OUT에 한 번 씁니다. TCP 단편화와 연속 프레임을 별도로 처리합니다. 현재 서버 명령 한도는 4096바이트이며, 256바이트 등 더 작은 실기기 한도가 적용될 수 있습니다.
3. 상태 단계에서는 USB Bulk IN 한 번의 응답을 TCP 프레임 하나로 반환합니다. `INFO`와 `TEXT`는 여러 번 전달할 수 있습니다. `OKAY`와 `FAIL`은 수신한 내용을 그대로 전달합니다. 종료 표식을 문자열 검색으로 찾아 프레임을 분리하지 않습니다.
4. `DATA`와 정확히 8개 16진 문자를 검증합니다. `download:`는 호스트에서 USB로, `upload`와 `fetch:`는 USB에서 호스트로 전달합니다. 다운로드는 TCP 프레임을 합쳐 USB 패킷 배수의 64 KiB 버퍼를 채웁니다. 마지막 쓰기에서만 남은 크기를 사용합니다. 업로드는 DATA 길이 전체를 한 TCP 프레임 헤더에 기록하고 64 KiB씩 흘려보냅니다. TCP 클라이언트의 여러 읽기가 같은 프레임 내용을 이어 읽습니다.
5. DATA 크기만큼 바이너리를 이동한 후 최종 상태 단계로 돌아갑니다. 다운로드 길이 초과, 비정상 DATA, 확인 불가능한 USB 쓰기, USB 시간 초과는 TCP를 닫고 격리합니다. 데이터의 바이트 값은 상태 파싱에 사용하지 않습니다.

WinUSB `IGNORE_SHORT_PACKETS`는 끄고 USB Short Packet이 IN 읽기를 끝내도록 합니다. DATA 단계의 짧은 IN 읽기는 남은 길이만큼 이어서 읽습니다. 0바이트 USB 읽기는 연속 최대 7개까지 건너뜁니다. 명령의 짧은 OUT은 결과 불명으로 처리하며, DATA의 성공한 짧은 OUT은 완료 바이트 수가 USB endpoint 패킷 배수일 때에만 나머지를 이어 씁니다. 프록시는 각 Bulk OUT 끝에 ZLP를 강제로 넣지 않습니다. USB 응답을 논리적으로 구분해서 보내는 기기가 필요합니다.

TCP 길이는 64비트입니다. 단일 DATA 최대값은 8자리 16진수인 `0xffffffff`입니다. AOSP 호스트의 fetch/upload 경로는 부호 있는 `int` 크기 변수를 확인하므로 실용적인 단일 수신 요청은 `0x7fffffff` 이하로 유지해야 합니다. 기기 최대값은 이보다 작을 수 있습니다. 순정 클라이언트의 sparse 변환과 분할을 사용하면 전체 파일은 단일 DATA 제한을 초과할 수 있습니다.

## 명령과 버전

| 명령 | 서버에서의 처리와 검증 |
| --- | --- |
| `getvar product/all/serialno/current-slot/is-userspace/max-download-size` | 실제 디바이스에 전달. 순정 Linux 클라이언트로 기본 조회와 연속 응답을 검증했습니다. |
| `flash`, `erase`, `boot`, `set_active`, `oem`, `flashing` | 명령과 상태를 전달하며 실제 기기 정책을 적용받습니다. Mock에서는 합성 파티션 flash만 실행했습니다. OEM DATA 방향을 알 수 없는 경우 세션을 종료합니다. |
| `reboot`, `reboot bootloader`, `reboot fastboot` | 실제 최종 응답을 전달하고 USB 분리 뒤 새 TCP 세션 사용. Mock에서 부트로더 ↔ fastbootd 왕복을 확인했습니다. |
| `fetch`, `stage`, `get_staged` | AOSP의 DATA 업로드·다운로드 경로. 순정 Linux 클라이언트와 Mock으로 해시를 확인했습니다. |
| `update`, `flashall` | 순정 CLI가 수행하는 복합 동작. 합성 이미지와 Mock에서 fastbootd 재접속, sparse 전송, 부트로더 복귀를 확인했습니다. |

AOSP `android-7.1.2_r39`, `android-10.0.0_r47`, `android-13.0.0_r1` 소스에는 명시적 TCP 주소 지정이 있으며 네트워크 장치 저장을 수행하는 `connect` 구현은 없습니다. `android-15.0.0_r1`, `android-16.0.0_r1` 및 조사한 main에는 `connect`가 주소를 로컬에 기록하고 `devices`가 등록된 주소만 프로빙하는 코드가 있습니다. Google 배포 버전과 Android 태그의 기능 포함 시점은 별개입니다. 실제 사용 예에서는 `-s tcp:...`를 사용합니다. [AOSP 태그 목록](https://android.googlesource.com/platform/system/core/+refs) · [Platform Tools 릴리스](https://developer.android.com/tools/releases/platform-tools)
