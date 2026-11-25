# 원격 접속과 보안

서버 기본 바인딩은 loopback입니다. Fastboot TCP 자체는 평문이며, 열린 포트에 연결할 수 있는 피어는 기기가 허용하는 Flash, Erase 등의 명령을 요청할 수 있습니다. 인터넷에 포트를 직접 게시하지 마세요. LAN이나 VPN에서는 IP Allowlist와 호스트 방화벽을 함께 적용합니다. 프록시는 부트로더 잠금 정책을 변경하지 않습니다.

다음은 관리자 PowerShell에서 허용된 개발 PC 한 대만 연결하도록 하는 예입니다. 환경에 맞는 인터페이스 프로필과 원격 주소를 지정하세요.

```powershell
New-NetFirewallRule -DisplayName "Remote Fastboot Proxy 5554" -Direction Inbound `
  -Action Allow -Protocol TCP -LocalPort 5554 -RemoteAddress 192.168.0.42 -Profile Private
```

Windows 호스트에 SSH 서버가 운영 중이면 프록시를 loopback에 둔 채 SSH 포워딩을 사용할 수 있습니다.

```sh
ssh -N -L 127.0.0.1:15554:127.0.0.1:5554 -L 127.0.0.1:15037:127.0.0.1:5037 user@windows-host
fastboot -s tcp:127.0.0.1:15554 getvar product
adb -H 127.0.0.1 -P 15037 devices
```

VPN에서는 `--bind`에 Windows의 VPN IPv4 주소를 지정하고 허용 피어만 설정합니다. 장치별 TCP 포트는 독립적이며, 클라이언트가 포트를 선택합니다. 로그에는 이미지 바이트와 응답 본문을 남기지 않습니다. DEBUG에는 명령 텍스트와 응답 코드가 포함되므로 공유 전 OEM 명령의 인수를 확인하세요. 파일 로테이션은 외부 로그 관리 도구에서 처리할 수 있습니다.

## ADB 서버 공존

같은 Windows PC에서 기존 ADB 서버를 그대로 사용합니다. SSH 예시는 기본 loopback의 ADB 서버 `5037`에도 접근합니다. 원격 PC가 직접 LAN으로 ADB 서버를 사용해야 하고 이미 해당 구성이 승인된 환경이면 Windows ADB의 `adb -a nodaemon server`로 LAN 리스닝을 구성하고 TCP 5037에도 별도 방화벽 제한을 적용할 수 있습니다. 기존 로컬 ADB 서버가 같은 포트를 쓰고 있으면 운영자 조율 후 실행합니다.

```sh
adb -H 192.168.0.100 -P 5037 devices
adb -H 192.168.0.100 -P 5037 -s ANDROID_SERIAL reboot bootloader
fastboot -s tcp:192.168.0.100:5554 getvar product
```

Android 모드의 ADB 시리얼과 Fastboot USB 시리얼은 기기마다 다를 수 있습니다. 리스너 기본 포트 5554 및 인접 포트는 Android Emulator 콘솔이나 ADB 연결에 사용될 수 있습니다. 충돌하면 `doctor --port 15554`로 검사한 뒤 매핑 변경을 고려하세요. [ADB 공식 문서](https://developer.android.com/tools/adb) · [Windows 방화벽 PowerShell](https://learn.microsoft.com/en-us/powershell/module/netsecurity/new-netfirewallrule)
