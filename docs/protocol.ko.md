# 프로토콜 (LPN)

[English](protocol.md) | 한국어

서버와 클라이언트가 TCP로 주고받는 바이트, 그 뒤의 설계 결정, 그리고 바꾸는 방법.

- 구현: `common/lpn/`. 상수는 `wire.h`, 바이트 배치는 `frame.h`.
- 예제 검증: `tests/wire_test.cpp`의 `test_documented_examples`.
- 1-12절은 구조를 설명한다. 13-16절은 결정과 변경 가이드를 기록한다.

## 1. 기본 규칙

| 항목 | 값 |
|---|---|
| 전송 | TCP. 기본 포트 10000 |
| 바이트 순서 | **big-endian** |
| 본문 | protobuf (proto2) |
| 문자열 | UTF-8. 서버는 그 외를 모두 거부한다 |
| 최대 크기 | `frame_len` ≤ 1 MiB (1,048,576). 더 크면 연결을 닫는다 |
| 하트비트 | 클라이언트가 3초마다 하나 보낸다 |
| 타임아웃 | 서버는 아무 패킷도 없이 5초가 지나면 연결을 닫는다 |

## 2. 계층

```
L1  transport frame  uint32 frame_len
L2  LPN header       uint8 type_ | uint8 param_
L2b server sid       uint64 server_sid          (tunnel packets only: type_ = 1..5)
L3  msgid            uint32                     (type_ = DATA with a body only)
L4  body             protobuf, no length prefix, up to the end of the frame
```

수신 절차:

1. 4바이트를 읽는다 → `frame_len`. 2 미만이거나 1 MiB 초과면 닫는다.
2. `frame_len` 바이트를 읽는다. 패킷 전체는 `frame_len + 4` 바이트다.
3. `type_`로 분기한다. 알 수 없는 값은 무시한다.
4. 터널 패킷이면: `param_`(터널 id)이 32 이상이거나 뒤따르는 sid 바이트가 8보다 적으면 닫는다.

## 3. 패킷 배치

### 3.1 터널 패킷 (`type_` = 1..5)

| 오프셋 | 크기 | 필드 | 타입 | 의미 |
|---|---|---|---|---|
| 0 | 4 | `frame_len` | `uint32` | 이 필드 뒤에 오는 바이트 수. 자기 자신은 제외한다 |
| 4 | 1 | `type_` | `uint8` | 동작: 1 CONNECT, 2 DISCONNECT, 3 DATA, 4 FAILED, 5 SHIFT (4.1절) |
| 5 | 1 | `param_` | `uint8` | 터널 id, 0..31 (4.3절) |
| 6 | 8 | `server_sid` | `uint64` | 서버 인스턴스. 의미는 5절 |
| 14 | 4 | `msgid` | `uint32` | `type_` = DATA이고 본문이 있을 때만 |
| 18 | N | 본문 | bytes | protobuf 메시지 |

- `payload` = 오프셋 14부터 끝까지.
- `frame_len = 10 + payload`.
- DATA의 고정 오버헤드: 18바이트.

### 3.2 하트비트 (`type_` = 6, 6바이트)

| 오프셋 | 크기 | 필드 | 값 |
|---|---|---|---|
| 0 | 4 | `frame_len` | 2 |
| 4 | 1 | `type_` | 6 |
| 5 | 1 | `param_` | 하위 명령 (4.2절) |

## 4. 상수

### 4.1 패킷 타입 (`type_`)

| 값 | 이름 | 의미 | 방향 | `param_` | sid |
|---|---|---|---|---|---|
| 0 | - | 보내지 않는다 | - | - | - |
| 1 | CONNECT | 터널 열기 / 수락 | 클라이언트 → 서버, 서버 → 클라이언트 | 터널 id | 있음 |
| 2 | DISCONNECT | 터널 닫기 | 양방향 | 터널 id | 있음 |
| 3 | DATA | 터널로 메시지 전송 | 양방향 | 터널 id | 있음 |
| 4 | FAILED | 요청 실패 | 서버 → 클라이언트 | 터널 id | 있음 |
| 5 | SHIFT | 터널이 다른 서버로 옮겨짐 | 서버 → 클라이언트 | 터널 id | 있음 |
| 6 | HEARTBEAT | 생존 확인 | 양방향 | 하위 명령 | 없음 |
| 그 외 | - | 로그만 남기고 무시한다. 연결은 유지된다. 새 타입을 위한 확장 지점 | - | - | 없음 |

