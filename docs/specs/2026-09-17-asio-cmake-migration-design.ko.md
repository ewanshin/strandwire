# asio + CMake 마이그레이션 설계

[English](2026-09-17-asio-cmake-migration-design.md) | 한국어

- 작성: 2026-09-17
- 상태: 승인 및 구현 완료 (2026-09-17, Windows에서 검증). 이후 수정은 본문에 날짜를 적는다.
- 범위: 저장소 전체
- 이 문서는 마이그레이션 시점의 설계를 기록한다. 현재 구조는 `docs/architecture.md`에,
  현재 프로토콜은 `docs/protocol.md`에 있다.

## 1. 목표와 범위

목표: 같은 소스를 Windows, macOS, Linux에서 빌드하고 실행한다.
이전: 서버는 WinSock2 overlapped I/O + APC를 썼고, 빌드는 MSBuild뿐이었다. Windows 전용이었다.

범위:

- 서버(`NetworkServer`)를 standalone asio 1.38.2 위에 다시 작성한다.
- 클라이언트(`NetworkClient`, `DummyClient`)를 같은 asio로 옮기고 MSVC 전용 CRT 호출을 제거한다.
- 빌드를 CMake로 바꾼다. 옛 VS 솔루션/프로젝트 파일을 삭제한다. Windows용으로 손으로 관리하는 새 VS
  솔루션을 만든다 (4.1절).
- 서버를 같은 프로세스 안에서 실행하는 smoke 테스트를 추가한다.

범위 밖: TLS, `io_uring`, 프로토콜 변경, DB, Godot/cocos2d-x 클라이언트, CI.

## 2. 결정 기록

| # | 항목 | 결정 | 이유 |
|---|---|---|---|
| D1 | 마이그레이션 범위 | 저장소 전체 | 클라이언트가 NuGet asio 1.12.2와 `strcpy_s`에 묶여 있었다. 서버만 옮기면 클라이언트는 mac/Linux에서 빌드할 수 없다 |
| D2 | 코드 스타일 | C++20 코루틴 (`co_spawn` + `awaitable`) | 현재 asio의 일급 인터페이스다. 세션 로직이 순차 코드처럼 읽힌다. 설치된 두 MSVC 버전(14.44/14.51) 모두 지원한다 |
| D3 | 스레드 모델 | `io_context` 하나 + 워커 N개 + 세션마다 strand 하나. 세션 레지스트리는 mutex 아래 (방식 A) | 세션 안에는 락이 없다. 유일한 락은 맵 하나다. B와 C는 이 규모에서 간접 단계만 더한다 (A.3) |
| D4 | asio 확보 방법 | git 서브모듈 `third_party/asio`, 태그 `asio-1-38-2` | 어느 OS에서도 추가 도구가 없다. 오프라인 빌드가 된다 |
| D5 | VS 솔루션 | 옛 솔루션 삭제. 손으로 관리하는 `strandwire.sln`을 CMake와 나란히 유지 (2026-09-17 수정) | 사용자가 CMake 출력이 아니라 손으로 관리하는 솔루션을 원했다. 첫 결정은 "CMake만"이었다. 공유 설정은 props 파일 두 개에 있다. 소스 목록 불일치는 CTest `check_vs_sync`가 잡는다 |
| D6 | 메모리 모델 | `std::shared_ptr` 소유. `ObjectPool` 제거 | 코루틴이 `self`를 캡처해 수명을 늘린다. 측정 후 풀을 allocator로 나중에 붙일 수 있다 |
| D7 | 프레이밍 헬퍼 | `common/packet_io.h`의 헤더/바디 읽기/쓰기 코루틴을 서버와 클라이언트가 공유 | 실행 파일 세 개에 중복된 프레이밍 코드를 없앤다 |
| D8 | 소스 인코딩 | 새 파일은 UTF-8. MSVC에서 `/utf-8` | 파일을 다시 쓰는 중이므로 CP949 규칙을 버린다 |
| D9 | 서버 라이브러리 위치와 이름 | 폴더 `ServerLib/`, 타깃 `ServerLib` (2026-09-18 수정) | 처음에는 `netsys_server` 소스가 `NetworkServer/`에 있었다. 한 폴더에 타깃 두 개는 혼란스러웠고 헤더를 `"NetworkServer/server.h"`로 include해야 했다. 다른 폴더처럼 PascalCase다 |

