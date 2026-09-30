strandwire
====

[English](README.md) | 한국어

개인 취미용 네트워크 서버/클라이언트.

- 구성 요소: standalone asio 1.38.2 + C++20 코루틴, protobuf 3.21.12, spdlog 1.17.0, CMake 3.21+
- 검증 환경: Windows(MSVC), Linux(g++ 11), macOS(Apple clang 21)
- 프로토콜: LPN 프레이밍 + protobuf 본문. 터널(서비스 종류별 논리 채널), 터널 동작, 하트비트,
  메시지 이름의 FNV-1a 해시 msgid

구성
---

| 이름 | 내용 |
|---|---|
| `LogLib` | spdlog 비동기 로거를 감싼 로거 |
| `ProtoLib` | `.proto`와 생성 코드 |
| `ServerLib` | 서버 로직 (asio, strand 기반 멀티 스레드) |
| `NetworkServer` | 서버 실행 파일 |
| `NetworkClient` | 대화형 콘솔 클라이언트 |
| `DummyClient` | 부하 클라이언트 |
| `common/lpn` | 와이어 프로토콜 구현 (헤더 전용) |
| `tests` | `wire_test`(바이트 단위), `smoke_test`(인프로세스 종단 간) |

빌드
---

```
git submodule update --init
cmake --preset windows-msvc    # linux / macos
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

- Windows의 Ninja 프리셋은 VS 개발자 명령 프롬프트에서 실행한다.
- 첫 빌드는 protobuf를 소스에서 컴파일하므로 몇 분이 더 걸린다.

Visual Studio (Windows)
---

1. 새 PC에서는 protobuf를 먼저 한 번 설치한다.
   ```
   powershell -ExecutionPolicy Bypass -File tools\build_protobuf.ps1
   ```
2. `strandwire.sln`을 VS 2022 이상에서 연다.

- 구성: `Debug|x64`, `Release|x64`. 출력: `build\vs\<Configuration>\bin\`.
- 공통 설정: `strandwire.config.props`, `strandwire.props`.
- CMake가 생성한 것이 아니라 직접 관리하는 솔루션이다. 소스 파일을 추가하거나 지우면 vcxproj와
  `CMakeLists.txt`를 함께 고친다. 어긋나면 CTest `check_vs_sync`가 실패한다.

실행
---

```
NetworkServer --port 10000            # ESC 또는 Ctrl+C로 종료
NetworkClient 127.0.0.1 10000 <name>
DummyClient --ip 127.0.0.1 --port 10000 --session 100 --duration 10
```

모든 실행 파일은 `--log-level <level>`과 `--log-dir <폴더>`(기본은 파일 로그 없음)를 받는다.

문서
---

문서는 영어로 쓴다.

| 문서 | 내용 |
|---|---|
| `docs/protocol.md` | 프로토콜: 바이트 배치, 메시지, 예시, 설계 결정, 변경 가이드 |
| `docs/specs/2026-09-17-asio-cmake-migration-design.md` | asio/CMake 이식 설계와 검토한 대안 |
| `CLAUDE.md` | 개발 안내 |

의존성
---

| 라이브러리 | 버전 | 라이선스 | 방식 |
|---|---|---|---|
| asio (standalone) | 1.38.2 | BSL-1.0 | git submodule, 헤더 전용 |
| protobuf | 3.21.12 | BSD-3-Clause | git submodule, 소스 빌드 |
| spdlog | 1.17.0 | MIT | git submodule, 헤더 전용 |

출처와 라이선스
---

2020년에 zeliard/EasyGameServer(MIT)를 축소해 다시 쓴 것에서 시작했다. 서버는 2026년에 처음부터
다시 썼고, 부하 테스트 클라이언트의 동작은 그때 것을 이어받았다.

MIT License. `LICENSE` 참고.
