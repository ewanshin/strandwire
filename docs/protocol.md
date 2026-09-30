# Protocol (LPN)

The bytes exchanged between server and client over TCP, the design decisions behind them, and how to change them.

- Implementation: `common/lpn/`. Constants in `wire.h`, byte layout in `frame.h`.
- Example verification: `test_documented_examples` in `tests/wire_test.cpp`.
- Sections 1-12 describe the structure; sections 13-16 record decisions and the change guide.

## 1. Basic rules

| Item | Value |
|---|---|
| Transport | TCP. Default port 10000 |
| Byte order | **Big-endian** |
| Body | protobuf (proto2) |
| Strings | UTF-8. The server rejects anything else |
| Maximum size | `frame_len` ≤ 1 MiB (1,048,576). Larger closes the connection |
| Heartbeat | The client sends one every 3 seconds |
| Timeout | The server closes a connection after 5 seconds without any packet |

## 2. Layers

```
L1  transport frame  uint32 frame_len
L2  LPN header       uint8 type_ | uint8 param_
L2b server sid       uint64 server_sid          (tunnel packets only: type_ = 1..5)
L3  msgid            uint32                     (type_ = DATA with a body only)
L4  body             protobuf, no length prefix, up to the end of the frame
```

Receive procedure:

1. Read 4 bytes → `frame_len`. Below 2 or above 1 MiB: close.
2. Read `frame_len` bytes. The whole packet is `frame_len + 4` bytes.
3. Branch on `type_`. Unknown values are ignored.
4. For a tunnel packet: close if `param_` (tunnel id) is 32 or more, or if fewer than 8 sid bytes follow.

## 3. Packet layout

### 3.1 Tunnel packet (`type_` = 1..5)

| Offset | Size | Field | Type | Meaning |
|---|---|---|---|---|
| 0 | 4 | `frame_len` | `uint32` | Bytes that follow this field. Excludes itself |
| 4 | 1 | `type_` | `uint8` | Operation: 1 CONNECT, 2 DISCONNECT, 3 DATA, 4 FAILED, 5 SHIFT (section 4.1) |
| 5 | 1 | `param_` | `uint8` | Tunnel id, 0..31 (section 4.3) |
| 6 | 8 | `server_sid` | `uint64` | Server instance. Meaning in section 5 |
| 14 | 4 | `msgid` | `uint32` | Only when `type_` = DATA and there is a body |
| 18 | N | body | bytes | protobuf message |

- `payload` = everything from offset 14.
- `frame_len = 10 + payload`.
- Fixed overhead of DATA: 18 bytes.

### 3.2 Heartbeat (`type_` = 6, 6 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `frame_len` | 2 |
| 4 | 1 | `type_` | 6 |
| 5 | 1 | `param_` | Sub-command (section 4.2) |

## 4. Constants

### 4.1 Packet type (`type_`)

| Value | Name | Meaning | Direction | `param_` | sid |
|---|---|---|---|---|---|
| 0 | - | Never sent | - | - | - |
| 1 | CONNECT | Open a tunnel / accepted | client → server, server → client | tunnel id | yes |
| 2 | DISCONNECT | Close a tunnel | both | tunnel id | yes |
| 3 | DATA | Send a message over a tunnel | both | tunnel id | yes |
| 4 | FAILED | Request failed | server → client | tunnel id | yes |
| 5 | SHIFT | The tunnel moved to another server | server → client | tunnel id | yes |
| 6 | HEARTBEAT | Liveness check | both | sub-command | no |
| other | - | Logged and ignored. Connection stays up. Extension point for new types | - | - | no |

1..5 are called "tunnel packets". Their rules are in section 5.

### 4.2 Heartbeat sub-commands

| Value | Name | Direction | Reply |
|---|---|---|---|
| 0 | - | Never sent | - |
| 1 | PINGREQ | client → server | PINGRES |
| 2 | PINGRES | server → client | - |
| 3 | NOOPREQ | client → server | NOOPRES |
| 4 | NOOPRES | server → client | - |

- Clients use NOOPREQ. PING is kept for round-trip measurement.
- The server resets its timeout on any packet.

### 4.3 Tunnel id (server type)

One tunnel per id. At most 32 per connection (0..31). The tunnel id equals the `type` part of a sid. Values not listed are reserved.

| Value | Server type | This server |
|---|---|---|
| 2 | GWS (gateway) | - |
| 4 | AUTH | - |
| 9 | QUEST | - |
| 10 | ITEM | - |
| 11 | LOBBY | **served** |
| 13 | REGION | - |
| 14 | AI | - |