## 3. 저장소 구조 (마이그레이션 시점)

```
strandwire/
├── CMakeLists.txt              # root: C++20, warning options, subdirectories
├── CMakePresets.json           # windows-msvc / linux / macos (Ninja)
├── .gitmodules
├── third_party/asio/           # git submodule, tag asio-1-38-2
├── common/                     # INTERFACE library netsys_common
│   ├── packet_header.h         # protocol (wire format kept)
│   ├── server_define.h
│   ├── strutil.h               # safe copy into fixed arrays
│   └── packet_io.h             # framing coroutines (read_packet / write_packet)
├── ServerLib/                  # static library, all server logic
│   ├── server.h/.cpp           # acceptor coroutine, io_context, thread pool, shutdown
│   ├── session.h/.cpp          # strand, receive/send coroutines, send queue
│   ├── session_manager.h/.cpp  # session registry, broadcast, player id allocation
│   └── dispatcher.h/.cpp       # packet type -> handler (later replaced by lobby_service)
├── NetworkServer/              # executable, main.cpp only, links ServerLib
├── NetworkClient/              # executable: main.cpp, client_session.h/.cpp
├── DummyClient/                # executable: main.cpp, dummy_session.h/.cpp
└── tests/
    ├── framing_test.cpp        # packet struct sizes and header round trip (later wire_test)
    └── smoke_test.cpp          # in-process server + two clients
```

모든 폴더에 자체 `CMakeLists.txt`가 있다.

- 삭제: `strandwire.sln`, `DummyClient.sln`, 모든 `.vcxproj`/`.vcxproj.filters`/`packages.config`/`stdafx.*`/
  `targetver.h`/`ReadMe.txt`, `NetworkServer/ObjectPool.h`, `NetworkServer/ProducerConsumerQueue.h`,
  `common/stream.hpp`, `common/stream.cpp`.
- 디렉터리 이름 `NetworkServer`, `NetworkClient`, `DummyClient`, `common`은 유지한다.
- 규칙: 폴더 하나 = 타깃 하나 = vcxproj 하나 (D9).

## 4. 빌드

- 루트 `add_library(asio INTERFACE)`: `third_party/asio/include`를 노출한다. `ASIO_STANDALONE`, `ASIO_NO_DEPRECATED`를 정의한다.
  Windows에서는 같은 타깃이 `_WIN32_WINNT=0x0A00`을 더하고 `ws2_32`를 링크한다. 다른 타깃은 `asio`만 링크한다.
- `CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_EXTENSIONS OFF`.
- 필요 버전: CMake 3.21 (Ubuntu 22.04의 3.22로 빌드할 수 있도록 2026-09-19에 3.25에서 낮춤),
  MSVC 2022, GCC 11, Clang 14.
- 경고: MSVC `/W4 /utf-8`, 그 외 `-Wall -Wextra`. 오류로 승격하지 않는다.
- `CMakePresets.json`: configure/build/test 프리셋 `windows-msvc`, `linux`, `macos`. Ninja.
  빌드 디렉터리는 `build/<preset>`, 실행 파일은 `build/<preset>/bin/`.
- 서버 로직은 정적 라이브러리 `ServerLib`이다. `NetworkServer`는 `main.cpp`뿐이다.
  smoke 테스트는 이 라이브러리를 링크해 서버를 같은 프로세스 안에서 실행한다.

