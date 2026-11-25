# Third-party notices

프로젝트 소스는 Apache License 2.0으로 제공합니다.

| 구성 요소 | 사용 범위와 라이선스 |
| --- | --- |
| Android Open Source Project Fastboot | `platform/system/core` 커밋 `a3b721a32242006b59cb12bd62c9133632af3a2d`. 조사한 `tcp.cpp`, `usb_windows.cpp` 등은 두 조항 BSD 고지, `transport.h`, `constants.h`, Android 사용자 공간 구현 등에는 Apache-2.0 고지가 있습니다. 코드 복사 없이 프로토콜 동작을 참고했습니다. [Fastboot 소스 및 파일별 고지](https://android.googlesource.com/platform/system/core/+/a3b721a32242006b59cb12bd62c9133632af3a2d/fastboot/) |
| AdbWinApi와 Google USB Driver | AOSP 개발 도구와 Google USB Driver INF를 조사했습니다. DLL과 드라이버는 이 프로젝트 및 압축에 포함하지 않습니다. [AOSP USB 호스트 NOTICE](https://android.googlesource.com/platform/development/+/88f7870b2c652922dc2afb01c076737e8bc50130/host/windows/usb/NOTICE) |
| Google Platform Tools 37.0.3 | 통합 테스트에 Google 배포 바이너리를 따로 받습니다. Android SDK 라이선스 조건이 적용되며 프로젝트 패키지에 포함하지 않습니다. [Google 약관 및 다운로드](https://developer.android.com/tools/releases/platform-tools) |
| WinUSB, SetupAPI, CfgMgr32, Winsock2 | Windows에 설치된 시스템 구성 요소로, DLL 또는 Windows SDK를 재배포하지 않습니다. |
| MSVC 정적 런타임 | MSVC로 빌드한 EXE의 일부로 포함할 수 있으며 Visual Studio의 배포 가능 코드 약관이 적용됩니다. [Microsoft 재배포 안내](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files) |
| Zig 교차 빌드 런타임 | 교차 빌드 ZIP에는 LLVM libc++/libc++abi/unwind 및 MinGW-w64 구성 요소의 고지와 Zig 라이선스 사본을 동봉합니다. [Zig 0.14.1](https://github.com/ziglang/zig/tree/0.14.1) |
| 비교 조사 | libusb는 LGPL 2.1 이상, U-Boot는 GPL-2.0 계열입니다. libusb, U-Boot, usbipd-win 코드는 패키지에 포함하지 않습니다. |

GitHub Actions의 Checkout, Setup Python, Upload Artifact는 CI 실행 시 사용하는 도구입니다.
