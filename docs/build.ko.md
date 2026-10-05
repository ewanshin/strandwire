# 빌드와 실행

[English](build.md) | 한국어

OS별로 CMake 또는 Visual Studio 솔루션으로 빌드하는 법과 서버, 클라이언트, 테스트를 실행하는 법.
타깃이 무엇인지는 `docs/architecture.ko.md`에 있다.

**두 빌드 체계를 모두 유지한다.** CMake 프리셋과 손으로 관리하는 Visual Studio 솔루션이다. 빌드에 영향을 주는
변경은 둘 다에 넣는다(`CLAUDE.ko.md` "규칙").

## 빌드

요구 사항: CMake 3.21+, Ninja, C++20 컴파일러(MSVC 2022+, GCC 11+, Clang 14+).

CMake 최소 버전은 3.21로 고정한다. Ubuntu 22.04 LTS에 기본으로 들어 있는 CMake(3.22)로 빌드되게 하기 위해서다.
3.25 이후 기능(`add_subdirectory(... SYSTEM)`, 프리셋 스키마 4+)은 쓰지 않는다.

```
git submodule update --init
cmake --preset windows-msvc        # or linux / macos
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

- 출력: `build/<preset>/bin/`.
- 테스트 하나만: `ctest --preset windows-msvc -R smoke_test --output-on-failure`.
- 첫 빌드는 protobuf와 protoc를 소스에서 컴파일한다. 약 180개 타깃, 몇 분이 더 걸린다.
- CMake 4에서 나오는 protobuf의 `cmake_minimum_required` deprecation 경고는 무해하다.

### Windows (CMake)

Ninja 프리셋은 VS 개발자 환경이 필요하다. 일반 PowerShell에서:

```
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>&1 && cmake --preset windows-msvc && cmake --build --preset windows-msvc && ctest --preset windows-msvc"
```

- VS 2022만 있는 PC: `...\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`.
- git bash의 `cmd //c`로 부르면 조용히 실패한다. PowerShell을 쓴다.

### Linux (WSL2에서 검증)

WSL2 Ubuntu 22.04 (g++ 11.4, CMake 3.22, Ninja 1.10). `linux` 프리셋은 경고 0으로 빌드되고 네 테스트가 모두
통과한다.

```
rsync -a --delete --exclude .git --exclude .vs --exclude build \
      --exclude third_party/_install --exclude third_party/_build /mnt/<drive>/<repo>/ ~/strandwire/
cd ~/strandwire && cmake --preset linux && cmake --build --preset linux && ctest --preset linux
```

- `/mnt/<drive>`(Windows 파일 시스템)에서 직접 빌드하면 매우 느리다. 먼저 WSL 홈으로 복사한다.
- `.vs`는 Visual Studio가 잠그고 있다. 복사에서 제외한다.
- PowerShell에서 WSL을 부르면 `$` 변수와 따옴표가 깨진다. 스크립트를 파일에 두고
  `wsl.exe -d Ubuntu -- bash /mnt/c/.../script.sh`로 실행한다.
- `sudo`는 비밀번호가 필요해 자동화할 수 없다. 패키지는 사용자가 직접 설치한다.
- WSL2 안의 서버는 Windows에서 `127.0.0.1:<port>`로 접속된다. 교차 접속 확인에 쓴다.

### macOS (SSH로 검증)

LAN의 Mac에 있는 클론(`<user>@<mac-host>`, 키 인증). `macos` 프리셋은 경고 0으로 빌드되고 네 테스트가 모두
통과한다.

```
tr -d '\r' < script.sh | ssh -o BatchMode=yes <user>@<mac-host> 'bash -s'
# inside script.sh:
cd <repo>
export PATH="/opt/homebrew/bin:$PATH"
git submodule update --init --depth 1
cmake --preset macos && cmake --build --preset macos && ctest --preset macos
```

- Mac의 저장소는 GitHub 클론이다. **검증할 커밋을 먼저 origin에 push해야 한다.**
- 비대화형 SSH 셸에는 Homebrew가 PATH에 없다. 스크립트에서 `/opt/homebrew/bin`을 앞에 붙인다.
- 스크립트는 표준 입력으로 파일째 보낸다. Windows에서 만든 파일은 CR을 제거한다.
- Mac의 방화벽이 꺼져 있어서 listen하는 서버가 권한 대화상자를 띄우지 않는다.
  켜져 있다면 서버나 `smoke_test`를 돌리기 전에 사용자에게 알린다.
- 교차 접속: Windows에서 `DummyClient --ip <mac-host>`.

### Visual Studio 솔루션 (Windows, 손으로 관리)

일상적인 Windows 개발은 루트의 `strandwire.sln`으로 한다. CMake가 생성하지 않고 손으로 관리한다.

