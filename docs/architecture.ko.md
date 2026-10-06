# 아키텍처

[English](architecture.md) | 한국어

코드가 무엇이고 어떻게 도는지: 폴더와 빌드 타깃, 각 프로젝트, 와이어 프로토콜 요약, 스레드 모델, 세션과 서버의
생명주기, 테스트가 확인하는 것. 프로토콜 자체는 `docs/protocol.ko.md`에 정의되어 있다. 빌드와 실행은
`docs/build.ko.md`에 있다.

## 현재 상태

`v0.3.0`, 2026-09-22.

- `v0.2.0`(2026-09-19): asio + CMake 이식, LPN 프레이밍 + protobuf 프로토콜.
- `v0.3.0`(2026-09-22): 중복 길이 필드 제거, `type_`를 값 하나당 연산 하나로 평탄화(와이어 변경).
- 빌드, 테스트, 실행, Windows 클라이언트 교차 접속을 세 OS 모두에서 검증
  (와이어 변경 뒤: Windows와 Linux 2026-09-20, macOS 2026-09-22).
  - Windows: MSVC. CMake와 VS 솔루션 둘 다.
  - Linux: WSL2 Ubuntu 22.04, g++ 11.4, CMake 3.22.
  - macOS: 27.0 arm64, Apple clang 21, CMake 3.31.
- 예정된 클라이언트: 순수 C++(있음), Godot, 어쩌면 Unity. Godot/cocos2d-x 클라이언트는 아직 없다.

## 개요

standalone asio 1.38.2 + C++20 코루틴 위의 TCP 채팅 서버와 클라이언트 둘.
프로토콜은 protobuf 본문을 담는 LPN 프레이밍이다(`docs/protocol.ko.md`).
소스 안의 로그 문자열은 영어다.

프로젝트(빌드 타깃)는 9개다. 그 밖의 폴더와 파일:

- `common/lpn/` - 와이어 프로토콜 구현. header-only라서 자기 프로젝트가 없다.
  CMake: INTERFACE 타깃 `netsys_common`. VS: `ServerLib`의 `common\lpn` 필터.
- `common/`의 나머지 헤더:
  - `server_define.h` - 와이어와 무관한 상수(`LISTEN_PORT`, `INVALID_ID`).
  - `utf8.h` - 엄격한 UTF-8 검증.
  - `error_text.h` - 로케일과 무관한 `std::error_code`의 영어 설명.
  - `console.h` - 콘솔 UTF-8 입출력 헬퍼와 ESC 키 감시. `main.cpp` 파일에서만 include한다.
- `strandwire.sln`, `strandwire.props`, `strandwire.config.props` - 손으로 관리하는 VS 솔루션과 공유 속성 시트.
- `tools/build_protobuf.ps1` - VS 솔루션용 protobuf를 `third_party/_install/`에 설치한다.
- `third_party/` - git 서브모듈 셋.
  - `asio` - 헤더는 `third_party/asio/include/`.
  - `protobuf` - 태그 `v3.21.12`.
  - `spdlog` - 태그 `v1.17.0`, header-only로 사용.
- `docs/protocol.ko.md` - **프로토콜의 기준 문서.** 바이트 배치, 상수, 메시지와 msgid, 에러 코드, 흐름, 바이트 예시,
  다른 언어로 클라이언트를 쓸 때의 체크리스트(1-12절), 설계 결정과 변경 가이드(13-16절).
  `wire_test`의 `test_documented_examples`가 예시를 구현과 대조한다.
  프로토콜이 바뀌면 이 문서와 그 테스트를 함께 고친다.
- `docs/specs/2026-09-17-asio-cmake-migration-design.ko.md` - asio/CMake 이식 결정, protobuf 빌드 통합(4.2절),
  검토한 대안(부록 A). 5절(옛 struct 프로토콜)은 `docs/protocol.ko.md`로 대체되었다.

## 프로젝트

폴더 하나 = CMake 타깃 하나 = vcxproj 하나이고 이름이 같다. 예외는 `tests/`(실행 파일 셋)다.

| 프로젝트 | 종류 | 폴더 | 의존 | 요약 |
|---|---|---|---|---|
| `LogLib` | 정적 lib | `LogLib/` | spdlog | spdlog 비동기 로거의 래퍼 |
| `ProtoLib` | 정적 lib | `ProtoLib/` | protobuf | `.proto` 파일과 생성 코드 |
| `ServerLib` | 정적 lib | `ServerLib/` | common, ProtoLib, LogLib | 서버 로직 전부 |
| `NetworkServer` | 실행 파일 | `NetworkServer/` | ServerLib | 얇은 서버 실행 파일 |
| `NetworkClient` | 실행 파일 | `NetworkClient/` | common, ProtoLib | 대화형 콘솔 채팅 클라이언트 |
| `DummyClient` | 실행 파일 | `DummyClient/` | common, ProtoLib | 부하 테스트 클라이언트 |
| `wire_test` | 테스트 | `tests/` | common, ProtoLib | 바이트 단위 와이어 형식 테스트 |
| `smoke_test` | 테스트 | `tests/` | ServerLib | 인프로세스 종단 간 서버 테스트 |
| `app_test` | 테스트 | `tests/` | ServerLib | 설정 로딩과 시작 단계 |