```
git submodule update --init
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

### 4.1 Visual Studio 솔루션 (Windows, 손으로 관리)

- 루트 `strandwire.sln`. 마이그레이션 시점에 프로젝트 여섯 개: `ServerLib` (정적 라이브러리), `NetworkServer`, `NetworkClient`,
  `DummyClient`, `framing_test`, `smoke_test`. 현재는 아홉 개다 (`docs/architecture.md`).
- 구성 `Debug|x64`, `Release|x64`. 툴셋 v143 (VS 2022). vcxproj와 `.filters`는 각 소스 폴더에 둔다.
- 공유 설정은 props 파일 두 개에 있다. 값은 루트 `CMakeLists.txt`와 같게 유지한다.
  - `strandwire.config.props`: 툴셋, 문자 집합, 디버그 런타임. `Microsoft.Cpp.props`보다 먼저 설정해야 하는 값.
  - `strandwire.props`: include 경로, 전처리기 정의, C++20, `/W4 /utf-8`, 출력 디렉터리.
- 각 vcxproj는 자신의 GUID, 출력 종류, 소스 목록, 프로젝트 참조만 가진다.
- 경로는 `$(SolutionDir)`가 아니라 props 파일 기준(`$(MSBuildThisFileDirectory)`)이므로 vcxproj 하나만으로도 빌드된다.
- 출력: `build\vs\<Configuration>\bin\`.
- 미리 컴파일된 헤더는 쓰지 않는다.
- 파일을 추가하거나 삭제하면 vcxproj(+filters)와 `CMakeLists.txt`를 함께 갱신한다.
  CTest `check_vs_sync` (`tests/check_vs_sync.cmake`)가 `ClCompile` 목록과 CMake `SOURCES`를 비교한다.

### 4.2 protobuf 빌드 통합 (2026-09-19 추가)

CMake (세 OS 모두):

- 루트: `add_subdirectory(third_party/protobuf EXCLUDE_FROM_ALL)`.
- 옵션: `protobuf_BUILD_TESTS=OFF`, `protobuf_WITH_ZLIB=OFF`, `protobuf_INSTALL=OFF`,
  `protobuf_BUILD_SHARED_LIBS=OFF`, `protobuf_MSVC_STATIC_RUNTIME=OFF` (이 저장소는 `/MD`를 쓴다).
- `libprotobuf`의 include 경로를 `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`로 설정해 protobuf 헤더를 `/W4`에서 뺀다.
  `add_subdirectory(... SYSTEM)`은 CMake 3.25 기능이라 쓰지 않는다 (필요 버전은 3.21).
- protobuf 자체는 C++11로 빌드된다.
- `ProtoLib/CMakeLists.txt`는 `.proto`마다 `protobuf::protoc`를 호출하는 custom command를 만들고
  생성된 `.pb.cc`를 정적 라이브러리 `ProtoLib`으로 묶는다.
- 생성 파일은 빌드 디렉터리(`<build>/ProtoLib/gen`)에 있고 커밋하지 않는다. `"chat.pb.h"`로 include한다.
- 첫 빌드는 protobuf와 protoc를 소스에서 컴파일하므로 몇 분 더 걸린다.

Visual Studio 솔루션:

- protobuf용 vcxproj를 손으로 쓰지 않는다. 한 번 빌드해 설치하고, 설치된 출력을 링크한다.
- `tools/build_protobuf.ps1`: 서브모듈을 CMake(VS 2022 generator, v143, x64, `/MD`)로 Debug와 Release 빌드하고
  `third_party/_install/protobuf/<Configuration>/`에 설치한다 (`bin/protoc.exe`, `lib/libprotobuf(d).lib`, `include/`).
  이 폴더는 gitignore된다. 새 PC에서 한 번, protobuf 버전이 바뀔 때 다시 실행한다.
- `strandwire.props`: 설치 폴더의 include/lib 경로와 생성 코드 폴더(`build\vs\gen`)를
  외부 include(`ExternalIncludePath`, 경고 끔)로 더한다. 구성에 따라 `libprotobufd.lib` 또는 `libprotobuf.lib`를 링크한다.
- `ProtoLib/ProtoLib.vcxproj`: `.proto` 파일을 `CustomBuild` 항목으로 가진다. 설치된 `protoc.exe`로 `build\vs\gen\`에
  코드를 생성하고 `.pb.cc`를 컴파일한다. 설치 폴더가 없으면 스크립트를 실행하라는 메시지와 함께 실패한다.
  생성 코드는 경고를 끈다 ("개별 vcxproj 파일에 컴파일러 옵션 없음" 규칙의 유일한 예외).
- `check_vs_sync`: `ProtoLib`은 `ClCompile` 대신 `CustomBuild`(`.proto`) 목록을 CMake의 `.proto` 목록과 비교한다.

이 protobuf 버전과 런타임을 고른 이유: `docs/protocol.md` 16절.

## 5. 프로토콜과 이식성 (대체됨)

> 2026-09-19에 LPN 프레이밍 + protobuf로 대체됐다. 참조 문서는 `docs/protocol.md`다.
> `packet_header.h`, `packet_io.h`, `strutil.h`는 삭제됐다. 아래는 마이그레이션 당시의 기록이다.

- `packet_header.h`의 `#pragma pack(1)` 구조체와 enum은 유지한다. 와이어 형식은 바뀌지 않는다.
- `short` → `int16_t`, `memset` → 값 초기화 (`char name[MAX_NAME_LEN]{}`). `<cstdint>`, `<cstring>`을 명시적으로 include한다.
- `MAX_PACKET_SIZE`는 `packet_header.h`에 둔다. 모든 구조체가 그보다 작다는 것을 `static_assert`로 확인한다.
  수신 측은 헤더 `size`가 `sizeof(packet_header)`와 `MAX_PACKET_SIZE` 사이인지 검사한다.