1..5를 "터널 패킷"이라 부른다. 그 규칙은 5절에 있다.

### 4.2 하트비트 하위 명령

| 값 | 이름 | 방향 | 응답 |
|---|---|---|---|
| 0 | - | 보내지 않는다 | - |
| 1 | PINGREQ | 클라이언트 → 서버 | PINGRES |
| 2 | PINGRES | 서버 → 클라이언트 | - |
| 3 | NOOPREQ | 클라이언트 → 서버 | NOOPRES |
| 4 | NOOPRES | 서버 → 클라이언트 | - |

- 클라이언트는 NOOPREQ를 쓴다. PING은 왕복 시간 측정용으로 남겨 둔다.
- 서버는 어떤 패킷이든 받으면 타임아웃을 다시 센다.

### 4.3 터널 id (서버 타입)

id 하나에 터널 하나. 연결당 최대 32개(0..31). 터널 id는 sid의 `type` 부분과 같다. 표에 없는 값은 예약되어 있다.

| 값 | 서버 타입 | 이 서버 |
|---|---|---|
| 2 | GWS (게이트웨이) | - |
| 4 | AUTH | - |
| 9 | QUEST | - |
| 10 | ITEM | - |
| 11 | LOBBY | **서비스함** |
| 13 | REGION | - |
| 14 | AI | - |

예: LOBBY CONNECT = `01 0B`. LOBBY DATA = `03 0B`. REGION FAILED = `04 0D`.

## 5. 터널 패킷 동작

서버는 연결마다 `터널 id → 바인딩된 server_sid` 테이블을 둔다(크기 32, 0 = 닫힘).

| `type_` | `server_sid` | 페이로드 | 서버 동작 |
|---|---|---|---|
| CONNECT (요청) | 열 서버. `id` = 0은 anycast | 없음 | 수락하면 CONNECT, 거부하면 FAILED. 이미 열린 터널은 그냥 다시 바인딩한다 |
| CONNECT (수락) | 실제로 바인딩된 서버 | 없음 | - |
| DATA | 바인딩된 서버 | `msgid` + protobuf | 닫힌 터널: FAILED. 열린 터널: 메시지를 처리한다 |
| DATA (길이 0) | 바인딩된 서버 | 없음 | 생존 갱신만 |
| DISCONNECT | 닫을 서버 | 없음 | 바인딩된 값과 같으면 닫고 DISCONNECT로 응답한다. 다르면 무시한다 |
| SHIFT | **새** 서버 | 8바이트: 이전 서버 sid | 서버가 서비스 인스턴스를 바꿀 때 보낸다. 이 서버는 보내지 않는다 |
| FAILED | 실패한 대상 | 없음 | - |

CONNECT는 다음 두 조건이 모두 맞을 때 수락한다:

- 터널 id = 요청한 sid의 `type` = 서버 sid의 `type`.
- 요청한 sid가 anycast이거나 서버 sid와 같다.

그 밖의 규칙:

- SHIFT를 받은 클라이언트는 테이블만 갱신하고 이후 DATA에 새 sid를 쓴다.
- DATA의 sid가 바인딩된 값과 다르면 서버는 로그를 남기고 바인딩된 값으로 라우팅한다. 터널 테이블이 기준이다.

## 6. 서버 sid (64비트)

| 비트 | 필드 | 의미 |
|---|---|---|
| 63..48 | `domain` | 도메인 (서비스) |
| 47..32 | `idc` | 데이터 센터 |
| 31..16 | `type` | 서버 타입. 터널 id와 같은 값 |
| 15..0 | `id` | 인스턴스 번호. **0 = anycast** (그 타입의 아무 인스턴스) |

