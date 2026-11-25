# 빌드 및 배포

Windows 10/11 x64에 Visual Studio 2022 또는 Build Tools 2022의 **Desktop development with C++** 워크로드와 Windows SDK, CMake 3.21 이상을 설치합니다. 개발 테스트 빌드에는 Python 3이 필요합니다. 순정 클라이언트 테스트는 Python 3.11 이상을 사용합니다. 실행 파일 빌드에 외부 라이브러리 다운로드는 필요하지 않습니다.

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\test.ps1 -Configuration Debug -FastbootClient C:\Android\platform-tools\fastboot.exe
.\scripts\package.ps1
```

실행 파일은 `out\win-x64\Release\fastboot-proxy.exe`에 생성됩니다. 패키징 명령은 Mock과 테스트를 제외한 Release를 빌드해 `dist\fastboot-proxy-win-x64.zip`에 담습니다. MSVC 빌드는 정적 C++ 런타임을 사용합니다. WinUSB와 Winsock 등 Windows 시스템 DLL은 운영체제가 제공합니다.

CMake Preset을 직접 사용하는 방법입니다.

```powershell
cmake --preset win-x64 -DFASTBOOT_CLIENT=C:/Android/platform-tools/fastboot.exe
cmake --build --preset win-x64-release --parallel
ctest --preset win-x64-release
```

GitHub Actions는 Windows Server 2022 x64의 Visual Studio 2022에서 Debug와 Release를 빌드합니다. 테스트에 사용하는 공식 Platform Tools `37.0.3` ZIP은 SHA-256을 확인하며 패키지에 포함하지 않습니다. 테스트가 모두 통과하면 Mock이 빠진 EXE와 문서를 ZIP artifact로 올립니다.

Linux에서도 프로토콜과 Mock 테스트를 실행할 수 있습니다. 이 빌드는 Windows USB 백엔드를 만들지 않습니다.

```sh
cmake -S . -B out/linux -DCMAKE_BUILD_TYPE=Release -DFASTBOOT_CLIENT=/opt/platform-tools/fastboot
cmake --build out/linux --parallel
ctest --test-dir out/linux --output-on-failure
python3 tests/benchmark.py out/linux/fastboot-proxy
```

교차 빌드 배포 ZIP은 Zig 0.14.1의 Clang/MinGW와 CMake 3.31에서 빌드한 PE x86-64입니다. MSVC 실행 파일은 CI 또는 로컬 PowerShell 빌드로 별도 생성할 수 있습니다. [Windows SDK 요구 사항](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_initialize) · [Google Platform Tools](https://developer.android.com/tools/releases/platform-tools)