- `strcpy_s`, `sprintf_s`, `printf_s`, `memcpy_s`를 제거한다. 고정 배열 복사는 `strutil.h`의 헬퍼 하나,
  `copy_to(char (&dst)[N], std::string_view)`를 거친다 (항상 null 종료, 잘라냄).
- 출력은 `std::cout`/`std::cerr`로 한다. `std::format`은 쓰지 않는다 (GCC 13 이전은 미지원).
- `WinSock2.h`, `WS2tcpip.h`, `tchar.h`, `process.h`, `SDKDDKVer.h`의 직접 include를 제거한다.
- 플랫폼 분기는 CMake 링크 설정에만 둔다.

## 6. 서버 아키텍처

스레드 모델과 소유 모델은 지금도 유효하다. 디스패처(6.4)와 패킷 세부는 프로토콜과 함께 바뀌었다.

### 6.1 스레드 모델

- `io_context` 하나. 워커 수는 `--threads N`, 기본값 `std::thread::hardware_concurrency()`, 최소 1.
- `start()`가 워커 N개를 띄우고 `wait()`가 join한다. 메인 스레드는 `io_context::run()`에 참여하지 않는다
  (테스트가 서버를 백그라운드에서 실행할 수 있도록).
- acceptor 코루틴 하나는 strand 없이 `io_context` executor에서 실행된다.
- 세션마다 `asio::strand<asio::io_context::executor_type>` 하나. 소켓은 그 strand를 executor로 쓴다.
  모든 세션 코루틴은 `co_spawn(strand, ...)`으로 띄운다.
- 결과: 세션 멤버는 strand 위에서만 건드린다. 락이 없다.
- 다른 스레드(예: 브로드캐스트)는 `asio::dispatch(session->strand(), ...)`로 세션에 접근한다.
- `session_manager`: `std::mutex` 아래의 `std::unordered_map<session_id, shared_ptr<session>>`.
  락을 쥔 채로 세션 메서드를 호출하지 않는다. 스냅샷을 뜨고, 풀고, 그다음 호출한다.

### 6.2 메모리와 소유

