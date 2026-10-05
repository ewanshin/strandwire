# CLAUDE.md

[English](CLAUDE.md) | 한국어

이 파일은 Claude Code(claude.ai/code)가 이 저장소의 코드를 다룰 때 따르는 안내다. Claude가 읽는 원본은
`CLAUDE.md`(영어)이고, 이 파일은 사람이 읽기 위한 한국어판이다.

사용자와는 항상 한국어로 대화한다. 코드, 로그 문자열, 커밋 메시지, 이슈는 영어다.

**모든 문서는 한국어판을 가진다**(2026-10-06 결정): `README.md`/`README.ko.md`, `docs/*.md`/`docs/*.ko.md`,
`docs/specs/*.md`/`*.ko.md`, `CLAUDE.md`/`CLAUDE.ko.md`. 둘은 같은 내용을 말한다. **한쪽이 바뀌면 같은 커밋에서
다른 쪽에도 같은 변경을 적용한다.** 모든 문서의 3번째 줄은 언어 링크다
(`English | [한국어](x.ko.md)` / `[English](x.md) | 한국어`).

**모든 코드 주석은 두 언어다**: 영어 주석 뒤에 한국어 번역이 따른다("규칙"의 "주석"). 새 주석도 처음부터 그렇게
쓴다.

## 프로젝트 목표

개인 네트워크 서버/클라이언트 프로젝트.

- 서버: C++
- 클라이언트: 순수 C++ 또는 Godot. cocos2d-x는 그 위에서 돈다.
- 지원 OS: Windows / macOS / Linux

현재 상태(`v0.3.0`, 2026-09-22):

- `v0.2.0`(2026-09-19): asio + CMake 이식, LPN 프레이밍 + protobuf 프로토콜.
- `v0.3.0`(2026-09-22): 중복 길이 필드 제거, `type_`를 값 하나당 연산 하나로 평탄화(와이어 변경).
- 빌드, 테스트, 실행, Windows 클라이언트 교차 접속을 세 OS 모두에서 검증
  (와이어 변경 뒤: Windows와 Linux 2026-09-20, macOS 2026-09-22).
  - Windows: MSVC. CMake와 VS 솔루션 둘 다.
  - Linux: WSL2 Ubuntu 22.04, g++ 11.4, CMake 3.22.
  - macOS: 27.0 arm64, Apple clang 21, CMake 3.31.
- Godot/cocos2d-x 클라이언트는 아직 없다.

작업 관리: GitHub 이슈. **순서와 우선순위는 고정된 로드맵 이슈 #12에 있다.** 작업을 시작하기 전에 #12를 읽고,
끝나면 해당 행을 완료로 표시한다. 기술 블로그 글감은 이슈 #11의 코멘트로 모은다.

## 문서

이 파일에는 코드를 바꾸는 규칙만 있다. 코드가 무엇인지는 `docs/`에 있다:

| 문서 | 읽는 목적 |
|---|---|
| `docs/architecture.md` | 빌드 타깃 9개와 각각의 역할, 와이어 프로토콜 요약, 스레드 모델, 세션과 서버의 생명주기, 시작 단계와 설정, 각 테스트가 확인하는 것 |
| `docs/build.md` | Windows, Linux(WSL2), macOS(SSH)에서의 CMake 빌드, 손으로 관리하는 VS 솔루션과 protobuf 설치, 실행 파일과 테스트 실행 |
| `docs/protocol.md` | **프로토콜의 기준 문서.** 바이트 배치, 상수, 메시지와 msgid, 에러 코드, 흐름, 바이트 예시, 다른 언어로 클라이언트를 쓸 때의 체크리스트(1-12절), 설계 결정과 변경 가이드(13-16절) |
| `docs/specs/2026-09-17-asio-cmake-migration-design.md` | asio/CMake 이식 결정, protobuf 빌드 통합(4.2절), 검토한 대안(부록 A). 5절(옛 struct 프로토콜)은 `docs/protocol.md`로 대체되었다 |
| `README.md` | 공개 첫 페이지 |