### LogLib

spdlog 비동기 로거의 래퍼 `nslog::logger`.

- 패턴 `[date time.ms][module][level][thread]message`, 레벨 trace..fatal.
- 인자는 스트림으로 이어 붙인다. 형식 문자열이 없다: `server_log.info("[session ", id, "] closed: ", why)`.
- 파일 로그는 **선택이며 기본은 꺼짐.** `configuration::folder_name`이 비어 있으면 파일이 없다.
  클라이언트의 `--log-dir <folder>`, 서버 환경 파일의 `log_config.dir`이 일별 파일을 켠다.
- `logger.cpp`만 spdlog를 include한다. 로그를 쓰는 프로젝트는 spdlog의 include 경로도 컴파일 시간도 부담하지 않는다.
- 전역 레지스트리 대신 자기 스레드 풀을 쓴다.
- Windows에서는 `WriteConsoleW`로 콘솔에 쓰므로 코드 페이지와 관계없이 UTF-8 이름이 표시된다.
  옵션 매크로는 `logger.cpp` 맨 위에 있다.

접근은 모듈마다 싱글턴 매크로 하나로 한다:
`ServerLib/server_log.h`의 `server_log`, 각 클라이언트 폴더 `client_log.h`의 `client_log`.

**수명 규칙:**

- `main()`이 `start({level, module name, folder})`를 부르고, 반환하기 전에 `stop()`을 부른다.
  정적 소멸 중에는 spdlog 내부 정적 객체가 이미 사라졌을 수 있다.
- `start()` 전이나 `stop()` 뒤의 로그는 조용히 버려진다.
- `start`/`stop`은 워커 스레드가 시작되기 전과 join된 뒤에만 부른다.

### ProtoLib

`.proto` 파일(`chat.proto`, `error_code.proto`, `lobby_config.proto`)과 protoc 생성 코드의 정적 라이브러리.

- 생성 코드는 커밋하지 않는다. CMake는 `<build>/ProtoLib/gen`, VS는 `build\vs\gen`에 둔다.
- 사용하는 쪽은 `#include "chat.pb.h"`라고 쓴다.
- 패키지와 메시지 이름이 msgid를 결정한다. **이름을 바꾸면 와이어 계약이 바뀐다.**
- `.proto` 규약(`docs/protocol.ko.md` 8절):
  - proto2, 소문자 snake_case, 접미사 `_req`/`_res`/`_noti`.
  - 응답의 첫 필드는 `required int32 error_code_ = 1`.
  - 에러 코드는 도메인별로 100 단위 범위(1-99 시스템, 100- 인증, 200- 로비 ...).
- `.proto`를 추가하면 `ProtoLib/CMakeLists.txt`의 `NETSYS_PROTO_FILES`, `ProtoLib.vcxproj`의
  `CustomBuild`/`ClCompile`, `.filters`도 고친다.

### ServerLib

서버 로직 전부. 서버 기능은 거의 항상 여기서 바뀐다. 자세한 내용은 "설계".

- `server_app` - 프로세스를 단계로: config -> logger -> connections -> assets -> listen -> serve. 역순 해체.
- `server_config` - 설정: 명령줄의 정체성, JSON 파일의 환경, 검증, `describe()`.
- `server` - acceptor, 워커 스레드, start/stop, `server_options`.
- `session` - 연결 하나: strand, 수신/송신 코루틴, 송신 큐, 터널 표, 타임아웃 워치독.
- `session_manager` - 세션 목록, 터널별 브로드캐스트, id 할당.
- `lobby_service` - LOBBY 터널의 메시지 핸들러.

`smoke_test`가 같은 프로세스에서 서버를 돌릴 수 있도록 라이브러리로 두었다. 사용하는 쪽: `NetworkServer`와 `smoke_test`.

### NetworkServer

서버 실행 파일. `main.cpp` 하나뿐이며 콘솔(UTF-8 출력, ESC 감시)을 `server_app`에 연결한다.
프로세스 시작부터 "server ready"까지의 모든 것은 `ServerLib/server_app.h`에 있다.