- 세션은 `std::make_shared<session>`이다. 강한 참조는 둘: `session_manager` 맵과 실행 중인 코루틴의 `self`.
- 연결이 끊기면 맵 항목을 제거한다. 코루틴이 끝나면 `self`가 풀리고 객체가 사라진다. `ObjectPool`은 없다.
- 송신 패킷은 `std::shared_ptr<const std::vector<char>>`다. 브로드캐스트는 버퍼 하나를 세션 N개가 공유하므로 한 번만 복사한다.
- 세션별 송신 큐는 그 `shared_ptr`의 `std::deque`이며 strand 위에서만 건드린다.
- 수신 버퍼(마이그레이션 시점): 세션마다 `std::array<char, MAX_PACKET_SIZE>` 하나. 헤더 4바이트를 정확히 읽고,
  검증한 뒤, 바디 `size - 4`바이트를 정확히 읽는다. 링 버퍼(`stream.hpp`)는 삭제했다.
- `session_id`: 서버 전역 `std::atomic<uint32_t>`. `player_id`: 로그인 시 발급하는 별도 atomic, 1부터 시작.
  맵 크기를 id로 쓰는 방식은 버렸다. `INVALID_ID = -1`은 유지한다.

### 6.3 세션 수명

1. accept → `make_shared<session>(std::move(socket), manager)` → `manager.add()` →
   `co_spawn(strand, session->run(), detached)`.
2. `run()`은 수신 루프다. `packet_io::read_packet`으로 패킷 하나를 읽어 `dispatcher::dispatch`에 넘긴다.
   오류와 예외는 `close("reason")`으로 모인다.
3. `send(buf)`는 strand 위에서만 호출한다. 큐에 넣고, 송신 코루틴이 없으면 `co_spawn(strand, write_loop())`한다.
   `write_loop`는 큐가 빌 때까지 `async_write`를 반복한 뒤 플래그를 내린다.
4. `close(reason)`은 멱등이다: 소켓 `shutdown` + `close`, `manager.remove(id)`, 큐 비우기, 로그 한 줄.
   이후 호출은 아무것도 하지 않는다.
5. 송신 큐 한도 `MAX_SEND_QUEUE = 256`. 넘으면 느린 소비자다: `close`.

`write_loop`는 2026-09-19에 수정됐다: await 전에 버퍼를 pop한다 (`docs/architecture.md`의 송신 큐 규칙).

### 6.4 디스패처 (대체됨)

마이그레이션 시점의 설계. 지금은 `common/lpn/dispatcher.h`의 msgid 디스패처와 `lobby_service`가 대신한다.

- `std::array<handler_fn, PACKET_TYPE::MAX>`. `handler_fn`은 `void(*)(session&, std::span<const char> packet)`이다.
- 정적 초기화 매크로 대신 `dispatcher::register_all()`에서 채운다. 서버 시작 시 명시적으로 한 번 호출한다.
  정적 초기화 순서 문제를 없앤다.
- 등록되지 않았거나 범위 밖인 타입은 `close("unknown packet")`를 호출한다. null 포인터는 절대 호출하지 않는다.
- 핸들러는 세션 strand 위에서 실행된다. mutex는 `session_manager`를 건드릴 때만 잡는다.
- `CS_LOGIN`: `manager.next_player_id()`로 id를 발급하고, 이름을 저장하고, `SC_LOGIN`으로 응답한다.
- `CS_CHAT`: 로그인 전이면 `close("chat before login")`. 아니면 `chat_res`를 만들어 `manager.broadcast(buf)`한다.
- `broadcast(buf)`: 락 → `shared_ptr` 스냅샷 → 언락 → 세션마다
  `asio::dispatch(s->strand(), [s, buf]{ s->send(buf); })`.

### 6.5 서버 시작과 정지

- `server`: `server(port, threads)` (지금은 `server_options`), `start()`, `stop()`, `wait()`,
  `port()` (포트 0으로 시작했을 때의 실제 포트), `session_count()`. 테스트가 쓴다.
