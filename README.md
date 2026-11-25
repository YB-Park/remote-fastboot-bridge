# Remote Fastboot Proxy

Windows에 USB로 연결된 Android 부트로더와 fastbootd를 다른 PC의 순정 Google Platform Tools `fastboot`로 제어합니다. 서버는 Fastboot TCP v1과 WinUSB Bulk IN/OUT 사이에서 명령, 응답, 바이너리를 전달합니다.

## 시작하기

Windows 10/11 x64 PC에 대상 기기의 Google 또는 OEM Android Bootloader Interface 드라이버를 설치합니다. 부트로더 모드와 fastbootd에서 드라이버가 각각 인식되는지 확인합니다. 실행 파일은 압축을 풀어 사용할 수 있습니다. 소스 빌드는 [빌드 및 배포](docs/BUILD.md)를 참고하세요.

```powershell
.\fastboot-proxy.exe --version
.\fastboot-proxy.exe devices
.\fastboot-proxy.exe doctor
.\fastboot-proxy.exe serve --serial SERIAL_A --port 5554
```

`devices`에 나온 USB 시리얼을 사용합니다. 기본 바인딩은 `127.0.0.1:5554`입니다. 로컬 호스트에서 먼저 `fastboot -s tcp:127.0.0.1:5554 getvar product`를 실행하세요. 신뢰하는 LAN에서 특정 원격 PC만 허용하는 예는 다음과 같습니다.

```powershell
.\fastboot-proxy.exe serve --serial SERIAL_A --port 5554 --bind 0.0.0.0 --allow 192.168.0.42
```

원격 Ubuntu 또는 macOS에서 실행합니다. 원격 Windows에서는 같은 구문으로 `fastboot.exe`를 사용합니다.

```sh
fastboot -s tcp:192.168.0.100:5554 getvar product
fastboot -s tcp:192.168.0.100:5554 getvar all
fastboot -s tcp:192.168.0.100:5554 reboot bootloader
```

Fastboot TCP에는 인증과 암호화가 없습니다. SSH 터널, VPN, 방화벽 규칙은 [원격 접속과 보안](docs/SECURITY.md)을 참고하세요.

## 여러 디바이스 설정

`config.example.json`을 복사해 포트와 시리얼을 지정한 뒤 `fastboot-proxy.exe serve --config config.json`으로 시작합니다. 포트와 디바이스당 활성 세션은 1개이며, 서로 다른 기기는 동시에 사용할 수 있습니다. `max_sessions`는 전체 동시 세션 수입니다.

| 설정 | 의미 및 기본값 |
| --- | --- |
| `listen_address`, `allowlist` | 숫자 IPv4 바인딩 `127.0.0.1`. Allowlist는 IPv4 또는 CIDR 목록이며 기본값은 빈 목록입니다. |
| `usb_backend` | `auto`는 Windows에서 WinUSB 선택. 개발 빌드는 loopback 전용 `mock`도 지원합니다. |
| `devices` | 정적 매핑 1~32개. 각 항목에는 `serial` 또는 `instance`와 고유한 `port`가 필요합니다. 포트 기본값은 5554입니다. |
| `location`, `serial_aliases` | 중복 시리얼은 `location`으로 구분. 모드별 시리얼 별칭은 동일한 `location`과 함께 명시합니다. |
| `reconnect_timeout_ms` | 알려진 모드 전환에서 기존 USB 분리를 기다리는 시간. 기본 60000ms. |
| `session_timeout_ms`, `io_timeout_ms`, `usb_timeout_ms` | 명령 사이 TCP 유휴, TCP I/O, USB I/O 제한. 기본 60000/30000/90000ms. |
| `max_sessions`, `log_level`, `log_file` | 동시 세션 기본 8개, 로그 기본 `INFO`, 선택적 파일 경로. 상대 로그 경로는 실행 디렉터리 기준입니다. |

포트 매핑은 설정 파일에 유지됩니다. 동일한 시리얼이 부트로더와 fastbootd에서 달라지는 경우, 두 모드에서 `devices`를 실행해 물리 위치와 시리얼을 확인한 후 다음과 같이 기록합니다.

```json
{
  "devices": [
    {
      "serial": "BOOTLOADER_SERIAL",
      "serial_aliases": ["FASTBOOTD_SERIAL"],
      "location": "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(2)",
      "port": 5554
    }
  ]
}
```

시리얼이 없으면 `--instance` 또는 `instance`로 현재 인터페이스를 지정합니다. 분리 이후에는 기기 식별을 다시 확인하고 서버를 재시작해야 합니다. Windows 측 장치 목록은 `fastboot-proxy.exe devices`에서 확인합니다. 클라이언트의 `fastboot devices`는 TCP 포트 스캔 명령이 아닙니다.

## 문서

- [아키텍처 및 개발 단계](docs/ARCHITECTURE.md)
- [AOSP 프로토콜 분석과 명령 호환성](docs/PROTOCOL.md)
- [Windows USB 드라이버와 백엔드 분석](docs/WINDOWS_USB.md)
- [빌드 및 배포](docs/BUILD.md)
- [원격 접속과 보안, ADB 공존](docs/SECURITY.md)
- [문제 해결](docs/TROUBLESHOOTING.md)
- [실기기 테스트 계획](docs/TEST_PLAN.md)
- [검증 결과 및 성능](docs/TEST_RESULTS.md)
- [제약사항과 후속 검증](docs/LIMITATIONS.md)
- [라이선스](LICENSE) · [외부 구성 요소 고지](THIRD_PARTY_NOTICES.md)