**시작 단계**는 순서대로이며 `server_app`의 생명주기 메서드로 묶인다. 첫 실패에서 순서가 멈추고, 올라온 단계만
역순으로 내린다. 모든 단계가 올라오기 전에는 아무것도 accept하지 않는다.

| 메서드 | # | 단계 | 하는 일 | 실패 |
|---|---|---|---|---|
| `pre_init_instance` | 1 | config | `main()`이 `load_config`로 읽은 설정(명령줄의 정체성, `--config` 파일의 환경, 검증됨)을 받아 출력 | false 반환 |
| | 2 | logger | 읽은 로그 설정으로 `server_log.start()`. 이후 모든 단계가 로그를 쓸 수 있다 | stderr에 메시지, false 반환 |
| `init_instance` | 3 | connections | `add_connection()`으로 추가한 `server_component`(DB, 캐시, 디스커버리). 지금은 비어 있음 | 올라온 컴포넌트를 내림 |
| | 4 | assets | `add_asset()`으로 추가한 `server_component`(서비스 전에 읽는 데이터). 지금은 비어 있음 | 같음 |
| | 5 | listen | `server` 생성, 핸들러 등록, bind/listen. 이후 OS가 핸드셰이크를 완료하지만 accept도 읽기도 하지 않음 | fatal 로그, false 반환 |
| `start` | 6 | serve | (나중에: 다른 서버에 연결.) accept 루프와 워커 스레드. 이때부터 패킷을 처리 | fatal 로그, false 반환 |

`stop()`은 정지를 요청하고, `wait()`는 서버가 멈출 때까지 막고, `exit_instance()`는 같은 목록을 거꾸로 돈다:
serve -> listen -> assets -> connections -> logger. `server_app::trace()`는 테스트용으로 `up:`/`fail:`/`down:`
항목을 기록한다. `listening()`은 5단계부터, `serving()`은 6단계부터 true다.

나눈 이유: **`start()` 전에는 클라이언트가 접속할 수는 있지만 패킷은 처리되지 않는다.** 보낸 바이트는 커널에서
기다리다가 serve 단계가 올라오면 읽힌다(`smoke_test`의 `test_listen_before_start`).

**로거 시작부터 `server ready`까지 모두 로그에 남는다.** 각 단계는 `phase 'X' up (N ms)`를, 각 생명주기 메서드는
true를 돌려줄 때 `<method> success`를 남긴다(마지막은 `start success: server ready`). 실패하면 단계가 자기 이유를
남기고, 그다음 fatal 한 줄 `<method> failed at phase 'X' after N ms: start-up aborted, tearing down`을 남긴 뒤
해체가 로거를 내린다. `server stopped`는 serve 단계가 내려갈 때 남으므로 기동에 실패한 실행에는 나오지 않는다.

`main()`이 직접 설정을 읽고(`load_config`, `config_error`는 메시지와 usage를 stderr에 출력), `pre_init_instance`,
`init_instance`, `start`를 세 문장으로 따로 부른다. 각 실패는 stderr에 한 줄을 쓰고 자기 코드로 끝나므로 스크립트나
서비스 관리자가 구분할 수 있다:

| 종료 코드 | 단계 | 뜻 |
|---|---|---|
| 0 | - | 정상 종료 |
| 1 | `load_config`, `pre_init_instance` | 설정이 잘못되었거나 로거가 뜨지 않음 |
| 2 | `init_instance` | 연결, 에셋, listen 소켓 중 하나가 올라오지 않음 |
| 3 | `start` | 서비스를 시작하지 못함 |

코드는 작은 양수다. 종료 상태는 POSIX에서 8비트, Windows에서 부호 없는 32비트라서 음수 코드는 255나
4294967295로 보인다.

**로거 수명 밖의 콘솔 줄.** 로거는 2단계의 up과 down 사이에만 존재한다. 그 전의 줄(설정, phase 'config' up)과 그
뒤의 줄(phase 'logger'와 'config' down, `exited with code 0`)은 stdout으로, 오류는 stderr로 가며 모두
`[NetworkServer]` 접두어가 붙는다. `server_app.cpp`의 `note()`/`note_error()`가 `server_log.running()`이면 로거를,
아니면 콘솔을 고른다. `ServerLib`에서 허용되는 유일한 콘솔 출력이다.

**설정** (`ServerLib/server_config.h`). 명령줄은 이 인스턴스가 누구이고 환경 파일이 어디 있는지를 말한다. 파일은
나머지 전부를 담는다. 둘은 겹치지 않으므로 우선순위가 없다.

명령줄(`parse_command_line`):

| 옵션 | 기본값 | 유효 범위 |
|---|---|---|
| `--ip <address>` | `0.0.0.0` (모든 인터페이스) | listen할 IPv4 또는 IPv6 주소 |
| `--sid d.i.t.id` | `0.0.11.1` | `domain.idc.type.id` |
| `--config <file>` | 없음(전부 기본값) | protobuf `JsonStringToMessage`로 파싱하는 JSON. 모르는 키는 오류 |

