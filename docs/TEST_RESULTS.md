# 검증 결과

| 환경 | 결과 |
| --- | --- |
| Linux x86-64, GCC 14.2 Release | CTest 3/3 통과. 단위, Mock TCP 통합, Google 순정 fastboot 통합. |
| 순정 Linux fastboot 37.0.3-16887417 + Mock | 명시적 TCP 연결, `getvar`, 여러 INFO와 TEXT, 기기 FAIL 전달, `stage`, `get_staged`, `fetch`, 재부팅 및 재접속, 합성 sparse 이미지를 사용한 `update`와 `flashall` 확인. |
| 자체 TCP 클라이언트 + Mock | TCP 바이트 단편화, 연속 프레임, 버전 협상·거부, 64비트 초과/0 프레임 거부, ZLP, USB 부분 I/O, 동시 요청, USB 모의 분리, 재시작, 시간 초과, 중도 단절 격리, 동일 기기의 중복 포트 및 중복 시리얼 확인. |
| 데이터 비교 | Download는 Mock에서 계산한 FNV-1a 64와 클라이언트 원본을 비교했습니다. Upload와 다중 범위 fetch는 SHA-256으로 비교했습니다. |
| Windows x64 PE | Zig 0.14.1 Clang/MinGW, CMake 3.31.10 Release 교차 빌드 및 PE 헤더·DLL import 검사 통과. |
| VS 2022 MSVC | Debug 및 Release Windows CI와 ZIP 작업이 포함됩니다. 실행 상태는 저장소 Actions를 확인하세요. |
| 실제 USB·Windows 하드웨어 | 이 보고서의 Mock 및 파일 검사는 실제 장치 검증을 대체하지 않습니다. 실기기 검증, OEM 드라이버 검증, macOS 원격 바이너리 실험, 실제 Flash·Erase·Unlock은 수행하지 않았습니다. |

사용한 Google Linux Platform Tools ZIP SHA-256은 `214548146cadb2ca3508143d8a1d4be4c59de5bb837253e3ede89ed74cf450d1`입니다. 순정 CLI의 `getvar`는 기기 FAIL을 출력해도 37.0.3에서 프로세스 종료 코드 0을 반환할 수 있어 테스트는 실제 출력도 확인합니다.

## Linux loopback Mock 벤치마크

Linux Release 프록시, TCP loopback, Mock USB OUT에서 측정했습니다. 아래 처리량은 TCP와 Mock의 합산 경로입니다. USB 버스 처리량이나 물리 기기 처리량은 아직 측정하지 않았습니다.

| 전송량 | 경과 | 처리량 | 프로세스 RSS | CPU, 논리 코어 하나 기준 |
| --- | ---: | ---: | ---: | ---: |
| 1 MiB | 0.004 s | 252.9 MiB/s | 2400 KiB | 샘플 해상도 미만 |
| 100 MiB | 0.177 s | 564.2 MiB/s | 2412 KiB | 90.3% |
| 1 GiB | 1.572 s | 651.3 MiB/s | 2412 KiB | 99.9% |
| 4 GiB, 2 GiB × 2 DATA | 6.373 s | 642.7 MiB/s | 2412 KiB | 97.9% |
| 두 기기 동시 100 MiB씩 | 0.167 s | 1196.7 MiB/s 합계 | 미측정 | 미측정 |

Getvar RTT 200회 중앙값 20.2µs, p95 43.3µs. 대기 RSS 2368 KiB, 4 GiB 실행의 RSS 최대 2412 KiB. 모의 재연결 455.6ms이며 Mock의 450ms 오프라인 시간을 포함합니다. 결과는 현재 환경의 단일 시험값입니다. 재현 명령은 `python3 tests/benchmark.py out/linux/fastboot-proxy`입니다.
