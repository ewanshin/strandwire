# Architecture

English | [한국어](architecture.ko.md)

What the code is and how it runs: the folders and build targets, each project, the wire protocol summary,
the thread model, the session and server lifecycles, and what the tests check. The protocol itself is
specified in `docs/protocol.md`; how to build and run is in `docs/build.md`.

## Current state

`v0.3.0`, 2026-09-22.

- `v0.2.0` (2026-09-19): asio + CMake migration, LPN framing + protobuf protocol.
- `v0.3.0` (2026-09-22): the duplicate length field was removed and `type_` was flattened to one operation per value (wire change).
- Build, tests, execution and a Windows-client cross-connection were verified on all three OSes
  (after the wire change: Windows and Linux 2026-09-20, macOS 2026-09-22).
  - Windows: MSVC. Both CMake and the VS solution.
  - Linux: WSL2 Ubuntu 22.04, g++ 11.4, CMake 3.22.
  - macOS: 27.0 arm64, Apple clang 21, CMake 3.31.
- Planned clients: pure C++ (present), Godot, maybe Unity. No Godot/cocos2d-x client yet.

## Overview

A TCP chat server and two clients on standalone asio 1.38.2 + C++20 coroutines.
The protocol is LPN framing with a protobuf body (`docs/protocol.md`).
Log strings inside the sources are English.

There are 9 projects (build targets). Other folders and files:

- `common/lpn/` - the wire protocol implementation. Header-only, so no project of its own.
  CMake: INTERFACE target `netsys_common`. VS: the `common\lpn` filter of `ServerLib`.
- The other headers in `common/`:
  - `server_define.h` - constants unrelated to the wire (`LISTEN_PORT`, `INVALID_ID`).
  - `utf8.h` - strict UTF-8 validation.
  - `error_text.h` - locale-independent English descriptions of `std::error_code`.
  - `console.h` - console UTF-8 I/O helpers and the ESC key watcher. Included only from `main.cpp` files.
- `strandwire.sln`, `strandwire.props`, `strandwire.config.props` - the hand-maintained VS solution and its
  shared property sheets.
- `tools/build_protobuf.ps1` - installs protobuf for the VS solution into `third_party/_install/`.
- `third_party/` - three git submodules.
  - `asio` - headers in `third_party/asio/include/`.
  - `protobuf` - tag `v3.21.12`.
  - `spdlog` - tag `v1.17.0`, used header-only.
- `docs/protocol.md` - **the reference for the protocol.** Byte layout, constants, messages and msgids, error codes,
  flow, byte examples, a checklist for clients in other languages (sections 1-12), design decisions and the change
  guide (sections 13-16). `test_documented_examples` in `wire_test` checks the examples against the implementation.
  When the protocol changes, update this document and that test together.
- `docs/specs/2026-09-17-asio-cmake-migration-design.md` - asio/CMake migration decisions, protobuf build integration
  (section 4.2), alternatives considered (appendix A). Its section 5 (the old struct protocol) is superseded by `docs/protocol.md`.

## Projects

One folder = one CMake target = one vcxproj, all with the same name. The exception is `tests/` (three executables).

| Project | Kind | Folder | Depends on | Summary |
|---|---|---|---|---|
| `LogLib` | static lib | `LogLib/` | spdlog | Wrapper around spdlog's asynchronous logger |
| `ProtoLib` | static lib | `ProtoLib/` | protobuf | `.proto` files and generated code |
| `ServerLib` | static lib | `ServerLib/` | common, ProtoLib, LogLib | All server logic |
| `NetworkServer` | executable | `NetworkServer/` | ServerLib | Thin server executable |
| `NetworkClient` | executable | `NetworkClient/` | common, ProtoLib | Interactive console chat client |
| `DummyClient` | executable | `DummyClient/` | common, ProtoLib | Load-test client |
| `wire_test` | test | `tests/` | common, ProtoLib | Byte-level wire format test |
| `smoke_test` | test | `tests/` | ServerLib | In-process end-to-end server test |
| `app_test` | test | `tests/` | ServerLib | Config loading and start-up phases |