각 문서 옆에 한국어판(`*.ko.md`, `CLAUDE.ko.md`)이 있다. 하위 시스템을 바꾸기 전에 `docs/architecture.md`의 해당
절을 읽는다. 한 줄로: standalone asio 1.38.2 + C++20 코루틴 위의 TCP 채팅 서버와 클라이언트 둘. 프로토콜은
protobuf 본문을 담는 LPN 프레이밍이다.

## 규칙

구조:

- **두 빌드 체계, VS 솔루션과 CMake를 모두 유지한다.** 어느 쪽도 버리자고 제안하지 않는다.
  - 서버는 지금 Windows에서 개발하므로 VS 솔루션(`strandwire.sln`)이 있다.
  - Linux/macOS에서도 개발할 수 있으므로 `CMakeLists.txt`와 프리셋이 있다.
  - 빌드에 영향을 주는 변경(소스 추가/삭제, 옵션, 의존성)은 둘 다에 넣고 둘 다 검증한다.
- include 루트는 저장소 루트다: `"common/lpn/frame.h"`, `"ServerLib/session.h"`.
  생성된 protobuf 헤더만 `"chat.pb.h"`처럼 경로 없이 include한다.
- 폴더 하나 = 타깃 하나 = vcxproj 하나. 서버 기능은 `ServerLib/`에 넣고, `NetworkServer/`에는 실행 파일 전용
  코드만 둔다.
- 라이브러리 코드(`ServerLib`, `common/lpn`)에는 플랫폼 분기를 두지 않는다.
  `windows.h`는 `tests/test_support.h`와 `common/console.h`에서만, `#ifdef _WIN32`로 감싸서 쓴다.
- 와이어 상수는 `common/lpn/wire.h`에만, 바이트 배치는 `common/lpn/frame.h`에만 있다.
  다른 파일에는 오프셋이나 상수 값이 나오면 안 된다.
- 사람 입력이나 이름을 다루는 새 실행 파일은 `common/console.h`의 `console::` 헬퍼(`read_line`, `arg_to_utf8`,
  `init_utf8_output`)를 쓴다. 모든 문자열은 UTF-8이다.

로그와 문자열:

- `std::cout`/`std::cerr`가 아니라 `server_log`/`client_log`로 로그를 쓴다.
  여러 워커 스레드가 `std::cout << a << b`를 쓰면 줄 중간에 섞인다. 세션 100개가 한꺼번에 끊길 때 실제로 일어났다.
- 예외는 프로그램 자체의 출력이다: NetworkClient의 프롬프트와 `CHAT from ...`, DummyClient의 마지막 통계 줄
  (스크립트가 파싱), usage 메시지, 테스트의 `CHECK`, 그리고 로거가 돌지 않는 동안 `server_app`이 찍는
  `[NetworkServer]` 줄(`note()`/`note_error()`; `docs/architecture.md`, "NetworkServer").
- `std::error_code::message()`를 로그에 쓰지 않는다. `netsys::describe(ec)`(`common/error_text.h`)를 쓴다.
  예: `connection reset by peer (asio.system:10054)`.
  system 카테고리 메시지는 OS가 사용자 언어와 ANSI 코드 페이지(CP949)로 주므로 UTF-8 콘솔에서 깨진다.
- 로그 문자열은 영어다(Windows 콘솔 코드 페이지 문제).
- 소스는 UTF-8이다. 줄 끝은 `.gitattributes`의 `text=auto`에 맡긴다.
- `main()`은 워커 스레드가 시작되기 전에 `server_log.start(...)`/`client_log.start(...)`를 부르고, join된 뒤
  `main()`이 반환하기 전에 `stop()`을 부른다. 그 밖의 로그는 조용히 버려진다.

동시성:

- 세션 메서드를 그 strand 밖에서 부르지 않는다. `session_manager::broadcast`가 올바른 방법을 보여 준다.
- `co_await`를 사이에 두고 멤버 컨테이너가 그대로라고 가정하지 않는다. 같은 strand의 다른 핸들러(특히
  `close()`)가 그 사이에 돌 수 있다.

언어와 라이브러리:

- asio의 `get_executor()`가 non-const라서 `session::strand()`도 non-const다.
- enum은 소문자 `enum class`다. `FAILED`, `ERROR` 같은 Windows 매크로와의 충돌을 피한다.
- 쓰지 않는 것: `strcpy_s` 같은 MSVC 전용 CRT, 직접 `WinSock2.h` include, `std::format`(GCC 13 전에는 미지원).
- `ASIO_NO_DEPRECATED`가 켜져 있다. `io_service` 같은 옛 이름은 컴파일되지 않는다.
- protobuf는 3.21.12로 고정한다. 22+는 Abseil에 의존해 손으로 관리하는 VS 솔루션에 맞지 않는다
  (`docs/protocol.md` 16절, P2).
- CMake 최소 버전은 3.21을 유지한다. Ubuntu 22.04 LTS 기본 CMake(3.22)로 빌드되게 하기 위해서다.
  3.25 이후 기능(`add_subdirectory(... SYSTEM)`, 프리셋 스키마 4+)은 쓰지 않는다.

포매팅(2026-10-02 결정). 루트의 `.clang-format`이 규칙이다. 건드린 모든 C++ 파일에 clang-format을 돌린다
(`third_party/` 제외). Visual Studio에 `VC\Tools\Llvm\x64\bin\clang-format.exe`로 들어 있다.

- **모든 여는 중괄호는 자기 줄에 둔다**: 함수, 람다, `if`/`else`/`for`/`while`/`do`, `try`/`catch`, `switch`의
  `case`, 클래스, 네임스페이스. 모든 함수에 같은 규칙이므로 한 줄짜리 본문도 없다.
- 4칸 들여쓰기, 120칸, `char* p`, `template <class T>`는 자기 줄에, 생성자 초기화 목록은 한 줄에 하나.
- 전체 트리 검사:
  `git ls-files '*.cpp' '*.h' | grep -v ^third_party | xargs clang-format --style=file --dry-run -Werror`.

주석(2026-10-06 결정). 모든 주석은 영어 뒤에 한국어가 따른다. `third_party/`는 제외.

- 블록 주석(연속된 `//` 줄, 파일 머리말 포함): 영어 블록 다음에 같은 들여쓰기의 한국어 블록. 영어 블록이 두 줄
  이상이면 그 사이에 빈 `//` 한 줄을 둔다.
- 후행 주석(`code; // text`): ` / 한국어`를 덧붙인다. 줄이 120칸을 넘으면 주석을 문장 위의 자기 줄로 옮긴다.
  영어 줄 먼저, 그다음 한국어.
- 건드리지 않는 것: `\` 줄 이음이 있는 매크로 정의 안의 주석(한국어는 매크로 위의 블록에), `// _WIN32` 같은
  `#ifdef`/`#else` 표시, 문자열 리터럴, 로그 메시지, usage 텍스트, `CHECK` 텍스트.
- 단어가 아니라 뜻을 옮긴다. 평서문("~한다"), 짧은 문장. 코드에 가까운 용어는 영어 그대로: asio, strand,
  io_context, co_await, protobuf, msgid, sid, acceptor, 패킷 이름(CONNECT, DATA, ...).
  session → 세션, coroutine → 코루틴, tunnel → 터널, heartbeat → 하트비트, phase → 단계, config → 설정, logger → 로거.
- `.proto` 파일도 같은 규칙을 따른다.

생명주기 메서드 이름(2026-10-02 결정). 객체는 필요한 것만 갖되 항상 이 짝으로 쓴다:

| 메서드 | 뜻 | 되돌리는 것 |
|---|---|---|
| `pre_init_instance` | 다른 모든 것보다 먼저 필요한 것: 설정을 받아 검증, 로거 시작, 의존성 확인 | `exit_instance` |
| `init_instance` | 자원 확보: DB 연결, 에셋, 핸들러 등록, 소켓 bind/listen(아직 accept는 안 함) | `exit_instance` |
| `start` | 동작 시작: 스레드, accept, 타이머, 다른 서버로의 연결 | `stop` |

- `exit_instance`는 `init_instance`와 `pre_init_instance`가 확보한 것을 역순으로 해제한다. `stop`은 `start`가
  시작한 것을 끝낸다(join 포함). 해체는 구성의 역순이다.
- 연결 객체(`session`, 클라이언트 세션)는 `start`/`close`를 쓴다. 소켓을 닫는 것이 이 영역의 용어이고, 세션이
  시작한 모든 것을 끝낸다.