Examples: LOBBY CONNECT = `01 0B`. LOBBY DATA = `03 0B`. REGION FAILED = `04 0D`.

## 5. Tunnel packet behaviour

The server keeps a per-connection table `tunnel id → bound server_sid` (size 32, 0 = closed).

| `type_` | `server_sid` | payload | Server action |
|---|---|---|---|
| CONNECT (request) | Server to open. `id` = 0 means anycast | none | Accept with CONNECT, refuse with FAILED. An already open tunnel is simply rebound |
| CONNECT (accept) | The server actually bound | none | - |
| DATA | Bound server | `msgid` + protobuf | Closed tunnel: FAILED. Open tunnel: handle the message |
| DATA (length 0) | Bound server | none | Liveness refresh only |
| DISCONNECT | Server to close | none | Close and reply DISCONNECT if it equals the bound value. Otherwise ignore |
| SHIFT | The **new** server | 8 bytes: previous server sid | Sent when the server changes the serving instance. This server never sends it |
| FAILED | The target that failed | none | - |

CONNECT is accepted when both hold:

- tunnel id = `type` of the requested sid = `type` of the server sid.
- The requested sid is anycast or equals the server sid.

Other rules:

- A client that receives SHIFT only updates its table and uses the new sid in later DATA.
- If a DATA sid differs from the bound value, the server logs it and routes by the bound value. The tunnel table is the source of truth.

## 6. Server sid (64 bits)

| Bits | Field | Meaning |
|---|---|---|
| 63..48 | `domain` | Domain (service) |
| 47..32 | `idc` | Data centre |
| 31..16 | `type` | Server type. Same value as the tunnel id |
| 15..0 | `id` | Instance number. **0 = anycast** (any instance of that type) |

- `value = id | type << 16 | idc << 32 | domain << 48`.
- Text form: `domain.idc.type.id`.
- On the wire: 64-bit big-endian, `domain`'s high byte first.
- Server default `0.0.11.1` → `00 00 00 00 00 0B 00 01`. Changeable with `--sid`.
- LOBBY anycast `0.0.11.0` → `00 00 00 00 00 0B 00 00`.

## 7. msgid

`msgid = FNV-1a 32-bit(full message name including the package)`

```
hash = 2166136261
for each byte b of the name:
    hash = hash XOR b
    hash = (hash × 16777619) mod 2^32
```

- There is no id table. The name is the id. Renaming a message changes its msgid.
- Stock protoc. The name reported by the protobuf runtime (`GetTypeName()`) is hashed at registration time.
- A msgid only has to be unique within a tunnel. Dispatchers are independent per tunnel.
- Reference values for checking an implementation (published FNV-1a 32-bit test vectors): `""` = `0x811C9DC5`, `"a"` = `0xE40C292C`, `"foobar"` = `0xBF9CF968`.

Dispatch:

- Per tunnel, a table `msgid → (prototype message, handler)`.
- Registration takes one function pointer. Message type and msgid are deduced from its signature.
- Two handlers with the same msgid in one tunnel refuse server start-up.
- Receive: read `msgid`, clone the prototype with `New()`, parse the remaining bytes. Parse failure and `IsInitialized()` failure are told apart.

## 8. LOBBY tunnel messages

`ProtoLib/chat.proto`, package `chat`.

| Message | msgid (decimal) | msgid (hex) | Direction |
|---|---|---|---|
| `chat.login_req` | 2267621343 | 87 29 27 DF | client → server |
| `chat.login_res` | 2301176581 | 89 29 2B 05 | server → client |
| `chat.chat_req` | 1118036162 | 42 A3 E0 C2 | client → server |
| `chat.chat_res` | 1084480924 | 40 A3 DD 9C | server → client |
| `chat.chat_noti` | 2575960202 | 99 8A 08 8A | server → everyone |

Fields:

| Message | No. | Field | Type | Rule |
|---|---|---|---|---|
| `login_req` | 1 | `name` | `string` | required. 1-64 bytes, UTF-8 |
| `login_res` | 1 | `error_code_` | `int32` | required |
| | 2 | `player_id` | `int32` | optional. Only on success. Starts at 1 and increases |
| `chat_req` | 1 | `text` | `string` | required. UTF-8 |
| `chat_res` | 1 | `error_code_` | `int32` | required |
| `chat_noti` | 1 | `player_id` | `int32` | required. The sender |
| | 2 | `name` | `string` | required. Filled by the server; cannot be forged |
| | 3 | `text` | `string` | required |