환경 파일(스키마 `ProtoLib/lobby_config.proto`, 패키지 `config`). 루트 메시지 `lobby_config`는 관심사마다 섹션
하나를 가지므로 JSON은 `{ "log_config": {...}, "listen_config": {...} }` 모양이다:

| 키 | 기본값 | 유효 범위 |
|---|---|---|
| `listen_config.port` | 10000 | 0..65535 (0 = 임시 포트, 테스트용) |
| `listen_config.threads` | CPU 코어 수 | 1..1024 |
| `listen_config.session_timeout_ms` | 5000 | >= 1 |
| `log_config.level` | info | trace..fatal, off |
| `log_config.dir` | 없음(파일 로그 꺼짐) | 폴더 |
| `log_config.console` | true | `false`면 콘솔 싱크를 끈다(테스트, 서비스) |

- 없는 옵션이나 키는 기본값을 유지한다(proto2 `has_*`).
- 잘못된 값은 양쪽 모두에서 한 번에 보고된다.
- **서버마다 스키마 파일 하나: `<server>_config.proto`.** 이 서버는 LOBBY라서 `lobby_config.proto`다.
  모든 서버가 공유하는 섹션(`log_config`, `listen_config`)은 두 번째 서버가 생길 때 `base_config.proto`(이름 미정)로
  옮기고 서버 파일이 `import`한다. 서버 전용 섹션은 생기는 대로 루트 메시지에 붙인다. 지금 LOBBY에는 없다.
- `NetworkServer/lobby_config.example.json`이 템플릿이다. 로컬 사본 `NetworkServer/lobby_config.json`은 gitignore되어 있다.
- **깨끗하게 멈추는 길은 셋이고 모두 `server::stop()`으로 모인다.**
  - 콘솔의 ESC. `console::key_watcher`. stdin이 대화형 콘솔일 때만 동작한다.
  - Ctrl+C, SIGTERM. 라이브러리의 `signal_set`이 받는다.

### NetworkClient

사람이 쓰는 콘솔 채팅 클라이언트. 서버 기능을 손으로 확인할 때 쓴다.
사용법: `NetworkClient <host> <port> <name>`.

- 접속하면 anycast로 LOBBY 터널을 열고, 서버의 sid를 받고, 자동으로 로그인한다.
- 입력 한 줄 = 채팅 메시지 하나. `quit`으로 끝낸다.
- 3초마다 NOOP 하트비트를 보낸다.
- `io_context`는 백그라운드 스레드에서 돈다. 메인 스레드는 stdin만 읽는다. 입력은 `asio::post`로 io 스레드에 넘겨
  소켓 접근을 단일 스레드로 유지한다.
- 로그인 응답 전의 입력은 "not logged in yet"을 출력하고 버린다.

### DummyClient

부하 테스트 클라이언트. 자동 테스트는 동시성이나 브로드캐스트 부하를 덮지 않는다. 서버의 스레딩이나 송신 경로를
바꾼 뒤에는 이것으로 확인한다.

- 단일 스레드 `io_context` 위의 세션 N개.
- 세션마다: 터널 열기 → `UserName<n>`으로 로그인 → 0-1초 임의 지연 → 매초 채팅. 하트비트도 보낸다.
- 옵션: `--ip --port --session --duration`.
- `--duration`(초) 0이면 영원히 돈다. 아니면 그 시간 뒤에 통계(`sessions`, `logged_in`, `chats_received`)를
  출력하고 끝난다.
- 모든 세션이 로그인했으면 종료 코드 0. 스크립트의 합격/불합격 판정에 쓸 수 있다.

### wire_test

서버 없이 도는 단위 테스트. **기대 바이트열이 곧 와이어 계약이다.**
`common/lpn/`을 건드린 뒤 가장 먼저 돌린다. 기대값은 와이어를 의도적으로 바꿀 때만 고친다.
프레임워크 없이 `CHECK` 매크로뿐이다(실패하면 위치를 출력하고 코드 1로 끝난다).

확인 항목:

- big-endian 헬퍼.
- sid 조립/분해/텍스트/파싱.
- 공개된 FNV-1a 32비트 테스트 벡터 셋.
- 골든 패킷 셋의 전체 바이트열: NOOP 하트비트, LOBBY anycast CONNECT, `chat.login_req`를 담은 DATA.
- `docs/protocol.ko.md` 11절의 바이트 예시 10개와 8절의 msgid 값 5개(`test_documented_examples`).
- 디코드 거부: 짧은 터널 패킷, 터널 id ≥ 32, 헤더보다 짧은 것.
- `read_frame`이 너무 큰 프레임을 거부하는 것.
- 루프백 왕복.
- 디스패처 결과 다섯 가지와 중복 등록 예외.
- `utf8::is_valid`: UTF-8 한국어와 4바이트 문자는 통과. CP949 바이트, 잘린 시퀀스, overlong 형식, 서로게이트는 실패.