- 같은 목적으로 `init`, `shutdown`, `setup`, `teardown`, `run` 같은 이름을 새로 만들지 않는다.
- 종료 코드는 작은 양수이며 실패할 수 있는 생명주기 메서드마다 하나다(1 config/logger, 2 init, 3 start).
  음수는 쓰지 않는다. 종료 상태는 POSIX에서 8비트, Windows에서 부호 없는 32비트다.

설정:

- 서버의 명령줄은 인스턴스의 정체(`--ip`, `--sid`)와 환경 파일(`--config`)만 지정한다. 나머지는 파일의 키다.
  둘은 겹치지 않으므로 우선순위 규칙이 없다.
- 서버마다 스키마 파일 하나, `ProtoLib/<server>_config.proto`. 섹션마다 하위 메시지 하나(`log_config`,
  `listen_config`, ...). 모든 서버가 공유하는 섹션은 두 번째 서버가 생길 때 서버 파일이 `import`하는
  `base_config.proto`로 옮긴다.

테스트:

- 모든 테스트는 `main()` 첫 줄에서 `test_support::init()`(`tests/test_support.h`)을 부른다.
  새 테스트 실행 파일도 같이 해야 한다. 출력 버퍼링을 끄고, Windows에서 CRT 단언, `abort`, 접근 위반을
  **대화상자 없이** stderr로 보낸다.
- 서버 로직을 바꾼 뒤에는 `smoke_test`를 먼저, `common/lpn/`을 건드린 뒤에는 `wire_test`를 먼저 돌린다.
  그 기대 바이트열이 와이어 계약이다. 와이어를 의도적으로 바꿀 때만 고친다.
- 자동 테스트는 동시성이나 브로드캐스트 부하를 덮지 않는다. 서버의 스레딩이나 송신 경로를 바꾼 뒤에는
  `DummyClient`로 확인한다.
- 간헐적 실패는 실행 파일을 수십 번 돌리며 실패 횟수를 세어 조사한다
  (`$LASTEXITCODE`를 모으는 PowerShell `for` 루프).

문서 작성:

- 모든 문서(`CLAUDE.md`, `README.md`, `docs/`)는 간결하게 쓴다. 새 문서도 처음부터 그렇게 쓴다.
- 한 문장에 사실 하나. 나열은 목록으로, 옵션과 필드는 표로, 절차는 번호 목록으로.
- 결정이 어떻게 바뀌어 왔는지 서술하지 않는다. 이유는 한 줄만 남긴다.
- 간결함은 짧은 문장이지 적은 내용이 아니다. 식별자, 명령, 숫자, 규칙, 이유는 남긴다.
- 설계 스펙은 `docs/specs/`에 둔다. 구현 계획 문서는 작업이 끝나면 지운다.
- 모든 문서와 그 한국어판은 같은 내용을 말한다. 한쪽을 고치면 다른 쪽도 고친다. 어느 쪽에서 시작했든 마찬가지다.
  커밋 전에 둘 다 확인한다. 새 문서는 같은 커밋에서 두 언어로 만든다.
- 각 사실은 문서 한 곳에만 두고 다른 곳은 링크한다. 프로토콜 표는 `docs/protocol.md`에만 있다.
- 한국어 문서는 코드 블록, 식별자, 숫자, 경로, 표 모양을 영어 문서와 똑같이 유지한다.

작업 환경:

- bash heredoc으로 파일을 쓰지 않는다. Write/Edit 도구를 쓴다.
  - 백슬래시 시퀀스(`\v`, `\b`, ...)가 제어 문자로 바뀐다.
  - 여러 파일을 heredoc 한 묶음으로 쓰다가 파싱 오류가 난 적이 있다.
  - 같은 이유로 sed 치환에 백슬래시 경로를 쓰지 않는다.
- 셸 명령줄의 한국어는 깨져서 도착한다. 한국어(이슈 본문, 코멘트)는 UTF-8 파일에 넣고 파일을 넘긴다.
- 사용자 PC에 GUI 대화상자를 띄우는 것은 절대 실행하지 않는다. 크래시할 수 있는 Windows 실행 파일을 반복
  실행하기 전에 `test_support::init()`을 부르는지 확인한다.