- `value = id | type << 16 | idc << 32 | domain << 48`.
- 텍스트 형식: `domain.idc.type.id`.
- 와이어에서: 64비트 big-endian, `domain`의 상위 바이트가 먼저.
- 서버 기본값 `0.0.11.1` → `00 00 00 00 00 0B 00 01`. `--sid`로 바꿀 수 있다.
- LOBBY anycast `0.0.11.0` → `00 00 00 00 00 0B 00 00`.

## 7. msgid

`msgid = FNV-1a 32-bit(패키지를 포함한 전체 메시지 이름)`

```
hash = 2166136261
for each byte b of the name:
    hash = hash XOR b
    hash = (hash × 16777619) mod 2^32
```

- id 테이블은 없다. 이름이 곧 id다. 메시지 이름을 바꾸면 msgid가 바뀐다.
- 표준 protoc. protobuf 런타임이 알려 주는 이름(`GetTypeName()`)을 등록 시점에 해시한다.
- msgid는 터널 안에서만 유일하면 된다. dispatcher는 터널마다 독립이다.
- 구현 확인용 참조 값(공개된 FNV-1a 32-bit 테스트 벡터): `""` = `0x811C9DC5`, `"a"` = `0xE40C292C`, `"foobar"` = `0xBF9CF968`.

디스패치:

- 터널마다 `msgid → (프로토타입 메시지, 핸들러)` 테이블.
- 등록은 함수 포인터 하나를 받는다. 메시지 타입과 msgid는 그 시그니처에서 추론한다.
- 한 터널에 같은 msgid의 핸들러가 둘이면 서버 시작을 거부한다.
- 수신: `msgid`를 읽고, `New()`로 프로토타입을 복제하고, 남은 바이트를 파싱한다. 파싱 실패와 `IsInitialized()` 실패는 구분한다.

## 8. LOBBY 터널 메시지

`ProtoLib/chat.proto`, 패키지 `chat`.

| 메시지 | msgid (10진) | msgid (16진) | 방향 |
|---|---|---|---|
| `chat.login_req` | 2267621343 | 87 29 27 DF | 클라이언트 → 서버 |
| `chat.login_res` | 2301176581 | 89 29 2B 05 | 서버 → 클라이언트 |
| `chat.chat_req` | 1118036162 | 42 A3 E0 C2 | 클라이언트 → 서버 |
| `chat.chat_res` | 1084480924 | 40 A3 DD 9C | 서버 → 클라이언트 |
| `chat.chat_noti` | 2575960202 | 99 8A 08 8A | 서버 → 전원 |

필드:

| 메시지 | 번호 | 필드 | 타입 | 규칙 |
|---|---|---|---|---|
| `login_req` | 1 | `name` | `string` | required. 1-64바이트, UTF-8 |
| `login_res` | 1 | `error_code_` | `int32` | required |
| | 2 | `player_id` | `int32` | optional. 성공 시에만. 1부터 시작해 증가한다 |
| `chat_req` | 1 | `text` | `string` | required. UTF-8 |
| `chat_res` | 1 | `error_code_` | `int32` | required |
| `chat_noti` | 1 | `player_id` | `int32` | required. 보낸 사람 |
| | 2 | `name` | `string` | required. 서버가 채운다. 위조할 수 없다 |
| | 3 | `text` | `string` | required |

서버 처리:

| 요청 | 조건 | 응답 |
|---|---|---|
| `login_req` | 이미 로그인됨 | `login_res{ALREADY_LOGGED_IN}` |
| | 이름이 비었거나, 64바이트를 넘거나, UTF-8이 아님 | `login_res{INVALID_NAME}` |
| | 그 외 | `login_res{SUCCESS, player_id}` |
| `chat_req` | 로그인 전 | `chat_res{NOT_LOGGED_IN}` |
| | UTF-8이 아님 | `chat_res{INVALID_TEXT}`. 브로드캐스트 없음 |
| | 그 외 | `chat_res{SUCCESS}` 다음 `chat_noti` 브로드캐스트 |

브로드캐스트:

- 수신자: LOBBY 터널이 열린 모든 세션, 보낸 사람 포함. 로그인 상태는 확인하지 않는다.
- 보낸 사람은 `chat_res`를 먼저 받고 그다음 `chat_noti`를 받는다.

메시지 수준 오류:

| 상황 | 서버 동작 |
|---|---|
| 알 수 없는 msgid | 로그만 남기고 무시한다 |
| required 필드 누락 | 로그만 남기고 무시한다 |
| 페이로드가 1-3바이트 (msgid보다 짧음) | 연결을 닫는다 |
| protobuf 파싱 실패 | 연결을 닫는다 |

`.proto` 규약:

- `syntax = "proto2"`. 패키지 이름이 msgid의 일부이므로 패키지 이름과 메시지 이름은 와이어 계약이다.
- 메시지 이름은 소문자 snake_case에 접미사 `_req` / `_res` / `_noti`를 붙인다.
- 모든 응답의 첫 필드는 `required int32 error_code_ = 1;`이다.
- `req`와 `res`는 이름으로만 짝을 이룬다. 헤더에 상관 id는 없다. 같은 종류의 요청 둘이 동시에 있으면 구분할 수 없다.

## 9. 오류 코드

`ProtoLib/error_code.proto`, 패키지 `nserror`. 모든 `_res`의 첫 필드 `error_code_`에 실린다.

| 값 | 이름 | 의미 |
|---|---|---|
| 0 | `SUCCESS` | 성공 |
| 1 | `SYSTEM_ERROR` | 시스템 오류 |
| 200 | `NOT_LOGGED_IN` | 로그인 전 요청 |
| 201 | `ALREADY_LOGGED_IN` | 이 세션은 이미 로그인됨 |
| 202 | `INVALID_NAME` | 이름이 비었거나, 너무 길거나, UTF-8이 아님 |
| 203 | `INVALID_TEXT` | 채팅 텍스트가 UTF-8이 아님 |

범위(도메인당 100): 1-99 시스템, 100-199 인증, 200-299 로비, 300-399 아이템, 400-499 퀘스트, 500-599 리전.

## 10. 흐름

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

실패 사례:

| 클라이언트 동작 | 서버 응답 |
|---|---|
| 터널을 열지 않고 DATA | FAILED. 연결은 유지된다 |
| 이 서버가 서비스하지 않는 터널(예: REGION)에 CONNECT | FAILED. 연결은 유지된다 |
| `frame_len` < 2 또는 > 1 MiB | 연결을 닫는다 |
| 8바이트 `server_sid`를 담기에 너무 짧은 터널 패킷 | 연결을 닫는다 |
| 터널 id(`param_`)가 32 이상인 터널 패킷 | 연결을 닫는다 |
| 5초 동안 아무것도 보내지 않음 | 연결을 닫는다 |

## 11. 바이트 예제

서버 sid는 `0.0.11.1`이다. `|`는 눈으로 구분하기 위한 표시이며 데이터가 아니다.

CONNECT 요청 (anycast), 14바이트:
```
00 00 00 0A | 01 | 0B | 00 00 00 00 00 0B 00 00
```

CONNECT 수락, 14바이트:
```
00 00 00 0A | 01 | 0B | 00 00 00 00 00 0B 00 01
```

`chat.login_req{name="alice"}`, 25바이트:
```
00 00 00 15 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 87 29 27 DF | 0A 05 61 6C 69 63 65
```

`chat.login_res{error_code_=0, player_id=1}`, 22바이트:
```
00 00 00 12 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 89 29 2B 05 | 08 00 10 01
```

`chat.chat_req{text="hi"}`, 22바이트:
```
00 00 00 12 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 42 A3 E0 C2 | 0A 02 68 69
```

`chat.chat_res{error_code_=0}`, 20바이트:
```
00 00 00 10 | 03 | 0B | 00 00 00 00 00 0B 00 01 | 40 A3 DD 9C | 08 00
```