### smoke_test

실제 서버를 띄우고 실제 TCP 소켓으로 접속하는 종단 간 테스트. 서버 로직을 바꾼 뒤 가장 먼저 돌린다.

- `ServerLib`를 링크해 같은 프로세스에서 포트 0(임시)으로 서버를 띄운다.
- 시나리오 코루틴은 메인 스레드의 별도 `io_context`에서 돈다.
- 서버에 직접 묻는 것은 `session_count()`뿐이다. 나머지는 모두 소켓을 거친다.
- `main()`이 `server_log`를 콘솔 전용으로 켜므로 실패한 실행에서 서버가 본 것이 보인다.
- VS에서 시작 프로젝트로 두고 F5를 누르면 한 프로세스에서 서버 코드를 디버그할 수 있다.

`test_main_flow`:

- 닫힌 터널에 DATA → FAILED.
- 서비스하지 않는 터널(REGION)에 CONNECT → FAILED.
- LOBBY anycast CONNECT → sid `0.0.11.1`로 수락.
- 로그인 전 채팅 → `chat_res{NOT_LOGGED_IN}`.
- 빈 이름 → `INVALID_NAME`.
- 클라이언트 둘이 다른 id로 로그인.
- 중복 로그인 → `ALREADY_LOGGED_IN`.
- 채팅 → 보낸 쪽은 `chat_res` 다음 `chat_noti`, 다른 클라이언트는 `chat_noti`.
- UTF-8 한국어 채팅이 그대로 브로드캐스트된다.
- CP949 바이트는 `INVALID_TEXT`로 거부되고 브로드캐스트되지 않는다.
  여기서 protobuf가 찍는 `invalid UTF-8` 한 줄은 예상된 것이다.
- NOOP/PING 하트비트 응답.
- 모르는 msgid와 모르는 패킷 타입(`type_` 0x7F)은 연결을 유지한다.
- DISCONNECT 뒤의 DATA → FAILED.
- 깨진 프레임(sid를 담기에 너무 짧은 CONNECT) → 연결 종료.
- 소켓을 닫으면 → 세션 수가 줄어든다.

`test_close_during_write`: 회귀 테스트. 서버가 세션을 닫는 것과 아직 전송 중인 응답 사이의 경쟁을 100번
일으킨다. "세션 생명주기"의 송신 큐 규칙 참고.

`test_timeout`: 타임아웃 300 ms의 서버. 하트비트가 오가는 동안 연결이 유지되고, 멈추면 끊긴다.

`test_listen_before_start`: `init_instance()`만 한 뒤 클라이언트가 접속해 하트비트를 보낸다. 300 ms 뒤에도 세션
수는 0이다. `start()` 뒤에 대기하던 연결이 accept되고 하트비트에 응답한다.

### app_test

`server_config`와 `server_app`의 테스트. 임시 포트 listen 외에는 소켓이 없다. 로거는 레벨 off로 돈다.

- 기본값. 명령줄의 정체성과 파일의 환경. 모르는 키(파일 안의 `sid` 포함), 잘못된 값, 빠진 값, 모르는 옵션
  (`--port` 포함), 깨진 JSON은 값 이름과 함께 거부. 양쪽의 모든 문제를 함께 보고.
- 전체 생명주기 trace: `up:` config, logger, connection, connections, asset, assets, listen, serve, 그다음 `down:`
  역순. `init_instance`와 `start` 사이에는 `listening()`이면서 `serving()`은 아님.
- 실패하는 연결이나 에셋: 뒤 단계는 실행되지 않고, 올라온 컴포넌트는 역순으로 내려가며, 앱은 listen하지 않는다.
  뜨지 못하는 로거(일반 파일 아래의 로그 폴더)는 `pre_init_instance`를 실패시킨다.
  명령줄에서 읽은 설정은 그대로 앱에 도달한다.

멈춤 방지:

- 세션 수 확인은 10 ms마다 최대 5초 폴링한다.
- 클라이언트 이벤트 루프는 `run_for`로 제한한다.
- CTest 타임아웃 60초.

덮지 않는 것: 동시성 부하(→ `DummyClient`), 송신 큐 256 초과 경로.

참고: 세션 수 폴링은 코루틴 안에서 스레드를 재우므로 그동안 클라이언트 이벤트 루프가 멈춘다.
기다리는 동안 계속 받아야 하는 경우는 타이머 기반 `sleep_for` 코루틴을 쓴다.