Server handling:

| Request | Condition | Reply |
|---|---|---|
| `login_req` | Already logged in | `login_res{ALREADY_LOGGED_IN}` |
| | Name empty, over 64 bytes, or not UTF-8 | `login_res{INVALID_NAME}` |
| | Otherwise | `login_res{SUCCESS, player_id}` |
| `chat_req` | Before login | `chat_res{NOT_LOGGED_IN}` |
| | Not UTF-8 | `chat_res{INVALID_TEXT}`. No broadcast |
| | Otherwise | `chat_res{SUCCESS}` then a `chat_noti` broadcast |

Broadcast:

- Recipients: every session with the LOBBY tunnel open, including the sender. Login state is not checked.
- The sender receives `chat_res` first, then `chat_noti`.

Message-level errors:

| Situation | Server action |
|---|---|
| Unknown msgid | Log and ignore |
| Missing required field | Log and ignore |
| Payload of 1-3 bytes (shorter than a msgid) | Close the connection |
| protobuf parse failure | Close the connection |

`.proto` conventions:

- `syntax = "proto2"`. The package name is part of the msgid, so package and message names are wire contracts.
- Message names are lower snake_case with suffix `_req` / `_res` / `_noti`.
- The first field of every response is `required int32 error_code_ = 1;`.
- `req` and `res` are paired by name only. There is no correlation id in the header. Two concurrent requests of the same kind cannot be told apart.

## 9. Error codes

`ProtoLib/error_code.proto`, package `nserror`. Carried in the first field `error_code_` of every `_res`.

| Value | Name | Meaning |
|---|---|---|
| 0 | `SUCCESS` | Success |
| 1 | `SYSTEM_ERROR` | System error |
| 200 | `NOT_LOGGED_IN` | Request before login |
| 201 | `ALREADY_LOGGED_IN` | This session is already logged in |
| 202 | `INVALID_NAME` | Name empty, too long, or not UTF-8 |
| 203 | `INVALID_TEXT` | Chat text not UTF-8 |

Ranges (100 per domain): 1-99 system, 100-199 auth, 200-299 lobby, 300-399 item, 400-499 quest, 500-599 region.

## 10. Flow

```
client                                       server
    |--- TCP connect -------------------------->|
    |--- CONNECT  tunnel=11 sid=0.0.11.0 ------>|   open the LOBBY tunnel (anycast)
    |<-- CONNECT  tunnel=11 sid=0.0.11.1 -------|   accepted; use this sid in later DATA
    |--- DATA chat.login_req{name} ------------>|
    |<-- DATA chat.login_res{0, player_id} -----|
    |--- DATA chat.chat_req{text} ------------->|
    |<-- DATA chat.chat_res{0} -----------------|
    |<-- DATA chat.chat_noti{id, name, text} ---|   also to every other session with the tunnel open
    |--- HEARTBEAT NOOPREQ -------------------->|   every 3 seconds
    |<-- HEARTBEAT NOOPRES ---------------------|
```

Failure cases:

| Client behaviour | Server response |
|---|---|
| DATA without opening the tunnel | FAILED. Connection stays up |
| CONNECT to a tunnel this server does not serve (e.g. REGION) | FAILED. Connection stays up |
| `frame_len` < 2 or > 1 MiB | Connection closed |
| Tunnel packet too short to hold the 8-byte `server_sid` | Connection closed |
| Tunnel packet with tunnel id (`param_`) ≥ 32 | Connection closed |
| Nothing sent for 5 seconds | Connection closed |

## 11. Byte examples

Server sid is `0.0.11.1`. `|` is a visual separator, not data.

CONNECT request (anycast), 14 bytes:
```
00 00 00 0A | 01 | 0B | 00 00 00 00 00 0B 00 00
```

CONNECT accept, 14 bytes:
```
00 00 00 0A | 01 | 0B | 00 00 00 00 00 0B 00 01
```

`chat.login_req{name="alice"}`, 25 bytes:
```
00 00 00 15 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 87 29 27 DF | 0A 05 61 6C 69 63 65
```

`chat.login_res{error_code_=0, player_id=1}`, 22 bytes:
```
00 00 00 12 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 89 29 2B 05 | 08 00 10 01
```

`chat.chat_req{text="hi"}`, 22 bytes:
```
00 00 00 12 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 42 A3 E0 C2 | 0A 02 68 69
```

`chat.chat_res{error_code_=0}`, 20 bytes:
```
00 00 00 10 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 40 A3 DD 9C | 08 00
```