`chat.chat_noti{player_id=1, name="alice", text="hi"}`, 31바이트:
```
00 00 00 1B | 03 | 0B | 00 00 00 00 00 0B 00 01 | 99 8A 08 8A | 08 01 12 05 61 6C 69 63 65 1A 02 68 69
```

NOOPREQ와 NOOPRES, 각 6바이트:
```
00 00 00 02 | 06 | 03
00 00 00 02 | 06 | 04
```

REGION anycast CONNECT에 대한 FAILED 응답, 14바이트. `type_` = 4, `param_` = 13 = 0x0D:
```
00 00 00 0A | 04 | 0D | 00 00 00 00 00 0D 00 00
```

## 12. 다른 언어로 클라이언트 작성하기

1. 정수는 big-endian이다. Godot에서는 `StreamPeer`에 `big_endian = true`를 설정한다.
2. 4바이트 `frame_len`을 먼저 읽고, 정확히 그만큼의 바이트를 읽는다.
3. 7절의 msgid 해시를 직접 구현하고 32비트로 잘라낸다. 7절의 벡터와 8절의 값으로 확인한다.
4. 그 언어의 protobuf로 본문을 인코딩한다. `ProtoLib/`의 `.proto` 파일을 그대로 쓴다.
5. 연결 직후 CONNECT를 보낸다. 수락 패킷의 sid를 저장하고 이후 DATA에 쓴다.
6. 3초마다 NOOPREQ를 보낸다. 보내지 않으면 서버가 5초 후 연결을 닫는다.
7. 문자열은 UTF-8이다.
8. 출력을 11절의 예제와 바이트 단위로 비교한다.

## 13. 설계 결정

| 항목 | 결정 | 이유 |
|---|---|---|
| 바이트 순서 | big-endian. 구조체를 복사하지 않고 필드 단위로 조립한다 | 어느 호스트에서나 같은 바이트. C++가 아닌 클라이언트(Godot 등)에서 구현하기 쉽다 |
| 길이 필드 | `frame_len` 하나. 자기 자신은 제외한다 | 수신자는 4바이트를 읽고 정확히 그만큼 읽는다 |
| `type_` | 값 하나에 동작 하나 (1..6) | 분기 한 번. 1바이트면 충분하다 |
| `param_` | 터널 패킷은 터널 id, 하트비트는 하위 명령 | 덤프에서 터널을 읽을 수 있다 (`0B` = LOBBY) |
| 터널 | 서비스 타입마다 논리 채널 하나. 연결마다 터널 테이블 | 게이트웨이를 별도 프로세스로 분리해도 클라이언트 프로토콜은 바뀌지 않는다 |
| sid | 4 × 16비트. `id` = 0은 anycast | 클라이언트는 서버 타입만 알면 된다. 인스턴스는 서버가 고른다. `idc`는 지역(IDC) 분리를 위해 남겨 둔다 |
| DATA의 `server_sid` | 모든 터널 패킷에 싣는다 | 게이트웨이가 여러 백엔드 서버 중 하나를 고르는 키다. 지금은 모두 한 프로세스에서 돌아가므로 터널 테이블 값과 같다. 게이트웨이 분리 후에는 라우팅을 결정한다 (2026-10-01 결정) |
| msgid | 메시지 이름의 FNV-1a 32-bit | 손으로 관리하는 id 테이블이 없다. 메시지 이름을 바꾸면 바로 "unknown msgid"로 드러난다 |
| 본문 | protobuf proto2, full 런타임 | 스키마 진화와 다국어 지원. `DebugString()`과 리플렉션으로 진단이 쉽다 |

## 14. 구현 규칙

와이어 형식과 무관하게 구현에서 지키는 규칙.

