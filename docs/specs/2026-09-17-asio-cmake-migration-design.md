# asio + CMake migration design

- Written: 2026-09-17
- Status: approved and implemented (2026-09-17, verified on Windows). Later revisions are dated inline.
- Scope: the whole repository
- This document records the design at the time of the migration. The current structure is described in
  `docs/architecture.md`, the current protocol in `docs/protocol.md`.

## 1. Goal and scope

Goal: build and run the same sources on Windows, macOS and Linux.
Before: the server used WinSock2 overlapped I/O + APC, the build was MSBuild only. Windows only.

Scope:

- Rewrite the server (`NetworkServer`) on standalone asio 1.38.2.
- Move the clients (`NetworkClient`, `DummyClient`) to the same asio and remove MSVC-only CRT calls.
- Switch the build to CMake. Delete the old VS solution/project files. Create a new, hand-maintained VS
  solution for Windows (section 4.1).
- Add a smoke test that runs the server in-process.

Out of scope: TLS, `io_uring`, protocol changes, DB, Godot/cocos2d-x clients, CI.

## 2. Decision log

| # | Item | Decision | Reason |
|---|---|---|---|
| D1 | Migration scope | Whole repository | The clients were tied to NuGet asio 1.12.2 and `strcpy_s`. Migrating only the server leaves the clients unbuildable on mac/Linux |
| D2 | Code style | C++20 coroutines (`co_spawn` + `awaitable`) | First-class interface of current asio. Session logic reads like sequential code. Both installed MSVC versions (14.44/14.51) support it |
| D3 | Thread model | One `io_context` + N workers + one strand per session. Session registry under a mutex (approach A) | No locks inside a session. The only lock is the one map. B and C only add indirection at this scale (A.3) |
| D4 | Obtaining asio | git submodule `third_party/asio`, tag `asio-1-38-2` | No extra tools on any OS. Offline builds work |
| D5 | VS solution | Old solution deleted. Hand-maintained `strandwire.sln` kept alongside CMake (revised 2026-09-17) | The user wanted a hand-maintained solution, not CMake output. The first decision was "CMake only". Shared settings live in two props files; source-list drift is caught by the CTest `check_vs_sync` |
| D6 | Memory model | `std::shared_ptr` ownership. `ObjectPool` removed | Coroutines capture `self` to extend lifetime. A pool can be attached later as an allocator after measuring |
| D7 | Framing helper | Header/body read/write coroutines in `common/packet_io.h`, shared by server and clients | Removes the framing code duplicated across three executables |
| D8 | Source encoding | New files are UTF-8. `/utf-8` on MSVC | The files are being rewritten, so the CP949 rule is dropped |
| D9 | Server library location and name | Folder `ServerLib/`, target `ServerLib` (revised 2026-09-18) | At first the `netsys_server` sources lived in `NetworkServer/`. Two targets in one folder was confusing and headers had to be included as `"NetworkServer/server.h"`. PascalCase like the other folders |

## 3. Repository layout (at migration time)

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

Every folder has its own `CMakeLists.txt`.

- Deleted: `strandwire.sln`, `DummyClient.sln`, every `.vcxproj`/`.vcxproj.filters`/`packages.config`/`stdafx.*`/
  `targetver.h`/`ReadMe.txt`, `NetworkServer/ObjectPool.h`, `NetworkServer/ProducerConsumerQueue.h`,
  `common/stream.hpp`, `common/stream.cpp`.
- Directory names `NetworkServer`, `NetworkClient`, `DummyClient`, `common` are kept.
- Rule: one folder = one target = one vcxproj (D9).

## 4. Build

- Root `add_library(asio INTERFACE)`: exposes `third_party/asio/include`. Defines `ASIO_STANDALONE`, `ASIO_NO_DEPRECATED`.
  On Windows the same target adds `_WIN32_WINNT=0x0A00` and links `ws2_32`. Other targets only link `asio`.