- 9개 프로젝트: `LogLib`, `ProtoLib`, `ServerLib`, `NetworkServer`, `NetworkClient`, `DummyClient`,
  `wire_test`, `smoke_test`, `app_test`.
- 구성 `Debug|x64` / `Release|x64`, 툴셋 v143.
- vcxproj 파일은 각 소스 폴더에 있다. 출력: `build\vs\<Configuration>\bin\`.

**새 PC에서는 먼저 protobuf를 한 번 설치한다.** protobuf 서브모듈 버전을 바꾼 뒤에도 다시 실행한다.

```
powershell -ExecutionPolicy Bypass -File tools\build_protobuf.ps1
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" strandwire.sln /m /p:Configuration=Debug /p:Platform=x64
.\build\vs\Debug\bin\smoke_test.exe
```

protobuf:

- 솔루션은 protobuf를 빌드하지 않는다. 설치된 결과물을 링크한다.
- 스크립트는 Debug와 Release를 `third_party\_install\protobuf\<Configuration>\`에 설치한다
  (`bin\protoc.exe`, `lib\libprotobuf(d).lib`, `include\`).
- 설치본이 없으면 `ProtoLib` 빌드가 스크립트를 실행하라는 메시지와 함께 실패한다.
- 설치 폴더와 `third_party\_build\`는 gitignore되어 있다.

설정:

- 공유 설정은 props 파일 두 개에 있다. 값은 루트 `CMakeLists.txt`와 같게 유지한다.
  - `strandwire.config.props` - 툴셋, 문자 집합, 디버그 런타임. `Microsoft.Cpp.props`보다 먼저 import된다.
  - `strandwire.props` - include 경로, 전처리기 정의, C++20, `/W4 /utf-8`, 출력 디렉터리,
    protobuf 경로와 링크.
- 개별 vcxproj에는 컴파일러 옵션을 두지 않는다. props를 고친다.
  유일한 예외는 생성 코드의 경고를 끄는 `ProtoLib.vcxproj`다.
- protobuf 헤더, 생성 코드 폴더, `third_party\spdlog\include`는 `ExternalIncludePath`에 있어
  `/W4` 경고에서 제외된다.
- 경로는 `$(SolutionDir)`가 아니라 props 파일 기준 상대 경로라서 vcxproj 하나만으로도 빌드된다.
- 미리 컴파일된 헤더는 쓰지 않는다.

참조:

- spdlog는 header-only라서 사전 빌드 단계가 없다. `LogLib\logger.cpp`만 include한다.
- 로그를 쓰는 실행 파일은 `LogLib`를 직접 참조한다. `ProtoLib`를 쓰는 실행 파일은 그것을 직접 참조한다.
- `ServerLib`의 `ProtoLib` 참조는 빌드 순서 때문이다(`LinkLibraryDependencies=false`). 라이브러리는 합쳐지지 않는다.

소스 파일을 추가하거나 지우면 세 곳(`CMakeLists.txt`, `.vcxproj`, `.vcxproj.filters`)을 건드린다. 어긋나면 CTest
`check_vs_sync`가 실패한다. 절차는 `CLAUDE.ko.md` "변경 절차"에 있다.

## 실행

```
.\build\windows-msvc\bin\NetworkServer.exe [--ip 0.0.0.0] [--sid 0.0.11.1] [--config lobby_config.json]
.\build\windows-msvc\bin\NetworkClient.exe 127.0.0.1 10000 <name>     # a line = one chat message, quit = exit
.\build\windows-msvc\bin\DummyClient.exe --ip 127.0.0.1 --port 10000 --session 100 --duration 10
```

- 서버의 포트, 스레드, 타임아웃, 로그 설정은 `--config` 파일에서 온다
  (`docs/architecture.ko.md`, "NetworkServer").
  파일이 없으면 10000번 포트에서 CPU 코어당 워커 하나로 listen한다.
- 두 클라이언트의 옵션:
  - `--log-level trace|debug|info|warn|error|fatal|off` (기본 info).
  - `--log-dir <folder>` (기본 없음 = 콘솔만).
- 서버는 콘솔에서 ESC나 Ctrl+C로 깨끗하게 멈춘다.
- DummyClient의 세션별 `LOGIN OK`는 debug 레벨이라 기본으로는 숨겨진다.
- DummyClient는 `--duration` 초 뒤에 끝난다. 모든 세션이 로그인했으면 종료 코드 0.
- NetworkClient에 stdin을 파이프로 넣으려면 git bash를 쓴다:
  `(sleep 1; echo hello; sleep 1; echo quit) | ./NetworkClient.exe 127.0.0.1 10000 alice`.
  PowerShell 5.1의 스크립트 블록 파이프는 네이티브 프로세스의 stdin에 제때 닿지 않아 멈춘다.
