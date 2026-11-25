# Windows USB 드라이버 분석

Fastboot 인터페이스의 클래스/서브클래스/프로토콜은 `ff/42/03`입니다. ADB 인터페이스 `ff/42/01`은 탐색 및 열기 대상에서 제외합니다. Windows USB 장치의 VID/PID는 인벤토리와 검증에 사용하며 제조사별 허용 목록으로 사용하지 않습니다.

| 백엔드 | 드라이버와 호환성 | 배포 및 선택 이유 |
| --- | --- | --- |
| AOSP AdbWinApi | AOSP Windows `fastboot.exe`가 Android USB 인터페이스 GUID와 기본 Bulk endpoint를 사용합니다. 이 경로에 의존하는 OEM 구성이 있을 수 있습니다. | `AdbWinApi.dll`과 WinUSB 구현의 `AdbWinUsbApi.dll`을 함께 고려해야 합니다. AOSP 소스의 고지와 Google 바이너리 배포 약관은 별도입니다. |
| SetupAPI + WinUSB | Google Android Bootloader Interface처럼 `WinUSB` 서비스에 바인딩되고 디바이스 인터페이스를 게시하는 드라이버를 사용합니다. 호환 OEM GUID도 장치 레지스트리에서 찾습니다. | **선택.** 시스템 DLL과 기존 드라이버를 사용합니다. USB 위치·디바이스 인스턴스를 직접 얻고, Bulk I/O를 취소할 수 있습니다. |
| libusb | WinUSB, libusbK 계열, UsbDk 등 Windows 백엔드의 영향을 받습니다. 지원되지 않는 OEM 드라이버는 libusb 추가만으로 호환되지 않습니다. | LGPL 2.1 이상을 검토해야 하며 의존성 및 배포 경로가 추가됩니다. 백엔드 대안으로 남겨 두었습니다. |

Google USB Driver r13의 `android_winusb.inf`를 확인했습니다. Android 설치 클래스 GUID는 `{3F966BD9-FA04-4EC5-991C-D326973B5128}`, 장치 인터페이스 GUID는 `{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}`입니다. Windows의 일반 USB 인터페이스 GUID도 탐색에 포함합니다. AOSP `platform/development` 커밋 `88f7870b2c652922dc2afb01c076737e8bc50130`의 `ANDROID_USB_CLASS_ID`와 Windows `fastboot` 빌드의 DLL 연결을 확인했습니다. [Google USB Driver](https://developer.android.com/studio/run/win-usb) · [OEM USB Driver](https://developer.android.com/studio/run/oem-usb) · [AOSP Windows API](https://android.googlesource.com/platform/development/+/88f7870b2c652922dc2afb01c076737e8bc50130/host/windows/usb/api/adb_api.h) · [libusb Windows FAQ](https://github.com/libusb/libusb/wiki/FAQ)

SetupAPI에서 `USB\Class_FF&SubClass_42&Prot_03` 호환 ID를 찾고, 드라이버 서비스와 프로퍼티를 읽습니다. 복합 USB 기기는 가능한 경우 물리 부모의 시리얼과 `DEVPKEY_Device_LocationPaths`를 사용합니다. `CM_DEVCAP_UNIQUEID`가 있는 USB 인스턴스만 시리얼 근거로 사용합니다. 장치를 열 때 WinUSB 디스크립터에서 VID/PID, USB 시리얼, 인터페이스 클래스, Bulk endpoint를 추가 검사합니다. 실제 시리얼과 Windows 인스턴스가 서로 일치하지 않으면 열기를 거부할 수 있습니다.

파일 핸들은 `FILE_FLAG_OVERLAPPED`와 공유 모드 0으로 엽니다. 다른 로컬 Fastboot가 이미 디바이스를 열었으면 먼저 해당 세션을 닫으세요. 일반 사용자 실행 여부는 설치된 드라이버의 접근 제어에 달려 있습니다. 초기 드라이버 설치와 방화벽 구성에는 관리자 권한이 필요할 수 있습니다. 프록시는 드라이버나 시스템 설정을 수정하지 않습니다. [WinUSB 초기화](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_initialize) · [WinUSB 접근](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/using-winusb-api-to-communicate-with-a-usb-device) · [파이프 정책](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-functions-for-pipe-policy-modification)

관련 구현으로 [U-Boot TCP Fastboot](https://android.googlesource.com/platform/external/u-boot/+/refs/heads/main/net/fastboot_tcp.c)는 디바이스에서 Fastboot TCP를 구현하며 [usbipd-win](https://github.com/dorssel/usbipd-win)은 Windows에서 USB/IP로 USB 장치 자체를 공유합니다. 여기서는 순정 Fastboot TCP 포트와 Windows 로컬 USB 드라이버를 유지합니다. 두 구현의 코드는 포함하지 않습니다.