| 규칙 | 이유 |
|---|---|
| `MAX_FRAME_SIZE`(1 MiB)를 넘는 `frame_len`은 **할당 전에** 연결을 닫는다 | 길이 4바이트로 큰 할당을 강제할 수 없어야 한다 |
| 바이트가 모자라면 예외를 던지고 연결을 닫는다 | 잘린 필드를 쓰레기 값으로 읽는 일은 없다 |
| 알 수 없는 `type_`과 알 수 없는 msgid는 로그만 남기고 무시한다. 연결은 유지된다 | 새 타입과 메시지를 위한 확장 지점 |
| 닫힌 터널로 온 DATA에는 FAILED로 응답한다 | 클라이언트가 조용히 끊기는 대신 터널을 다시 열 수 있다 |
| 와이어 코드는 `common/lpn/`에 한 벌만 둔다 | 서버, 클라이언트, 테스트가 공유한다 |
| 상수는 `wire.h`에만, 배치는 `frame.h`에만 | 와이어 변경은 정확히 두 파일만 건드린다 |

## 15. 변경 가이드

여기서 바꾸는 것은 이 저장소의 모든 클라이언트에, 모든 언어로 적용해야 한다.

### 15.1 와이어 결정

| # | 항목 | 현재 | 정의 위치 | 바꿀 만한 이유 | 함께 바꿀 것 |
|---|---|---|---|---|---|
| W1 | 바이트 순서 | big-endian | `wire.h`의 `put_u16/u32/u64`, `get_*` | 대상 머신과 Godot의 기본이 little-endian이므로 변환 비용이 0이 된다 | 헬퍼 구현만. 호출 지점은 그대로. 골든 벡터 |
| W3 | 길이 폭 | `uint32` | 헬퍼 호출 지점 (`frame.h`) | `uint16`은 2바이트를 아끼고 프레임을 자연스럽게 64 KiB로 제한한다 | `frame.h`, `MAX_FRAME_SIZE` |
| W4 | 길이가 포함하는 범위 | 자기 자신 제외 | `frame.h` | 전체 길이면 수신 측 계산이 단순해진다 | `frame.h`, 골든 벡터 |
| W5 | `type_` 값 | 1..6 | `wire.h`의 `enum packet_type` | 새 동작 | `wire.h`, `frame.h` (`is_tunnel_packet`), 서버와 클라이언트의 수신 분기, 골든 벡터 |
| W6 | `param_`의 의미 | 터널 id (0..31) 또는 하위 명령 | `wire.h`의 `TUNNEL_COUNT`, `frame.h` | 32개를 넘는 터널 | `wire.h`, `frame.h`, 세션의 터널 테이블 크기 |
| W7 | 모든 터널 패킷의 `server_sid` | 8바이트. **유지하기로 결정** (13절) | `frame.h` | DATA에서 빼면 패킷당 8바이트를 아끼지만, 게이트웨이 분리 후 백엔드 라우팅 키를 잃는다 | `frame.h`의 동작별 분기, 5절의 불일치 로그 제거, 골든 벡터 |
| W8 | sid 구성 | 4 × 16비트 (`domain.idc.type.id`). **유지하기로 결정** | `sid.h` | 서버 그룹이 하나면 `type`과 `id`(32비트)만 있으면 되지만, 다중 리전 구상에는 `idc`가 필요하다 | `sid.h`, 그리고 W7을 같이 하지 않으면 `frame.h`의 크기 |
| W9 | msgid 폭과 알고리즘 | FNV-1a 32-bit, big-endian | `msgid.h`, `frame_io`/dispatcher의 읽기 | 충돌 걱정을 없애려면 64비트, 가독성을 위해서는 enum 서수(2바이트) | `msgid.h`, dispatcher, 다른 언어 클라이언트의 해시 구현, 참조 msgid |
| W10 | msgid 입력 | 패키지를 포함한 전체 이름 | `msgid.h`의 `msgid_of<T>()` | 패키지 이름을 바꿔도 id를 유지하려면 | `msgid.h`. 모든 msgid가 바뀐다 |
| W11 | 터널 id 값 | 2, 4, 9, 10, 11, 13, 14 | `wire.h`의 `enum tunnel` | 1부터 다시 번호 매기기 | `wire.h`, 그리고 sid의 `type` 값 |
| W12 | 하트비트 하위 명령 값 | 1..4 | `wire.h` | 구분이 필요 없으면 PING/NOOP 통합 | `wire.h`, 골든 벡터 |
| W13 | 하트비트 주기와 타임아웃 | 3000 ms / 5000 ms | `wire.h`의 상수 | 모바일 같은 고지연 환경 | 상수만. 와이어 바이트는 그대로 |
| W14 | 최대 프레임 크기 | 1 MiB | `wire.h`의 `MAX_FRAME_SIZE` | 더 강한 메모리 보호 | 상수만. 보통 패킷은 훨씬 작다 |
| W15 | protobuf 문법 | proto2 | `ProtoLib/*.proto` | proto3의 단순함 | 필드 번호와 타입이 같으면 와이어 호환이 유지된다. `required` 검사(`IsInitialized`)의 의미가 바뀐다 |