`chat.chat_noti{player_id=1, name="alice", text="hi"}`, 31 bytes:
```
00 00 00 1B | 03 | 0B | 00 00 00 00 00 0B 00 01 | 99 8A 08 8A | 08 01 12 05 61 6C 69 63 65 1A 02 68 69
```

NOOPREQ and NOOPRES, 6 bytes each:
```
00 00 00 02 | 06 | 03
00 00 00 02 | 06 | 04
```

FAILED in reply to a REGION anycast CONNECT, 14 bytes. `type_` = 4, `param_` = 13 = 0x0D:
```
00 00 00 0A | 04 | 0D | 00 00 00 00 00 0D 00 00
```

## 12. Writing a client in another language

1. Integers are big-endian. In Godot set `big_endian = true` on the `StreamPeer`.
2. Read the 4-byte `frame_len` first, then read exactly that many bytes.
3. Implement the msgid hash of section 7 yourself, truncated to 32 bits. Check it against the vectors in section 7 and the values in section 8.
4. Encode the body with that language's protobuf. Use the `.proto` files in `ProtoLib/` as they are.
5. Send CONNECT right after connecting. Store the sid from the accept packet and use it in later DATA.
6. Send NOOPREQ every 3 seconds. Without it the server closes the connection after 5 seconds.
7. Strings are UTF-8.
8. Compare your output byte by byte with the examples in section 11.

## 13. Design decisions

| Item | Decision | Reason |
|---|---|---|
| Byte order | Big-endian. Assembled field by field, never by copying a struct | The same bytes on any host. Easy to implement in non-C++ clients (Godot etc.) |
| Length field | One `frame_len`, excluding itself | The receiver reads 4 bytes and then exactly that many |
| `type_` | One operation per value (1..6) | One branch. One byte is enough |
| `param_` | Tunnel id for tunnel packets, sub-command for heartbeats | The tunnel is readable in a dump (`0B` = LOBBY) |
| Tunnels | One logical channel per service type. A tunnel table per connection | Splitting the gateway into its own process does not change the client protocol |
| sid | 4 × 16 bits. `id` = 0 is anycast | The client only needs the server type. The server picks the instance. `idc` is kept for regional (IDC) separation |
| `server_sid` on DATA | Carried on every tunnel packet | It is the key a gateway uses to choose among several backend servers. Today it equals the tunnel-table value because everything runs in one process; after the gateway split it drives routing (decided 2026-10-01) |
| msgid | FNV-1a 32-bit of the message name | No hand-maintained id table. Renaming a message shows up immediately as "unknown msgid" |
| Body | protobuf proto2, full runtime | Schema evolution and cross-language support. `DebugString()` and reflection make diagnostics easy |

## 14. Implementation rules

Rules kept in the implementation, independent of the wire format.

| Rule | Reason |
|---|---|
| A `frame_len` above `MAX_FRAME_SIZE` (1 MiB) closes the connection **before allocating** | Four length bytes must not be able to force a large allocation |
| Missing bytes raise an exception and close the connection | A truncated field is never read as garbage |
| Unknown `type_` and unknown msgid are logged and ignored; the connection stays up | Extension point for new types and messages |
| DATA on a closed tunnel is answered with FAILED | The client can reopen the tunnel instead of being dropped silently |
| One copy of the wire code, in `common/lpn/` | Server, clients and tests share it |
| Constants only in `wire.h`, layout only in `frame.h` | A wire change touches exactly two files |

## 15. Change guide

Any change here must be applied to every client in this repository, in every language.

### 15.1 Wire decisions