### LogLib

`nslog::logger`, a wrapper around spdlog's asynchronous logger.

- Pattern `[date time.ms][module][level][thread]message`, levels trace..fatal.
- Arguments are streamed, no format string: `server_log.info("[session ", id, "] closed: ", why)`.
- The file log is **optional and off by default.** No file when `configuration::folder_name` is empty.
  `--log-dir <folder>` on a client, `log_config.dir` in the server's environment file, turns on a daily file.
- Only `logger.cpp` includes spdlog. Projects that log pay neither spdlog's include path nor its compile time.
- Uses its own thread pool instead of the global registry.
- On Windows it writes to the console with `WriteConsoleW`, so UTF-8 names display regardless of the code page.
  The option macros are at the top of `logger.cpp`.

Access goes through one singleton macro per module:
`server_log` in `ServerLib/server_log.h`, `client_log` in each client folder's `client_log.h`.

**Lifetime rules:**

- `main()` calls `start({level, module name, folder})` and calls `stop()` before it returns.
  During static destruction spdlog's internal statics may already be gone.
- Logging before `start()` or after `stop()` is silently dropped.
- Call `start`/`stop` only before worker threads start and after they are joined.

### ProtoLib

Static library of the `.proto` files (`chat.proto`, `error_code.proto`, `lobby_config.proto`) and the
protoc-generated code.

- Generated code is not committed. CMake puts it in `<build>/ProtoLib/gen`, VS in `build\vs\gen`.
- Consumers write `#include "chat.pb.h"`.
- Package and message names determine the msgid. **Renaming changes the wire contract.**
- `.proto` conventions (`docs/protocol.md` section 8):
  - proto2, lower snake_case, suffixes `_req`/`_res`/`_noti`.
  - The first field of a response is `required int32 error_code_ = 1`.
  - Error codes come in ranges of 100 per domain (1-99 system, 100- auth, 200- lobby ...).
- When adding a `.proto`, also update `NETSYS_PROTO_FILES` in `ProtoLib/CMakeLists.txt`,
  `CustomBuild`/`ClCompile` in `ProtoLib.vcxproj`, and `.filters`.

### ServerLib

All server logic. Server features are almost always changed here. Details in "Architecture".

- `server_app` - the process as phases: config -> logger -> connections -> assets -> listen -> serve; reverse teardown.
- `server_config` - configuration: identity from the command line, environment from the JSON file, validation,
  `describe()`.
- `server` - acceptor, worker threads, start/stop, `server_options`.
- `session` - one connection: strand, receive/send coroutines, send queue, tunnel table, timeout watchdog.
- `session_manager` - session registry, per-tunnel broadcast, id allocation.
- `lobby_service` - message handlers of the LOBBY tunnel.

It is a library so that `smoke_test` can run the server in the same process. Consumers: `NetworkServer` and `smoke_test`.

### NetworkServer

The server executable. Only `main.cpp`, which wires the console (UTF-8 output, ESC watcher) to `server_app`.
Everything between process start and "server ready" is in `ServerLib/server_app.h`.

**Start-up phases**, in order, grouped into the lifecycle methods of `server_app`. The first failure stops
the sequence and tears down only the phases that came up, in reverse. Nothing is accepted before every phase is up.

| Method | # | Phase | What | Failure |
|---|---|---|---|---|
| `pre_init_instance` | 1 | config | Take the config `main()` loaded with `load_config` (identity from the command line, environment from the `--config` file, validated) and print it | returns false |
| | 2 | logger | `server_log.start()` with the loaded log config. From here on every phase can log | Message on stderr, returns false |
| `init_instance` | 3 | connections | `server_component`s added with `add_connection()` (DB, cache, discovery). Empty today | Components that came up are torn down |
| | 4 | assets | `server_component`s added with `add_asset()` (data loaded before serving). Empty today | Same |
| | 5 | listen | Construct `server`, register handlers, bind/listen. The OS completes handshakes from here; nothing is accepted or read | Fatal log, returns false |
| `start` | 6 | serve | (Later: connect to other servers.) Accept loop and worker threads; packets are handled from here | Fatal log, returns false |