- `asio::signal_set`이 SIGINT/SIGTERM을 받아 `stop()`을 호출한다. Windows Ctrl+C도 같은 경로를 탄다.
- `stop()`: acceptor 닫기 + `manager.close_all()`. 남은 작업이 없으면 워커가 끝나고 `wait()`가 join한다.
  마이그레이션 시점에는 `stop()`이 `io_context::stop()`도 호출하고 join까지 했다. 지금은 논블로킹이다.

## 7. 오류 처리와 로깅

- I/O는 `asio::use_awaitable`을 쓰므로 오류는 예외로 던져진다. 각 코루틴 최상위의 `try/catch`가 `close(e.what())`로 모은다.
- `operation_aborted`는 정상 종료이므로 로그하지 않는다.
- 프로토콜 위반(헤더 size 범위 밖, 알 수 없는 타입, 로그인 전 채팅)은 즉시 `close`한다.
- 로깅: 마이그레이션 시점에는 `std::cerr` 한 줄, 라이브러리 없음 (`[session 12] closed: eof`).
  2026-09-19에 `LogLib`(spdlog 래퍼)으로 대체됐다 (A.11).

## 8. 클라이언트

- 둘 다 `common/packet_io.h`의 `read_packet`/`write_packet` 코루틴을 쓴다. 각자의 헤더 조립 코드는 제거했다.
- `NetworkClient`: `io_context`는 백그라운드 `std::thread`에, 메인 스레드는 stdin에. 입력은 `asio::post(io, ...)`로 넘긴다.
  옛 코드는 다른 스레드에서 소켓에 썼다. 데이터 레이스였다.
  `quit`는 close를 post하고 join한다. 사용법 `NetworkClient <ip> <port> <name>`.
- `DummyClient`: 단일 스레드 `io_context` 위에 세션 N개. 세션마다 코루틴 둘 (수신 루프, `steady_timer`로 매초 채팅).
  옵션 `--ip --port --session --duration <sec>`. `duration` 0은 무한 실행. 아니면 그 시간 뒤 종료 코드 0.

## 9. 테스트 (마이그레이션 시점)

- CTest에 등록된 `assert` 기반 실행 파일 둘, 프레임워크 없음.
- `framing_test` (이후 `wire_test`): 패킷 구조체 크기가 기대값과 일치 (`static_assert` 포함),
  `write_packet` → `read_packet` 왕복이 원본과 같음.
- `smoke_test`: `server(0, 2)`를 시작하고 실제 포트를 읽는다. 단계:
  1. 클라이언트 둘이 로그인 → 서로 다른 `player_id`.
  2. A가 채팅 → 둘 다 A의 이름이 든 `SC_CHAT`을 받는다.
  3. A가 연결 해제 → `session_count() == 1`.
  4. 로그인 전 채팅 → 강제 close.
  5. `stop()`.
- 행 방지: 클라이언트 `io_context::run_for(15s)`, 세션 수 폴링 타임아웃 (5s).

현재 테스트 내용: `docs/architecture.md`의 `wire_test`와 `smoke_test` 절.

## 10. 문서 갱신

구현 후 `CLAUDE.md`와 `README.md`의 빌드/실행/아키텍처/노트 절을 새 구조에 맞게 다시 쓴다.
기원과 프로젝트 목표 절은 유지한다.

## 부록 A. 검토했으나 택하지 않은 대안

결정 기록의 항목마다 택하지 않은 선택지. 전제가 바뀌면 여기로 돌아온다.