- PowerShell에서 WSL을 부르면 `$` 변수와 따옴표가 깨진다. 스크립트를 파일에 둔다. Mac에는 CR을 제거한 파일을
  표준 입력으로 보낸다(`docs/build.md`).

## 변경 절차

**소스 파일 추가/삭제.** 세 곳을 고친다: 해당 `CMakeLists.txt`, `.vcxproj`, `.vcxproj.filters`.

- CTest `check_vs_sync`가 vcxproj의 `ClCompile` 목록과 CMake 타깃의 `SOURCES`를 비교해 어긋나면 실패한다.
- `ProtoLib`는 `CustomBuild`의 `.proto` 목록을 비교한다.
- 새 프로젝트(타깃)는 `tests/CMakeLists.txt`의 검사 목록과 `.sln`에도 추가한다.
- 개별 vcxproj에 컴파일러 옵션을 두지 않는다. `strandwire.props` / `strandwire.config.props`를 고치고 값을 루트
  `CMakeLists.txt`와 같게 유지한다. 유일한 예외는 생성 코드의 경고를 끄는 `ProtoLib.vcxproj`다.

**`.proto` 추가.** `ProtoLib/CMakeLists.txt`의 `NETSYS_PROTO_FILES`, `ProtoLib.vcxproj`의
`CustomBuild`/`ClCompile`, `.filters`에 추가한다. 패키지와 메시지 이름이 msgid를 결정한다: **이름을 바꾸면
와이어 계약이 바뀐다.** 규약(`docs/protocol.md` 8절): proto2, 소문자 snake_case, 접미사 `_req`/`_res`/`_noti`,
응답의 첫 필드는 `required int32 error_code_ = 1`, 에러 코드는 도메인별 100 단위 범위.

**메시지 추가.** msgid는 자동이다.

1. `ProtoLib/chat.proto`에 정의한다.
2. `ServerLib/lobby_service.cpp`에 `void on_xxx(session&, const chat::xxx&)`를 쓰고 `register_lobby_handlers`에
   `regist` 한 줄을 추가한다.
3. 클라이언트에서는 생성자에 `lobby_.regist(&client_session::on_xxx)`.
4. `docs/protocol.md` 8절을 갱신한다(msgid 값 포함).

**와이어 변경**(`docs/protocol.md` 15.3절). 모든 클라이언트가 함께 바뀌어야 한다.

1. `wire.h`/`frame.h`를 고친다.
2. `wire_test`가 실패하는 것을 확인한 뒤 기대 바이트를 고친다.
3. `docs/protocol.md`의 표와 11절 예시, `docs/architecture.md`의 와이어 프로토콜 절을 고친다.
   `wire_test`의 `test_documented_examples`가 예시를 구현과 대조한다.

**시작 단계 변경.** `ServerLib/server_app.cpp`의 `PHASES[]`에 행 하나, 멤버 함수 둘, 같은 위치의 `phase_id` 값
하나를 추가한다. `static_assert`가 표와 enum의 크기를 같게 지킨다. `docs/architecture.md`의 단계 표와
`tests/app_test.cpp`의 기대 trace를 갱신한다.

**변경 검증.** Windows에서 CMake와 VS 솔루션으로, Linux(WSL2)에서 빌드하고 CTest 테스트 넷을 돌린다. Mac이
켜져 있으면 macOS 검사도 돌린다. Mac의 저장소는 GitHub 클론이므로 먼저 push한다. 명령은 `docs/build.md`에 있다.

## 대화형 경로의 검증 상태

- Windows 콘솔에 한국어 입력(`console::read_line`): 사용자가 확인(2026-09-19).
- Windows 서버에서 ESC(`console::key_watcher`): 사용자가 확인(2026-09-19).
  ESC → 세션 종료 → `server stopped` → 종료 코드 0.
- `common/console.h`의 POSIX 경로(termios, poll 기반 ESC 감시)는 Linux와 macOS에서 컴파일만 했다.
  실제 키 입력으로는 써 보지 않았다. 자동 실행은 stdin이 콘솔이 아니라서 비활성 상태로 남는다.