## 기원

- zeliard/EasyGameServer(MIT, 2013)를 줄여 다시 쓴 것. 고지는 `LICENSE` 끝에 있다.
- 2020, 첫 버전: WinSock2 overlapped I/O + APC 서버.
- 2026-09-17: asio + CMake로 완전히 다시 씀.
- 2026-09-19: struct memcpy 프로토콜을 LPN 프레이밍 + protobuf로 교체.
- 2026-09-20: 중복 길이 필드 제거, `type_`를 값 하나당 연산 하나로 평탄화.
- DummyClient의 동작(로그인 뒤 매초 채팅, `UserName<n>` 이름)은 EasyGameServer에서 왔다.

## 설계

### 와이어 프로토콜 (`common/lpn/`)

프로토콜은 `docs/protocol.ko.md` 한 곳에 정의되어 있다: 패킷 배치(3절), `type_`와 하트비트 상수(4), 터널 패킷
동작(5), 서버 sid(6), msgid(7), 필드·서버 처리·msgid 값을 포함한 LOBBY 메시지(8), 에러 코드(9), 흐름(10), 바이트
예시(11). 이 절은 서버 코드가 의존하는 것만 적는다.

- 모든 다중 바이트 정수는 **big-endian**이며 바이트 단위로 조립한다. struct memcpy는 없다.
- 패킷은 `[frame_len][type_][param_]` 뒤에, 터널 패킷(`type_` 1-5)이면 `server_sid`가, DATA면 `msgid`와 protobuf
  본문이 따른다. 하트비트(`type_` 6)는 6바이트다.
  코드에서는 `lpn::is_tunnel_packet(type)` / `frame::is_tunnel()`.
- 터널 id ≥ 32면 디코더가 `protocol_error`를 던진다. 세션이 그 값으로 터널 표를 인덱싱하기 때문이다.
- **터널 id(터널 패킷의 `param_`, 0-31) = 서버 타입**: 2 GWS, 4 AUTH, 9 QUEST, 10 ITEM, 11 LOBBY,
  13 REGION, 14 AI. 이 서버는 LOBBY만 서비스한다.
- **`server_sid`**: 16비트 × 4, 텍스트 형식 `domain.idc.type.id`. `type`은 터널 id와 같다. `id` 0은 anycast.
  서버 기본값 `0.0.11.1`.
- **msgid**: 패키지를 포함한 전체 이름의 `fnv1a32`, 예: `"chat.login_req"`. 기본 protoc를 쓰며 런타임이 알려 주는
  이름을 등록 시점에 해시하므로 손으로 관리하는 id 표가 없고, 이름을 바꾼 메시지는 바로 "unknown msgid"로 드러난다.
- **흐름**: TCP 접속 → CONNECT(LOBBY, anycast `0.0.11.0`) → 서버의 CONNECT(`0.0.11.1`) →
  `login_req`/`login_res` → `chat_req` → `chat_res` + `chat_noti`. 클라이언트는 3초마다 NOOPREQ를 보낸다.
  서버는 패킷이 5초간 없으면 연결을 끊는다.

**모든 문자열은 UTF-8이다.** protobuf `string` 필드의 규칙이다.

- proto2에서 protobuf는 잘못된 문자열을 오류 로그만 남기고 통과시키므로, 서버가 `utf8::is_valid`로 검증해 거부한다.
- Windows 콘솔은 입력을 현재 코드 페이지(한국어 시스템은 CP949)로 전달한다. `NetworkClient`는 이 헬퍼로 변환한다:
  - `console::read_line` - 콘솔이면 `ReadConsoleW` → UTF-8, 파이프면 그대로 통과.
  - `console::arg_to_utf8` - 명령줄 인자.
  - `console::init_utf8_output` - 콘솔 출력 코드 페이지를 UTF-8로 바꾸고 종료 시 되돌린다.
- 콘솔 **입력** 코드 페이지를 UTF-8로 바꾸는 방법은 쓰지 않는다. 오래된 conhost가 한국어를 NUL로 전달한다.
- 사람 입력이나 이름을 다루는 새 실행 파일은 같은 헬퍼를 쓴다.

와이어 상수는 `common/lpn/wire.h`에만, 바이트 배치는 `common/lpn/frame.h`에만 있다. 와이어를 바꾸거나 메시지를
추가하는 법은 아래 "변경 절차"에 있다.

### `common/lpn/`의 파일