### A.1 네트워크 라이브러리 (asio 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| 직접 작성한 IOCP / epoll / kqueue | 최대의 제어와 성능. 상용 MMO 서버가 하는 방식 | OS 세 개분의 I/O 계층, 타이머, 취소, 스레드 모델을 써야 한다 |
| libuv (+ uvw) | Node.js로 검증된 3-OS 지원 | C API, 콜백 지향. 코루틴 통합이 asio보다 약하다 |
| libevent / libev | 오래되고 안정적 | Windows 지원이 select 기반. 새 프로젝트에서는 드물게 선택된다 |
| POCO Net | JSON, DB, 로깅 포함 | 무겁다. 스레드 풀 + reactor는 코루틴과 맞지 않는다 |
| ENet | 신뢰성 있는 UDP. 실시간 이동에 적합 | 프로토콜 전체가 바뀐다. 채팅 스트림에는 과하다 |
| Valve GameNetworkingSockets | 암호화와 NAT 통과를 갖춘 현대판 ENet | 큰 의존성. 라이브러리가 스레드 모델을 정한다 |
| gRPC / ZeroMQ / nng | 메시징 패턴과 직렬화 | 프로토콜 연습이 목적인 프로젝트와 맞지 않는다 |
| Boost.Asio | standalone과 같은 코드 | Boost 전체에 의존한다. Boost.Beast가 필요해지면 재검토 |

### A.2 비동기 코드 스타일 (C++20 코루틴 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| 콜백 + 람다 (C++17) | 어디서나 컴파일된다. 옛 클라이언트 스타일 | 수신→처리→송신이 핸들러 여러 개로 쪼개진다. `self` 캡처가 반복된다 |
| Stackful 코루틴 (`asio::spawn` + Boost.Context) | C++20 이전의 순차 코드 | 별도 스택 할당, Boost.Context 의존. standalone과 맞지 않는다 |
| future/promise (`use_future`) | 익숙한 API | 블로킹 대기는 이벤트 루프와 맞지 않는다. 스레드를 점유한다 |
| sender/receiver (`std::execution`, C++26) | 표준의 향후 방향 | asio 통합이 실험 단계. 컴파일러 지원이 초기. 2-3년 뒤의 선택지 |

### A.3 스레드 모델 (방식 A 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| B. 액터 스타일 (매니저도 strand 위에) | mutex 없음 | 로그인 하나가 세션 strand → 매니저 strand → 세션 strand로 옮겨 다닌다. 간접 단계가 늘어난다 |
| C. 스레드마다 io_context 하나 (샤딩) | strand 없음. 각 스레드가 자기 세션을 소유 | 브로드캐스트가 스레드를 넘나들어 결국 B의 post 구조가 된다. acceptor 분배 로직이 필요하다 |
| D. 단일 스레드 | 락이 전혀 없다. 옛 서버의 모델 | 코어 하나만 쓴다. A에 `--threads 1`을 준 것과 같으므로 별도 구현이 없다 |
| E. I/O 스레드 + 로직 스레드 | 복잡한 게임 서버의 교과서적 배치 | 채팅 서버에서는 큐만 하나 더한다. 로직이 커지면 재검토 |

### A.4 빌드 시스템 (CMake 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| 별도의 VS 솔루션 + Xcode + Makefile | 추가 도구 없음 | 세 벌을 손으로 맞춰야 한다 |
| Meson | 깔끔한 문법, 빠름 | Python 의존. VS 통합이 CMake보다 약하다 |
| Bazel | 큰 모노레포에 강함 | 이 규모에는 설정 비용이 너무 크다 |
| xmake / premake | 가볍고 Lua 기반 | 생태계와 IDE 통합이 작다 |

### A.5 asio 확보 방법 (git 서브모듈 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| CMake FetchContent | configure 시 자동 clone, 서브모듈 없음 | 오프라인 빌드가 안 된다. 새로 configure할 때마다 내려받는다 |
| vcpkg (manifest 모드) | `openssl` 같은 의존성 추가가 쉽다 | vcpkg 설치와 툴체인 설정이 먼저 필요하다. 의존성이 셋을 넘으면 재검토 |
| Conan | vcpkg와 비슷 | Python이 필요하다. 국내 게임 업계에서는 vcpkg가 더 흔하다 |
| 시스템 패키지 (apt/brew) | 설치가 간단 | Windows에 없다. 배포판마다 버전이 다르다 |
| 헤더 직접 포함 | 가장 단순 | 갱신 이력이 git에 남지 않는다 |

### A.6 VS 솔루션 처리