| # | Item | Current | Defined in | Reasons one might change it | Change together |
|---|---|---|---|---|---|
| W1 | Byte order | Big-endian | `put_u16/u32/u64`, `get_*` in `wire.h` | Target machines and Godot default to little-endian, so conversion cost would drop to zero | Helper implementations only; call sites unchanged. Golden vectors |
| W3 | Length width | `uint32` | Helper call sites (`frame.h`) | `uint16` saves 2 bytes and caps frames at 64 KiB naturally | `frame.h`, `MAX_FRAME_SIZE` |
| W4 | What the length covers | Excludes itself | `frame.h` | A total length simplifies the reader's arithmetic | `frame.h`, golden vectors |
| W5 | `type_` values | 1..6 | `enum packet_type` in `wire.h` | New operations | `wire.h`, `frame.h` (`is_tunnel_packet`), receive branches in server and clients, golden vectors |
| W6 | Meaning of `param_` | Tunnel id (0..31) or sub-command | `TUNNEL_COUNT` in `wire.h`, `frame.h` | More than 32 tunnels | `wire.h`, `frame.h`, the session's tunnel table size |
| W7 | `server_sid` on every tunnel packet | 8 bytes. **Decided to keep** (section 13) | `frame.h` | Dropping it from DATA saves 8 bytes per packet, but loses the backend routing key after the gateway split | Per-operation branch in `frame.h`, remove the mismatch log of section 5, golden vectors |
| W8 | sid composition | 4 × 16 bits (`domain.idc.type.id`). **Decided to keep** | `sid.h` | A single server group only needs `type` and `id` (32 bits), but the multi-region idea needs `idc` | `sid.h`, and `frame.h` sizes unless W7 is done too |
| W9 | msgid width and algorithm | FNV-1a 32-bit, big-endian | `msgid.h`, the reads in `frame_io`/dispatcher | 64 bits to stop worrying about collisions; enum ordinals (2 bytes) for readability | `msgid.h`, dispatcher, hash implementations in other-language clients, reference msgids |
| W10 | msgid input | Full name including the package | `msgid_of<T>()` in `msgid.h` | Keeping ids stable across package renames | `msgid.h`. Every msgid changes |
| W11 | Tunnel id values | 2, 4, 9, 10, 11, 13, 14 | `enum tunnel` in `wire.h` | Renumbering from 1 | `wire.h`, and the `type` values of sids |
| W12 | Heartbeat sub-command values | 1..4 | `wire.h` | Merge PING/NOOP if the distinction is not needed | `wire.h`, golden vectors |
| W13 | Heartbeat period and timeout | 3000 ms / 5000 ms | Constants in `wire.h` | High-latency environments such as mobile | Constants only. Wire bytes unchanged |
| W14 | Maximum frame size | 1 MiB | `MAX_FRAME_SIZE` in `wire.h` | Stronger memory protection | Constant only. Normal packets are far smaller |
| W15 | protobuf syntax | proto2 | `ProtoLib/*.proto` | proto3's simplicity | Same field numbers and types keep the wire compatible. The meaning of the `required` check (`IsInitialized`) changes |

W2 (duplicate length field) was removed; only its number remains.

### 15.2 Features not present yet, and how to add them

"Compatible" here means compatible with this repository's existing clients.

| Feature | Compatible way | Incompatible way |
|---|---|---|
| Protocol version exchange | Define a new `type_` (e.g. 7 = HANDSHAKE). Older peers ignore unknown types; no reply means "old version" | Mandatory handshake right after connect; close on failure |
| Compression/encryption flag | Define a new `type_` (e.g. 8) meaning "transformed DATA". Older peers ignore it | Add a `flags` byte to L2 (shifts offsets) |
| Request/response pairing (sequence) | Put it in a protobuf field of the messages that need it | Add `uint32 seq` before the msgid |
| Notices | Define a new `type_` (e.g. 9 = NOTICE) and its payload format | - |

### 15.3 Change procedure

1. Pick the item in 15.1 and check "change together".
2. Edit `wire.h` or `frame.h`. If another file needs changing, wire knowledge has leaked; move it into `common/lpn/` first.
3. Confirm that `wire_test` fails, then update its expected bytes.
4. Update the tables and section 11 examples in this document, and the wire protocol section of `CLAUDE.md`.
5. Apply the same change to any non-C++ client.

## 16. protobuf choices

| # | Item | Decision | Reason |
|---|---|---|---|
| P1 | Obtaining it | git submodule `third_party/protobuf`, tag `v3.21.12` (same commit as `v21.12`) | Same approach as asio. Offline builds work |
| P2 | Version | 3.21.x | The last series without the Abseil dependency. 22+ must build Abseil too and adds dozens of link targets, which does not fit a hand-maintained VS solution. Full proto2 support. Being an end-of-life series is accepted |
| P3 | Runtime | full (`libprotobuf`) | `DebugString()` and reflection make packet logs and test diagnostics easy. Size is not an issue for a server and desktop clients. lite shares the wire format, so a mobile C++ client could use lite later |
| P4 | Message scope | Chat messages only (`ProtoLib/chat.proto`, `ProtoLib/error_code.proto`) | Authentication, accounts and DB are out of scope |
| P5 | Default server sid | `0.0.11.1` (domain 0, idc 0, type LOBBY, id 1). Changeable with `--sid` | Anycast CONNECTs are accepted with this value |

Build integration (CMake, VS solution): `docs/specs/2026-09-17-asio-cmake-migration-design.md` section 4.2.