| 파일 | 책임 |
|---|---|
| `wire.h` | 모든 와이어 상수, 하트비트 주기와 타임아웃, `MAX_FRAME_SIZE`(1 MiB), big-endian `put_*`/`get_*` |
| `sid.h` | `lpn::sid` 조립/분해/텍스트/파싱, anycast 판정 |
| `frame.h` | `lpn::frame`, `make_tunnel`, `make_heartbeat`, `encode`, `decode_body`. 깨진 프레임은 `protocol_error`를 던짐 |
| `msgid.h` | `fnv1a32`, `msgid_of<M>()`, `encode_message`(msgid + 본문) |
| `frame_io.h` | asio 코루틴 `read_frame`(할당 전 크기 검사), `async_write_frame`, 동기 `write_frame`, `shared_buffer` |
| `dispatcher.h` | `message_dispatcher<Ctx>`. 함수 포인터 하나를 등록하면 시그니처에서 메시지 타입과 msgid를 추론. 결과: `ok`/`too_short`/`unknown_msgid`/`parse_error`/`not_initialized` |

와이어와 무관한 구현 규칙은 `docs/protocol.ko.md` 14절에 있다: 최대 크기 강제, 모르는 msgid 로그, 닫힌 터널의
DATA에 FAILED로 응답 등.

### 서버 스레드 모델

- `io_context` 하나를 `threads`개(환경 파일)의 워커가 돌린다.
- 세션마다 strand 하나. 소켓은 그 strand를 executor로 쓴다. 세션 멤버는 strand 위에서만 건드린다.
  세션 안에는 락이 없다.
- `session_manager`는 mutex 아래의 map 하나다. 락을 잡은 채로 세션 메서드를 부르지 않는다.
- 브로드캐스트: 스냅샷을 뜬 뒤 세션마다 `asio::dispatch(strand, ...)`. 그 터널이 열린 세션만 받는다.
  인코딩된 패킷 하나(`shared_buffer`)를 모든 수신자가 공유한다.
- 다른 스레드에서 세션을 건드리는 것은 모두 같은 방식으로 dispatch해야 한다.

### 세션 생명주기 (`ServerLib/session.cpp`)

1. accept → `make_shared<session>` → `manager.add` → `start()`.
2. `start()`가 strand 위에서 `run()`(수신 루프)과 `watchdog()`(타임아웃) 코루틴을 띄운다.
3. `run()`은 `read_frame` → `on_frame`을 반복한다. I/O 오류와 `protocol_error`는 `close(reason)`으로 모인다.

세션은 터널 표(`터널 id → 바인딩된 sid`, 크기 32, 0 = 닫힘)를 가진다. 게이트웨이를 분리하면 그쪽이 이 표를 갖는다.

- CONNECT: 터널 id와 요청한 sid의 type이 서버 sid의 type과 같고, 요청이 anycast이거나 서버 sid와 정확히 같으면
  수락. 표에 기록하고 실제 sid를 담은 CONNECT로 응답. 아니면 FAILED.
- DATA: 터널이 닫혀 있으면 FAILED. 아니면 LOBBY 디스패처에 넘긴다.
  `unknown_msgid`/`not_initialized`는 로그만 남기고 무시. `too_short`/`parse_error`는 연결을 닫는다.
- DISCONNECT: sid가 표의 값과 같을 때만 터널을 닫고 DISCONNECT로 응답.
- 하트비트: NOOPREQ → NOOPRES, PINGREQ → PINGRES.
- 받은 모든 패킷이 마지막 수신 시각을 갱신한다.

**송신 큐 규칙:**

- `send()`는 큐에 넣고, 송신 코루틴이 돌고 있지 않으면 띄운다. 큐에 256개가 넘게 쌓이면 연결을 닫는다.
- `write_loop`는 **await 전에 버퍼를 pop**해 코루틴이 소유하게 한다. `close()`가 전송 중에 큐를 비울 수 있어서다.
- await 뒤에 pop하면 빈 deque를 건드려 다섯 번에 한 번꼴로 크래시했다.
  회귀 테스트는 `smoke_test`의 `test_close_during_write`.
- co_await를 사이에 두고 컨테이너 상태를 가정하지 않는다.

`close()`는 멱등이다: 워치독 타이머를 취소하고, 소켓을 닫고, 큐와 터널 표를 비우고, map에서 세션을 뺀다. map과
돌고 있던 코루틴이 참조를 놓으면 객체가 해제된다.

### 서버 시작/정지 (`ServerLib/server.cpp`)

- `server(server_options)`. 옵션: `ip`, `port`, `threads`, `sid`, `session_timeout`.
- 생성자가 LOBBY 핸들러를 등록한다. msgid 충돌은 예외를 던진다.
- `init_instance()`: open/bind/listen. 포트를 잡고 OS가 연결을 줄 세운다. accept는 하지 않는다.
- `start()`: 시그널 핸들러, accept 코루틴, 워커 스레드를 띄우고 바로 반환한다.
  `init_instance()` 없이 부르면 `std::logic_error`를 던진다.