결정: CMake와 나란히 손으로 관리하는 .sln (4.1절).

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| CMake만 (첫 결정) | 빌드 정의 하나 | VS에서 "폴더 열기"나 생성된 vcxproj가 필요하다. 사용자가 손으로 관리하는 솔루션을 선호해 수정됨 |
| Windows는 .sln만, mac/Linux는 CMake만 | Windows에서 도구 하나 | 깨진 CMake 정의를 Windows에서 알아채지 못한다 |
| CMake로 생성해 커밋 | VS에서 바로 열린다 | 생성 출력: 지저분한 diff와 절대 경로 |

### A.7 메모리와 소유 (shared_ptr 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| ObjectPool 유지 | 할당을 아낀다. 옛 방식 | 코루틴이 `self`를 쥐므로 참조 카운트가 어차피 필요하다. 나중에 `std::allocate_shared` + 커스텀 allocator로 더할 수 있다 |
| 침습적 참조 카운트 | shared_ptr의 제어 블록 할당을 없앤다. EasyGameServer의 `RefCountable` | 측정 후 검토 |
| 세션 id + 세대 테이블 | 댕글링 포인터를 없앤다 | 코루틴이 await마다 다시 조회해야 한다 |
| unique_ptr + 수동 수명 | 최소 오버헤드 | 코루틴과 맞지 않는다 |

### A.8 프레이밍과 수신 버퍼 (고정 배열 + 정확한 크기 읽기 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| 링 버퍼 유지 (`stream.hpp`) | `async_read_some` 한 번에 패킷 여러 개를 파싱, 시스템 콜이 적다 | 부분 패킷 처리가 필요하다. 패킷당 두 번 읽기가 병목으로 확인되면 돌아간다 |
| `dynamic_buffer` + `async_read_until` | 텍스트 프로토콜에 편리 | 길이 접두 바이너리와 맞지 않는다 |
| 직렬화 라이브러리 (protobuf, flatbuffers, msgpack) | 스키마 진화, 다중 언어 | 당시에는 Godot 클라이언트까지 미뤘다. protobuf는 2026-09-19에 채택 |

### A.9 오류 처리 (예외 + 최상위 catch 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| `asio::as_tuple(use_awaitable)` | 명시적, 예외 없음. 모든 await가 `(error_code, result)`를 돌려준다 | 줄마다 `if (ec)`. 기본 completion token일 뿐이라 나중에 바꾸는 비용이 싸다 |
| `asio::redirect_error` | 선택한 await에만 error_code | 스타일이 섞이면 일관성이 깨진다 |

### A.10 테스트 (assert 실행 파일 대신)

| 대안 | 장점 | 택하지 않은 이유 |
|---|---|---|
| Catch2 / doctest | 단일 헤더, 좋은 출력 | 테스트가 늘면 doctest 검토 |
| GoogleTest | 업계 표준 | 서브모듈 하나 더, 빌드가 느려진다 |

### A.11 기타

| 항목 | 현재 결정 | 대안과 전환 시점 |
|---|---|---|
| 로깅 | spdlog 1.17.0을 감싼 `LogLib` (2026-09-19 수정). 인자는 스트림 방식, 포맷 문자열 없음. 파일 로그는 선택, 기본 꺼짐 | `std::cerr` 한 줄로 시작했다. 여러 워커 스레드의 출력이 실제로 줄 중간에 섞여 대체했다 |
| ID 발급 | 로그인 시 atomic 카운터 | 영속 계정이 생기면 DB 시퀀스나 UUID |
| 설정 | 명령줄 옵션 | 옵션이 다섯 개를 넘으면 JSON/TOML 설정 파일 |
| 디스패처 | 마이그레이션 시점: 함수 포인터 배열 + `register_all()`. 2026-09-19부터: msgid → 핸들러 맵, 함수 포인터 하나로 등록 | 당시의 대안: `unordered_map<type, std::function>`, 템플릿 자동 등록, `switch`. 패킷이 수십 개가 되면 자동 등록 |