`stop()` requests a stop, `wait()` blocks until the server stopped, and `exit_instance()` runs the same list
backwards: serve -> listen -> assets -> connections -> logger. `server_app::trace()` records `up:`/`fail:`/`down:`
entries for tests. `listening()` is true from phase 5, `serving()` from phase 6.

The rule behind the split: **until `start()` a client can connect, but no packet is handled.** The bytes it
sends wait in the kernel and are read once the serve phase is up (`test_listen_before_start` in `smoke_test`).

**Everything from the logger's start to `server ready` is in the log.** Each phase logs `phase 'X' up (N ms)` and
each lifecycle method logs `<method> success` when it returns true (`start success: server ready` last);
a failure logs the phase's own reason, then one fatal line `<method> failed at phase 'X' after N ms: start-up
aborted, tearing down`, before the teardown takes the logger down. `server stopped` is logged when the serve
phase goes down, so it never appears for a start-up that failed.

`main()` loads the config itself (`load_config`, a `config_error` prints the message and usage on
stderr), then calls `pre_init_instance`, `init_instance` and `start` as three separate statements. Each failure
prints one line to stderr and exits with its own code, so a script or a service manager can tell them apart:

| Exit code | Step | Meaning |
|---|---|---|
| 0 | - | Clean stop |
| 1 | `load_config`, `pre_init_instance` | Invalid config, or the logger did not start |
| 2 | `init_instance` | A connection, an asset or the listen socket did not come up |
| 3 | `start` | Could not start serving |

Codes are small positive numbers. An exit status is 8 bits on POSIX and an unsigned 32-bit value on Windows, so
a negative code shows up as 255 or 4294967295.

**Console lines outside the logger's lifetime.** The logger exists only between phase 2's up and down. The lines
before it (config, phase 'config' up) and after it (phases 'logger' and 'config' down, `exited with code 0`) go
to stdout, errors to stderr, each prefixed `[NetworkServer]`. `note()`/`note_error()` in `server_app.cpp` pick the
logger when `server_log.running()` and the console otherwise. This is the one allowed console output in
`ServerLib`.

**Configuration** (`ServerLib/server_config.h`). The command line says who this instance is and where its
environment file is. The file holds everything else. The two never overlap, so there is no precedence.

Command line (`parse_command_line`):

| Option | Default | Valid |
|---|---|---|
| `--ip <address>` | `0.0.0.0` (every interface) | IPv4 or IPv6 address to listen on |
| `--sid d.i.t.id` | `0.0.11.1` | `domain.idc.type.id` |
| `--config <file>` | none (all defaults) | JSON parsed with protobuf `JsonStringToMessage`; unknown keys are errors |

Environment file (schema `ProtoLib/lobby_config.proto`, package `config`). The root message `lobby_config` has
one section per concern, so the JSON reads `{ "log_config": {...}, "listen_config": {...} }`:

| Key | Default | Valid |
|---|---|---|
| `listen_config.port` | 10000 | 0..65535 (0 = ephemeral, tests) |
| `listen_config.threads` | number of CPU cores | 1..1024 |
| `listen_config.session_timeout_ms` | 5000 | >= 1 |
| `log_config.level` | info | trace..fatal, off |
| `log_config.dir` | none (file log off) | folder |
| `log_config.console` | true | `false` turns the console sink off (tests, services) |

- An absent option or key keeps the default (proto2 `has_*`).
- Every invalid value is reported at once, from both halves.
- **One schema file per server: `<server>_config.proto`.** This server is LOBBY, hence `lobby_config.proto`.
  Sections every server shares (`log_config`, `listen_config`) move to a `base_config.proto` (name not final)
  that the server files `import` when the second server appears. Server-only sections are added to the root
  message as they come; today LOBBY has none.
- `NetworkServer/lobby_config.example.json` is the template; `NetworkServer/lobby_config.json` is gitignored
  for local copies.
- **There are three ways to stop cleanly and all of them converge on `server::stop()`.**
  - ESC on the console. `console::key_watcher`. Active only when stdin is an interactive console.
  - Ctrl+C, SIGTERM. Received by the library's `signal_set`.

### NetworkClient

The console chat client for humans. Used to check server features by hand.
Usage: `NetworkClient <host> <port> <name>`.

- On connect it opens the LOBBY tunnel by anycast, receives the server's sid, and logs in automatically.
- One input line = one chat message. `quit` exits.
- Sends a NOOP heartbeat every 3 seconds.
- The `io_context` runs on a background thread; the main thread only reads stdin. Input is handed to the io
  thread with `asio::post`, keeping socket access single-threaded.
- Input before the login response prints "not logged in yet" and is dropped.

### DummyClient

The load-test client. The automated tests do not cover concurrency or broadcast load; after changing the
server's threading or send path, verify with this.

- N sessions on a single-threaded `io_context`.
- Per session: open the tunnel → log in as `UserName<n>` → random 0-1 s delay → chat every second. Heartbeats too.
- Options: `--ip --port --session --duration`.
- `--duration` (seconds) 0 runs forever. Otherwise it prints statistics (`sessions`, `logged_in`, `chats_received`)
  after that time and exits.
- Exit code 0 when every session logged in. Usable as a pass/fail check in scripts.

### wire_test

Unit test that runs without a server. **Its expected byte strings are the wire contract.**
Run it first after touching `common/lpn/`. Change the expected values only when the wire is changed on purpose.
No framework, just a `CHECK` macro (prints the location and exits with code 1 on failure).

Checks:

- Big-endian helpers.
- sid assembly/decomposition/text/parsing.
- Three published FNV-1a 32-bit test vectors.
- Full byte strings of three golden packets: NOOP heartbeat, LOBBY anycast CONNECT, DATA carrying `chat.login_req`.
- The 10 byte examples of `docs/protocol.md` section 11 and the 5 msgid values of section 8 (`test_documented_examples`).
- Decode rejection: short tunnel packet, tunnel id ≥ 32, shorter than the header.
- `read_frame` rejecting an oversized frame.
- Loopback round trip.
- The five dispatcher results and the duplicate-registration exception.
- `utf8::is_valid`: UTF-8 Korean and 4-byte characters pass; CP949 bytes, truncated sequences, overlong forms and surrogates fail.

### smoke_test

End-to-end test that starts a real server and connects over real TCP sockets. Run it first after changing server logic.

- Links `ServerLib` and starts the server in the same process on port 0 (ephemeral).
- Runs the scenario coroutines on a separate `io_context` on the main thread.
- The only direct question to the server is `session_count()`. Everything else goes through the socket.
- `main()` starts `server_log` console-only, so a failed run shows what the server saw.
- Set it as the start-up project in VS and press F5 to debug the server code in one process.

`test_main_flow`:

- DATA on a closed tunnel → FAILED.
- CONNECT to an unserved tunnel (REGION) → FAILED.
- LOBBY anycast CONNECT → accepted with sid `0.0.11.1`.
- Chat before login → `chat_res{NOT_LOGGED_IN}`.
- Empty name → `INVALID_NAME`.
- Two clients log in with different ids.
- Duplicate login → `ALREADY_LOGGED_IN`.
- Chat → the sender gets `chat_res` then `chat_noti`, the other client gets `chat_noti`.
- UTF-8 Korean chat is broadcast unchanged.
- CP949 bytes are rejected with `INVALID_TEXT` and not broadcast.
  The one `invalid UTF-8` line protobuf prints here is expected.
- NOOP/PING heartbeat replies.
- Unknown msgid and unknown packet type (`type_` 0x7F) keep the connection up.
- DATA after DISCONNECT → FAILED.
- Malformed frame (a CONNECT too short to hold the sid) → connection closed.
- Closing the socket → session count drops.

`test_close_during_write`: regression test. Provokes the race between the server closing a session and a
reply still in flight, 100 times. See the send-queue rule in "Session lifecycle".

`test_timeout`: a server with a 300 ms timeout. The connection survives while heartbeats flow and is dropped when they stop.

`test_listen_before_start`: after `init_instance()` only, a client connects and sends a heartbeat; 300 ms later the
session count is still 0. After `start()` the queued connection is accepted and the heartbeat answered.

### app_test

Tests for `server_config` and `server_app`. No sockets except an ephemeral listen; the logger runs with level off.

- Defaults; identity from the command line and environment from the file; unknown keys (including `sid` in the
  file), bad values, missing values, unknown options (including `--port`) and broken JSON rejected with the value
  named; all problems from both halves reported together.
- Full lifecycle trace: `up:` config, logger, connection, connections, asset, assets, listen, serve, then `down:`
  in reverse. `listening()` without `serving()` between `init_instance` and `start`.
- A failing connection or asset: later phases never run, the components that came up are shut down in reverse,
  the app is not listening. A logger that cannot start (log folder under a regular file) fails `pre_init_instance`.
  A config loaded from a command line reaches the app unchanged.

Hang protection:

- Session-count checks poll every 10 ms for at most 5 s.
- Client event loops are bounded with `run_for`.
- CTest timeout 60 s.

Not covered: concurrency load (→ `DummyClient`), the send-queue-over-256 path.

Note: session-count polling sleeps the thread inside a coroutine, which stalls the client event loop meanwhile.
Cases that must keep receiving while waiting use the timer-based `sleep_for` coroutine.

## Origin

- A reduced rewrite of zeliard/EasyGameServer (MIT, 2013). The notice is at the end of `LICENSE`.
- 2020, first version: WinSock2 overlapped I/O + APC server.
- 2026-09-17: complete rewrite on asio + CMake.
- 2026-09-19: the struct-memcpy protocol replaced by LPN framing + protobuf.
- 2026-09-20: duplicate length field removed, `type_` flattened to one operation per value.
- DummyClient's behaviour (chat every second after login, `UserName<n>` names) comes from EasyGameServer.

## Design

### Wire protocol (`common/lpn/`)

The protocol is specified once, in `docs/protocol.md`: packet layout (section 3), the `type_` and heartbeat
constants (4), tunnel packet behaviour (5), the server sid (6), msgid (7), the LOBBY messages with their
fields, server handling and msgid values (8), error codes (9), the flow (10) and byte examples (11). This
section only states what the server code relies on.

- Every multi-byte integer is **big-endian** and assembled byte by byte. No struct memcpy.
- A packet is `[frame_len][type_][param_]` followed, for tunnel packets (`type_` 1-5), by `server_sid` and,
  for DATA, by `msgid` and the protobuf body. A heartbeat (`type_` 6) is 6 bytes.
  In code: `lpn::is_tunnel_packet(type)` / `frame::is_tunnel()`.
- A tunnel id ≥ 32 makes the decoder throw `protocol_error`, because the session indexes its tunnel table with it.
- **Tunnel id (`param_` of a tunnel packet, 0-31) = server type**: 2 GWS, 4 AUTH, 9 QUEST, 10 ITEM, 11 LOBBY,
  13 REGION, 14 AI. This server serves LOBBY only.
- **`server_sid`**: 4 × 16 bits, text form `domain.idc.type.id`. `type` equals the tunnel id. `id` 0 is anycast.
  Server default `0.0.11.1`.
- **msgid**: `fnv1a32` of the full name including the package, e.g. `"chat.login_req"`. Stock protoc; the name
  reported by the runtime is hashed at registration time, so there is no hand-maintained id table and a renamed
  message shows up immediately as "unknown msgid".
- **Flow**: TCP connect → CONNECT (LOBBY, anycast `0.0.11.0`) → server's CONNECT (`0.0.11.1`) →
  `login_req`/`login_res` → `chat_req` → `chat_res` + `chat_noti`. The client sends NOOPREQ every 3 seconds.
  The server drops a connection after 5 seconds without any packet.

**All strings are UTF-8.** That is the rule for protobuf `string` fields.

- Under proto2, protobuf only logs an error and lets bad strings through, so the server validates with
  `utf8::is_valid` and rejects.
- The Windows console delivers input in the active code page (CP949 on Korean systems). `NetworkClient`
  converts with these helpers:
  - `console::read_line` - `ReadConsoleW` → UTF-8 on a console, pass-through on a pipe.
  - `console::arg_to_utf8` - command-line arguments.
  - `console::init_utf8_output` - switches the console output code page to UTF-8 and restores it at exit.
- Switching the console **input** code page to UTF-8 is not used: older conhost delivers Korean as NUL.
- A new executable that handles human input or names uses the same helpers.

Wire constants live only in `common/lpn/wire.h` and the byte layout only in `common/lpn/frame.h`. How to change
the wire or add a message is in "Change procedures" below.

### Files in `common/lpn/`

| File | Responsibility |
|---|---|
| `wire.h` | Every wire constant, heartbeat period and timeout, `MAX_FRAME_SIZE` (1 MiB), big-endian `put_*`/`get_*` |
| `sid.h` | `lpn::sid` assembly/decomposition/text/parsing, anycast check |
| `frame.h` | `lpn::frame`, `make_tunnel`, `make_heartbeat`, `encode`, `decode_body`. Malformed frames throw `protocol_error` |
| `msgid.h` | `fnv1a32`, `msgid_of<M>()`, `encode_message` (msgid + body) |
| `frame_io.h` | asio coroutines `read_frame` (size check before allocation), `async_write_frame`, synchronous `write_frame`, `shared_buffer` |
| `dispatcher.h` | `message_dispatcher<Ctx>`. Registering one function pointer deduces the message type and msgid from its signature. Results: `ok`/`too_short`/`unknown_msgid`/`parse_error`/`not_initialized` |

Implementation rules independent of the wire are in `docs/protocol.md` section 14: enforcing the maximum size,
logging unknown msgids, answering DATA on a closed tunnel with FAILED, and so on.

### Server thread model

- One `io_context` run by `threads` workers (environment file).
- One strand per session; the socket uses that strand as its executor. Session members are touched only on the strand.
  There are no locks inside a session.
- `session_manager` is one map under a mutex. No session method is called while holding the lock.
- Broadcast: take a snapshot, then `asio::dispatch(strand, ...)` per session. Only sessions with that tunnel open receive it.
  One encoded packet (`shared_buffer`) is shared by every recipient.
- Anything that touches a session from another thread must dispatch the same way.

### Session lifecycle (`ServerLib/session.cpp`)

1. accept → `make_shared<session>` → `manager.add` → `start()`.
2. `start()` launches the `run()` (receive loop) and `watchdog()` (timeout) coroutines on the strand.
3. `run()` repeats `read_frame` → `on_frame`. I/O errors and `protocol_error` converge on `close(reason)`.

A session owns a tunnel table (`tunnel id → bound sid`, size 32, 0 = closed). A separate gateway would own this table.

- CONNECT: accepted when the tunnel id and the requested sid's type equal the server sid's type, and the request is
  anycast or exactly the server sid. Recorded in the table and answered with CONNECT carrying the real sid. Otherwise FAILED.
- DATA: FAILED if the tunnel is closed. Otherwise handed to the LOBBY dispatcher.
  `unknown_msgid`/`not_initialized` are logged and ignored; `too_short`/`parse_error` close the connection.
- DISCONNECT: closes the tunnel and replies DISCONNECT only when the sid equals the table value.
- Heartbeat: NOOPREQ → NOOPRES, PINGREQ → PINGRES.
- Any received packet refreshes the last-receive time.

**Send-queue rules:**

- `send()` enqueues and starts the send coroutine if none is running. Over 256 queued buffers closes the connection.
- `write_loop` **pops the buffer before awaiting** so the coroutine owns it, because `close()` may clear the queue mid-write.
- Popping after the await touched an empty deque and crashed about one run in five.
  The regression test is `test_close_during_write` in `smoke_test`.
- Never assume a container's state across a co_await.

`close()` is idempotent: cancels the watchdog timer, closes the socket, clears the queue and the tunnel table,
removes the session from the map. The object is freed once the map and the running coroutines drop their references.

### Server start/stop (`ServerLib/server.cpp`)

- `server(server_options)`. Options: `ip`, `port`, `threads`, `sid`, `session_timeout`.
- The constructor registers the LOBBY handlers; a msgid collision throws.
- `init_instance()`: open/bind/listen. The port is held and the OS queues connections; nothing is accepted.
- `start()`: the signal handler, the accept coroutine and the worker threads, then returns immediately.
  Throws `std::logic_error` without `init_instance()`.
- `stop()`: posts "close the acceptor + close every session" to the io_context. Non-blocking.
- `wait()`: join.
- `exit_instance()`: closes the listen socket. After `wait()`, or when `start()` never ran.
- SIGINT/SIGTERM arrive through `signal_set` and call `stop()`.
- Port 0 picks an ephemeral port; read it with `port()` (for tests).
- One server plays both gateway and LOBBY backend. Splitting the gateway into its own process and designing the
  server-to-server segment come later (issue #13). A gateway per region is issue #19; the sid's `idc` part is kept for it.

### Clients

- Both run three coroutines on a single-threaded `io_context`: receive loop, heartbeat, and (DummyClient) a chat timer.
- Sending is the synchronous `write_frame`.
- Received DATA goes through the client's own `message_dispatcher` to member-function handlers.
- `NetworkClient` reads stdin on the main thread and hands lines to the io thread with `asio::post`.

## Tests

Four are registered with CTest.

- `wire_test`, `smoke_test`, `app_test` - executables; see "Projects".
- `check_vs_sync` - a CMake script (`tests/check_vs_sync.cmake`) that checks the vcxproj and CMake source lists
  match. Not visible in the VS solution.

```
ctest --preset windows-msvc                                   # all
ctest --preset windows-msvc -R smoke_test --output-on-failure  # one
.\build\vs\Debug\bin\smoke_test.exe                            # run the VS build directly
```

- Every test calls `test_support::init()` (`tests/test_support.h`) on the first line of `main()`.
  A new test executable must do the same.
  - Turns off output buffering.
  - On Windows, CRT assertions, `abort` and access violations go to stderr **without a dialog**.
- Investigate intermittent failures by running the executable dozens of times and counting failures
  (a PowerShell `for` loop collecting `$LASTEXITCODE`).

### Interactive paths

Not covered by the automated tests, because their stdin is not a console.

- Typing Korean into the Windows console (`console::read_line`): confirmed by the user (2026-09-19).
- ESC on the Windows server (`console::key_watcher`): confirmed by the user (2026-09-19).
  ESC → sessions closed → `server stopped` → exit code 0.
- The POSIX path of `common/console.h` (termios, poll-based ESC watcher) has only been compiled on Linux and macOS.
  It has not been exercised with real key presses; automated runs have a non-console stdin, so it stays inactive.

## Change procedures

**Adding a message.** The msgid is automatic.

1. Define it in `ProtoLib/chat.proto`.
2. Write `void on_xxx(session&, const chat::xxx&)` in `ServerLib/lobby_service.cpp` and add one `regist` line
   to `register_lobby_handlers`.
3. In the clients, `lobby_.regist(&client_session::on_xxx)` in the constructor.
4. Update `docs/protocol.md` section 8 (including the msgid value).

**Changing the wire** (`docs/protocol.md` section 15.3). Every client must change with it.

1. Edit `wire.h`/`frame.h`.
2. Confirm `wire_test` fails, then update its expected bytes.
3. Update the tables and section 11 examples of `docs/protocol.md`, and the wire protocol section of this
   document. `test_documented_examples` in `wire_test` checks the examples against the implementation.

**Changing the start-up phases.** Add one row to `PHASES[]` in `ServerLib/server_app.cpp`, the two member
functions, and one `phase_id` value at the same position; a `static_assert` keeps the table and the enum the same
size. Update the phase table in this document and the expected traces in `tests/app_test.cpp`.

Adding source files, `.proto` files and verifying a change: `docs/build.md`, "Change procedures".