- `stop()`: "acceptor 닫기 + 모든 세션 닫기"를 io_context에 post한다. 막지 않는다.
- `wait()`: join.
- `exit_instance()`: listen 소켓을 닫는다. `wait()` 뒤에, 또는 `start()`를 부른 적이 없을 때.
- SIGINT/SIGTERM은 `signal_set`으로 들어와 `stop()`을 부른다.
- 포트 0은 임시 포트를 고른다. `port()`로 읽는다(테스트용).
- 서버 하나가 게이트웨이와 LOBBY 백엔드를 겸한다. 게이트웨이를 별도 프로세스로 나누고 서버 간 구간을 설계하는 것은
  나중이다(이슈 #13). 지역별 게이트웨이는 이슈 #19. sid의 `idc` 부분은 그것을 위해 남겨 두었다.

### 클라이언트

- 둘 다 단일 스레드 `io_context`에서 코루틴 셋을 돌린다: 수신 루프, 하트비트, (DummyClient는) 채팅 타이머.
- 송신은 동기 `write_frame`이다.
- 받은 DATA는 클라이언트 자신의 `message_dispatcher`를 거쳐 멤버 함수 핸들러로 간다.
- `NetworkClient`는 메인 스레드에서 stdin을 읽고 `asio::post`로 io 스레드에 줄을 넘긴다.

## 테스트

CTest에 넷이 등록되어 있다.

- `wire_test`, `smoke_test`, `app_test` - 실행 파일. "프로젝트" 참고.
- `check_vs_sync` - vcxproj와 CMake의 소스 목록이 같은지 확인하는 CMake 스크립트(`tests/check_vs_sync.cmake`).
  VS 솔루션에는 보이지 않는다.

```
ctest --preset windows-msvc                                   # all
ctest --preset windows-msvc -R smoke_test --output-on-failure  # one
.\build\vs\Debug\bin\smoke_test.exe                            # run the VS build directly
```

- 모든 테스트는 `main()` 첫 줄에서 `test_support::init()`(`tests/test_support.h`)을 부른다.
  새 테스트 실행 파일도 같이 해야 한다.
  - 출력 버퍼링을 끈다.
  - Windows에서 CRT 단언, `abort`, 접근 위반이 **대화상자 없이** stderr로 간다.
- 간헐적 실패는 실행 파일을 수십 번 돌리며 실패 횟수를 세어 조사한다
  (`$LASTEXITCODE`를 모으는 PowerShell `for` 루프).

### 대화형 경로

자동 테스트가 덮지 않는다. 테스트의 stdin이 콘솔이 아니기 때문이다.

- Windows 콘솔에 한국어 입력(`console::read_line`): 사용자가 확인(2026-09-19).
- Windows 서버에서 ESC(`console::key_watcher`): 사용자가 확인(2026-09-19).
  ESC → 세션 종료 → `server stopped` → 종료 코드 0.
- `common/console.h`의 POSIX 경로(termios, poll 기반 ESC 감시)는 Linux와 macOS에서 컴파일만 했다.
  실제 키 입력으로는 써 보지 않았다. 자동 실행은 stdin이 콘솔이 아니라서 비활성 상태로 남는다.

## 변경 절차

**메시지 추가.** msgid는 자동이다.

1. `ProtoLib/chat.proto`에 정의한다.
2. `ServerLib/lobby_service.cpp`에 `void on_xxx(session&, const chat::xxx&)`를 쓰고 `register_lobby_handlers`에
   `regist` 한 줄을 추가한다.
3. 클라이언트에서는 생성자에 `lobby_.regist(&client_session::on_xxx)`.
4. `docs/protocol.ko.md` 8절을 갱신한다(msgid 값 포함).

**와이어 변경**(`docs/protocol.ko.md` 15.3절). 모든 클라이언트가 함께 바뀌어야 한다.

1. `wire.h`/`frame.h`를 고친다.
2. `wire_test`가 실패하는 것을 확인한 뒤 기대 바이트를 고친다.
3. `docs/protocol.ko.md`의 표와 11절 예시, 이 문서의 와이어 프로토콜 절을 고친다.
   `wire_test`의 `test_documented_examples`가 예시를 구현과 대조한다.

**시작 단계 변경.** `ServerLib/server_app.cpp`의 `PHASES[]`에 행 하나, 멤버 함수 둘, 같은 위치의 `phase_id` 값
하나를 추가한다. `static_assert`가 표와 enum의 크기를 같게 지킨다. 이 문서의 단계 표와
`tests/app_test.cpp`의 기대 trace를 갱신한다.

소스 파일 추가, `.proto` 추가, 변경 검증: `docs/build.ko.md`의 "변경 절차".