- `CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_EXTENSIONS OFF`.
- Required versions: CMake 3.21 (lowered from 3.25 on 2026-09-19 so Ubuntu 22.04's 3.22 can build it),
  MSVC 2022, GCC 11, Clang 14.
- Warnings: MSVC `/W4 /utf-8`, others `-Wall -Wextra`. Not promoted to errors.
- `CMakePresets.json`: configure/build/test presets `windows-msvc`, `linux`, `macos`. Ninja.
  Build directory `build/<preset>`, executables in `build/<preset>/bin/`.
- Server logic is the static library `ServerLib`. `NetworkServer` is `main.cpp` only.
  The smoke test links the library and runs the server in-process.

```
git submodule update --init
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

### 4.1 Visual Studio solution (Windows, hand-maintained)

- Root `strandwire.sln`. Six projects at migration time: `ServerLib` (static lib), `NetworkServer`, `NetworkClient`,
  `DummyClient`, `framing_test`, `smoke_test`. Nine today (`docs/architecture.md`).
- Configurations `Debug|x64`, `Release|x64`. Toolset v143 (VS 2022). vcxproj and `.filters` sit in each source folder.
- Shared settings live in two props files. Values are kept equal to the root `CMakeLists.txt`.
  - `strandwire.config.props`: toolset, character set, debug runtime. Values that must be set before `Microsoft.Cpp.props`.
  - `strandwire.props`: include paths, preprocessor definitions, C++20, `/W4 /utf-8`, output directories.
- Each vcxproj holds only its GUID, output type, source list and project references.
- Paths are relative to the props file (`$(MSBuildThisFileDirectory)`), not `$(SolutionDir)`, so a vcxproj builds on its own.
- Output: `build\vs\<Configuration>\bin\`.
- No precompiled headers.
- When a file is added or removed, update the vcxproj (+filters) and `CMakeLists.txt` together.
  The CTest `check_vs_sync` (`tests/check_vs_sync.cmake`) compares the `ClCompile` list with CMake `SOURCES`.

### 4.2 protobuf build integration (added 2026-09-19)

CMake (all three OSes):

- Root: `add_subdirectory(third_party/protobuf EXCLUDE_FROM_ALL)`.
- Options: `protobuf_BUILD_TESTS=OFF`, `protobuf_WITH_ZLIB=OFF`, `protobuf_INSTALL=OFF`,
  `protobuf_BUILD_SHARED_LIBS=OFF`, `protobuf_MSVC_STATIC_RUNTIME=OFF` (this repository uses `/MD`).
- `libprotobuf`'s include path is set as `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` so protobuf headers stay out of `/W4`.
  `add_subdirectory(... SYSTEM)` is a CMake 3.25 feature and is not used (required version is 3.21).
- protobuf builds itself as C++11.
- `ProtoLib/CMakeLists.txt` creates a custom command calling `protobuf::protoc` per `.proto`
  and bundles the generated `.pb.cc` into the static library `ProtoLib`.
- Generated files live in the build directory (`<build>/ProtoLib/gen`) and are not committed. Include as `"chat.pb.h"`.
- The first build compiles protobuf and protoc from source, which takes a few extra minutes.

Visual Studio solution:

- No hand-written vcxproj for protobuf. It is built and installed once, and the installed output is linked.
- `tools/build_protobuf.ps1`: builds the submodule with CMake (VS 2022 generator, v143, x64, `/MD`) for Debug and Release and
  installs into `third_party/_install/protobuf/<Configuration>/` (`bin/protoc.exe`, `lib/libprotobuf(d).lib`, `include/`).
  The folder is gitignored. Run once on a new PC and again when the protobuf version changes.
- `strandwire.props`: adds the install folder's include/lib paths and the generated-code folder (`build\vs\gen`)
  as external includes (`ExternalIncludePath`, warnings off). Links `libprotobufd.lib` or `libprotobuf.lib` per configuration.
- `ProtoLib/ProtoLib.vcxproj`: holds the `.proto` files as `CustomBuild` items. Generates code into `build\vs\gen\` with the
  installed `protoc.exe` and compiles the `.pb.cc`. If the install folder is missing it fails with a message to run the script.
  Warnings are disabled for generated code (the one exception to "no compiler options in individual vcxproj files").
- `check_vs_sync`: for `ProtoLib` it compares the `CustomBuild` (`.proto`) list with CMake's `.proto` list instead of `ClCompile`.

Why this protobuf version and runtime: `docs/protocol.md` section 16.

## 5. Protocol and portability (superseded)

> Replaced on 2026-09-19 by LPN framing + protobuf. The reference is `docs/protocol.md`.
> `packet_header.h`, `packet_io.h` and `strutil.h` were deleted. Below is the record from the migration.

- The `#pragma pack(1)` structs and enums in `packet_header.h` are kept. The wire format does not change.
- `short` → `int16_t`, `memset` → value initialisation (`char name[MAX_NAME_LEN]{}`). Explicit `<cstdint>`, `<cstring>`.
- `MAX_PACKET_SIZE` in `packet_header.h`; `static_assert` that every struct is smaller.
  The receiver checks that the header `size` is between `sizeof(packet_header)` and `MAX_PACKET_SIZE`.
- `strcpy_s`, `sprintf_s`, `printf_s`, `memcpy_s` removed. Fixed-array copies go through one helper,
  `copy_to(char (&dst)[N], std::string_view)` in `strutil.h` (always null-terminated, truncates).
- Output via `std::cout`/`std::cerr`. `std::format` is not used (unsupported before GCC 13).
- Direct includes of `WinSock2.h`, `WS2tcpip.h`, `tchar.h`, `process.h`, `SDKDDKVer.h` removed.
- Platform branches only in CMake link settings.

## 6. Server architecture

The thread and ownership models still hold. The dispatcher (6.4) and packet details changed with the protocol.

### 6.1 Thread model

- One `io_context`. Worker count from `--threads N`, default `std::thread::hardware_concurrency()`, at least 1.
- `start()` launches N workers, `wait()` joins them. The main thread does not take part in `io_context::run()`
  (so tests can run the server in the background).
- The single acceptor coroutine runs on the `io_context` executor without a strand.
- One `asio::strand<asio::io_context::executor_type>` per session. The socket uses that strand as its executor.
  Every session coroutine is launched with `co_spawn(strand, ...)`.
- Result: session members are touched only on the strand. No locks.
- Other threads (e.g. broadcast) reach a session through `asio::dispatch(session->strand(), ...)`.
- `session_manager`: `std::unordered_map<session_id, shared_ptr<session>>` under a `std::mutex`.
  No session method is called while holding the lock: take a snapshot, release, then call.

### 6.2 Memory and ownership

- Sessions are `std::make_shared<session>`. Two strong references: the `session_manager` map and the `self` of a running coroutine.
- On disconnect the map entry is removed. When the coroutines finish, `self` is released and the object dies. No `ObjectPool`.
- Outgoing packets are `std::shared_ptr<const std::vector<char>>`. A broadcast shares one buffer with N sessions, so it is copied once.
- The per-session send queue is a `std::deque` of that `shared_ptr`, touched only on the strand.
- Receive buffer (at migration time): one `std::array<char, MAX_PACKET_SIZE>` per session. Read exactly 4 header bytes,
  validate, then read exactly `size - 4` body bytes. The ring buffer (`stream.hpp`) was deleted.
- `session_id`: a server-wide `std::atomic<uint32_t>`. `player_id`: a separate atomic issued at login, starting at 1.
  Using the map size as an id was dropped. `INVALID_ID = -1` stays.

### 6.3 Session lifecycle

1. accept → `make_shared<session>(std::move(socket), manager)` → `manager.add()` →
   `co_spawn(strand, session->run(), detached)`.
2. `run()` is the receive loop. It reads one packet with `packet_io::read_packet` and hands it to `dispatcher::dispatch`.
   Errors and exceptions converge on `close("reason")`.
3. `send(buf)` is called only on the strand. It enqueues and, if no send coroutine is running, `co_spawn(strand, write_loop())`.
   `write_loop` repeats `async_write` until the queue is empty, then clears the flag.
4. `close(reason)` is idempotent: socket `shutdown` + `close`, `manager.remove(id)`, clear the queue, one log line.
   Further calls do nothing.
5. Send queue limit `MAX_SEND_QUEUE = 256`. Exceeding it means a slow consumer: `close`.

`write_loop` was fixed on 2026-09-19: the buffer is popped before awaiting (send-queue rule in `docs/architecture.md`).

### 6.4 Dispatcher (superseded)

Design at migration time. Today the msgid dispatcher in `common/lpn/dispatcher.h` and `lobby_service` replace it.

- `std::array<handler_fn, PACKET_TYPE::MAX>`. `handler_fn` is `void(*)(session&, std::span<const char> packet)`.
- Filled in `dispatcher::register_all()` instead of static-initialisation macros. Called once explicitly at server start.
  Removes static initialisation order problems.
- Unregistered or out-of-range types call `close("unknown packet")`, never a null pointer.
- Handlers run on the session strand. The mutex is taken only to touch `session_manager`.
- `CS_LOGIN`: issue an id with `manager.next_player_id()`, store the name, reply `SC_LOGIN`.
- `CS_CHAT`: before login `close("chat before login")`. Otherwise build `chat_res` and `manager.broadcast(buf)`.
- `broadcast(buf)`: lock → `shared_ptr` snapshot → unlock → per session
  `asio::dispatch(s->strand(), [s, buf]{ s->send(buf); })`.

### 6.5 Server start and stop

- `server`: `server(port, threads)` (now `server_options`), `start()`, `stop()`, `wait()`,
  `port()` (the real port when started on port 0), `session_count()`. Used by the tests.
- `asio::signal_set` receives SIGINT/SIGTERM and calls `stop()`. Windows Ctrl+C takes the same path.
- `stop()`: close the acceptor + `manager.close_all()`. When no work remains the workers finish and `wait()` joins.
  At migration time `stop()` also called `io_context::stop()` and joined. Today it is non-blocking.

## 7. Error handling and logging

- I/O uses `asio::use_awaitable`, so errors throw. A `try/catch` at the top of each coroutine converges on `close(e.what())`.
- `operation_aborted` is a normal shutdown and is not logged.
- Protocol violations (header size out of range, unknown type, chat before login) `close` immediately.
- Logging: at migration time one `std::cerr` line, no library (`[session 12] closed: eof`).
  Replaced by `LogLib` (spdlog wrapper) on 2026-09-19 (A.11).

## 8. Clients

- Both use the `read_packet`/`write_packet` coroutines of `common/packet_io.h`. Their own header-assembly code was removed.
- `NetworkClient`: `io_context` on a background `std::thread`, main thread on stdin. Input is handed over with `asio::post(io, ...)`.
  The old code wrote to the socket from another thread, a data race.
  `quit` posts a close and joins. Usage `NetworkClient <ip> <port> <name>`.
- `DummyClient`: N sessions on a single-threaded `io_context`. Two coroutines per session (receive loop, chat every second on a `steady_timer`).
  Options `--ip --port --session --duration <sec>`. `duration` 0 runs forever; otherwise exit code 0 after that time.

## 9. Tests (at migration time)

- Two `assert`-based executables registered with CTest, no framework.
- `framing_test` (later `wire_test`): packet struct sizes match expectations (including `static_assert`),
  `write_packet` → `read_packet` round trip equals the original.
- `smoke_test`: start `server(0, 2)` and read the real port. Steps:
  1. Two clients log in → different `player_id`s.
  2. A chats → both receive `SC_CHAT` with A's name.
  3. A disconnects → `session_count() == 1`.
  4. Chat before login → forced close.
  5. `stop()`.
- Hang protection: client `io_context::run_for(15s)`, session-count polling timeout (5s).

Current test contents: the `wire_test` and `smoke_test` sections of `docs/architecture.md`.

## 10. Documentation update

After the implementation, rewrite the build/run/architecture/notes sections of `CLAUDE.md` and `README.md` for the new
structure. Keep the origin and project-goal sections.

## Appendix A. Alternatives considered and not chosen

The options not taken for each decision-log item. Revisit here if the premises change.

### A.1 Network library (instead of asio)

| Alternative | Pros | Why not |
|---|---|---|
| Hand-written IOCP / epoll / kqueue | Maximum control and performance. What commercial MMO servers do | Three OSes' worth of I/O layer, timers, cancellation and thread model to write |
| libuv (+ uvw) | Three-OS support proven by Node.js | C API, callback-oriented. Coroutine integration is weaker than asio's |
| libevent / libev | Old and stable | Windows support is select-based. Rarely chosen for new projects |
| POCO Net | Includes JSON, DB, logging | Heavy. Thread pool + reactor does not fit coroutines |
| ENet | Reliable UDP. Good for real-time movement | Changes the whole protocol. Overkill for a chat stream |
| Valve GameNetworkingSockets | Modern ENet with encryption and NAT traversal | Big dependency. The library dictates the thread model |
| gRPC / ZeroMQ / nng | Messaging patterns and serialisation | Does not fit a project whose point is protocol practice |
| Boost.Asio | Same code as standalone | Depends on all of Boost. Revisit if Boost.Beast becomes necessary |

### A.2 Async code style (instead of C++20 coroutines)

| Alternative | Pros | Why not |
|---|---|---|
| Callbacks + lambdas (C++17) | Compiles everywhere. The old client style | receive→process→send splits into many handlers. Repeated `self` captures |
| Stackful coroutines (`asio::spawn` + Boost.Context) | Sequential code before C++20 | Separate stack allocation, Boost.Context dependency. Does not fit standalone |
| future/promise (`use_future`) | Familiar API | Blocking waits do not fit an event loop. Occupies threads |
| sender/receiver (`std::execution`, C++26) | The standard's future direction | asio integration is experimental. Early compiler support. An option in 2-3 years |

### A.3 Thread model (instead of approach A)

| Alternative | Pros | Why not |
|---|---|---|
| B. Actor style (manager also on a strand) | No mutex | One login hops session strand → manager strand → session strand. More indirection |
| C. One io_context per thread (sharding) | No strands. Each thread owns its sessions | Broadcast crosses threads and ends up as B's post structure. Acceptor distribution logic needed |
| D. Single thread | No locks at all. The old server's model | One core only. Same as A with `--threads 1`, so no separate implementation |
| E. I/O threads + logic thread | The textbook layout for complex game servers | For a chat server it only adds a queue. Revisit when logic grows |

### A.4 Build system (instead of CMake)

| Alternative | Pros | Why not |
|---|---|---|
| Separate VS solution + Xcode + Makefile | No extra tool | Three copies to keep in sync by hand |
| Meson | Clean syntax, fast | Python dependency. Weaker VS integration than CMake |
| Bazel | Strong for large monorepos | Setup cost too high at this size |
| xmake / premake | Lightweight, Lua-based | Small ecosystem and IDE integration |

### A.5 Obtaining asio (instead of a git submodule)

| Alternative | Pros | Why not |
|---|---|---|
| CMake FetchContent | Auto-clones at configure time, no submodule | No offline builds. Downloads on every fresh configure |
| vcpkg (manifest mode) | Easy to add dependencies such as `openssl` | Requires vcpkg install and toolchain setup first. Revisit past three dependencies |
| Conan | Similar to vcpkg | Needs Python. vcpkg is more common in the local game industry |
| System packages (apt/brew) | Simple install | Not on Windows. Versions differ per distribution |
| Vendored headers | Simplest | Update history is not in git |

### A.6 VS solution handling

Decision: a hand-maintained .sln alongside CMake (section 4.1).

| Alternative | Pros | Why not |
|---|---|---|
| CMake only (first decision) | One build definition | Needs "open folder" or generated vcxproj in VS. Revised because the user prefers a hand-maintained solution |
| .sln only on Windows, CMake only for mac/Linux | One tool on Windows | A broken CMake definition goes unnoticed on Windows |
| Generate with CMake and commit | Opens directly in VS | Generated output: noisy diffs and absolute paths |

### A.7 Memory and ownership (instead of shared_ptr)

| Alternative | Pros | Why not |
|---|---|---|
| Keep ObjectPool | Saves allocations. The old way | Coroutines hold `self`, so reference counting is needed anyway. Can be added later via `std::allocate_shared` + a custom allocator |
| Intrusive reference count | Removes shared_ptr's control-block allocation. EasyGameServer's `RefCountable` | Consider after measuring |
| Session id + generation table | Eliminates dangling pointers | Coroutines must re-look-up after every await |
| unique_ptr + manual lifetime | Minimal overhead | Does not fit coroutines |

### A.8 Framing and receive buffer (instead of a fixed array + exact-size reads)

| Alternative | Pros | Why not |
|---|---|---|
| Keep the ring buffer (`stream.hpp`) | Parses several packets per `async_read_some`, fewer syscalls | Needs partial-packet handling. Return to it if two reads per packet proves to be a bottleneck |
| `dynamic_buffer` + `async_read_until` | Convenient for text protocols | Does not fit length-prefixed binary |
| Serialisation library (protobuf, flatbuffers, msgpack) | Schema evolution, cross-language | Deferred to the Godot client at the time. protobuf adopted on 2026-09-19 |

### A.9 Error handling (instead of exceptions + top-level catch)

| Alternative | Pros | Why not |
|---|---|---|
| `asio::as_tuple(use_awaitable)` | Explicit, no exceptions. Every await returns `(error_code, result)` | `if (ec)` on every line. Cheap to switch later since it is just the default completion token |
| `asio::redirect_error` | error_code for selected awaits only | Mixing styles breaks consistency |

### A.10 Tests (instead of assert executables)

| Alternative | Pros | Why not |
|---|---|---|
| Catch2 / doctest | Single header, good output | Consider doctest when tests grow |
| GoogleTest | Industry standard | Another submodule, slower builds |

### A.11 Other

| Item | Current decision | Alternatives and when to switch |
|---|---|---|
| Logging | `LogLib` wrapping spdlog 1.17.0 (revised 2026-09-19). Arguments are streamed, no format string. File log optional, off by default | Started as one `std::cerr` line. Replaced when output from several worker threads actually interleaved mid-line |
| ID allocation | Atomic counter at login | DB sequence or UUID once persistent accounts exist |
| Configuration | Command-line options | JSON/TOML config file once there are more than five options |
| Dispatcher | At migration: function-pointer array + `register_all()`. Since 2026-09-19: msgid → handler map, registered with one function pointer | Alternatives at the time: `unordered_map<type, std::function>`, template auto-registration, `switch`. Auto-registration once packets number in the dozens |