W2(중복 길이 필드)는 제거되었다. 번호만 남아 있다.

### 15.2 아직 없는 기능과 추가 방법

여기서 "호환"은 이 저장소의 기존 클라이언트와 호환된다는 뜻이다.

| 기능 | 호환되는 방법 | 호환되지 않는 방법 |
|---|---|---|
| 프로토콜 버전 교환 | 새 `type_`을 정의한다 (예: 7 = HANDSHAKE). 옛 상대는 알 수 없는 타입을 무시한다. 응답이 없으면 "옛 버전"이다 | 연결 직후 필수 핸드셰이크. 실패하면 닫는다 |
| 압축/암호화 플래그 | "변환된 DATA"를 뜻하는 새 `type_`(예: 8)을 정의한다. 옛 상대는 무시한다 | L2에 `flags` 바이트 추가 (오프셋이 밀린다) |
| 요청/응답 짝 맞추기 (시퀀스) | 필요한 메시지의 protobuf 필드에 넣는다 | msgid 앞에 `uint32 seq` 추가 |
| 공지 | 새 `type_`(예: 9 = NOTICE)과 그 페이로드 형식을 정의한다 | - |

### 15.3 변경 절차

1. 15.1에서 항목을 고르고 "함께 바꿀 것"을 확인한다.
2. `wire.h` 또는 `frame.h`를 고친다. 다른 파일도 고쳐야 한다면 와이어 지식이 새어 나간 것이다. 먼저 `common/lpn/`으로 옮긴다.
3. `wire_test`가 실패하는 것을 확인한 뒤 기대 바이트를 갱신한다.
4. 이 문서의 표와 11절 예제, 그리고 `docs/architecture.md`의 와이어 프로토콜 절을 갱신한다.
5. C++가 아닌 클라이언트에도 같은 변경을 적용한다.

## 16. protobuf 선택

| # | 항목 | 결정 | 이유 |
|---|---|---|---|
| P1 | 가져오기 | git submodule `third_party/protobuf`, 태그 `v3.21.12` (`v21.12`와 같은 커밋) | asio와 같은 방식. 오프라인 빌드가 된다 |
| P2 | 버전 | 3.21.x | Abseil 의존이 없는 마지막 시리즈. 22+는 Abseil도 빌드해야 하고 링크 대상이 수십 개 늘어나 손으로 관리하는 VS 솔루션에 맞지 않는다. proto2 완전 지원. 지원이 끝난 시리즈라는 점은 감수한다 |
| P3 | 런타임 | full (`libprotobuf`) | `DebugString()`과 리플렉션으로 패킷 로그와 테스트 진단이 쉽다. 서버와 데스크톱 클라이언트에는 크기가 문제가 아니다. lite는 와이어 형식이 같으므로 모바일 C++ 클라이언트는 나중에 lite를 쓸 수 있다 |
| P4 | 메시지 범위 | 채팅 메시지만 (`ProtoLib/chat.proto`, `ProtoLib/error_code.proto`) | 인증, 계정, DB는 범위 밖이다 |
| P5 | 서버 sid 기본값 | `0.0.11.1` (domain 0, idc 0, type LOBBY, id 1). `--sid`로 바꿀 수 있다 | anycast CONNECT는 이 값으로 수락한다 |

빌드 통합 (CMake, VS 솔루션): `docs/specs/2026-09-17-asio-cmake-migration-design.md` 4.2절.
