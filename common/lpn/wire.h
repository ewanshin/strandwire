#pragma once

// Every constant of the LPN wire protocol lives in this file, and the byte layout lives in frame.h.
// Nothing else in the repository may know a wire value or an offset.
// Reference and change guide: docs/protocol.md
//
// LPN 와이어 프로토콜의 모든 상수는 이 파일에, 바이트 배치는 frame.h에 있다.
// 저장소의 다른 어느 곳도 와이어 값이나 오프셋을 알아서는 안 된다.
// 참조와 변경 안내: docs/protocol.md

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lpn
{

// L2 type_ : what the packet does. 0 is never sent. Unknown values are ignored by the receiver,
// which makes new values the extension point (version handshake, compressed packets, ...).
// connect..shift are tunnel packets: param_ is the tunnel id and a 64-bit server sid follows.
//
// L2 type_ : 패킷이 하는 일. 0은 절대 보내지 않는다. 수신자는 모르는 값을 무시하므로
// 새 값이 확장 지점이 된다 (버전 핸드셰이크, 압축 패킷, ...).
// connect..shift는 터널 패킷이다. param_은 터널 id이고 64비트 서버 sid가 뒤따른다.
enum class packet_type : std::uint8_t
{
    // open a tunnel (client -> server) / accepted (server -> client)
    // 터널 열기 (client -> server) / 수락 (server -> client)
    connect = 1,
    disconnect = 2, // close a tunnel / 터널 닫기
    // payload: uint32 msgid + protobuf body; empty payload = keep-alive
    // 페이로드: uint32 msgid + protobuf 본문. 빈 페이로드 = keep-alive
    data = 3,
    // server -> client: the request on this tunnel failed
    // server -> client: 이 터널의 요청이 실패했다
    failed = 4,
    // server -> client: the tunnel moved to another server; payload = previous sid
    // server -> client: 터널이 다른 서버로 옮겨졌다. 페이로드 = 이전 sid
    shift = 5,
    heartbeat = 6, // param_ is a heartbeat_command; no sid / param_은 heartbeat_command이다. sid 없음
};

inline constexpr bool is_tunnel_packet(packet_type t)
{
    return t >= packet_type::connect && t <= packet_type::shift;
}

// param_ when type_ == heartbeat. 0 is never sent.
// type_ == heartbeat일 때의 param_. 0은 절대 보내지 않는다.
enum class heartbeat_command : std::uint8_t
{
    ping_req = 1,
    ping_res = 2,
    noop_req = 3,
    noop_res = 4,
};

// Tunnel id: param_ of a tunnel packet. Equals the server type (the `type` part of a sid).
// 터널 id: 터널 패킷의 param_. 서버 타입(sid의 `type` 부분)과 같다.
enum class tunnel : std::uint8_t
{
    unknown = 0,
    gws = 2,
    auth = 4,
    quest = 9,
    item = 10,
    lobby = 11,
    region = 13,
    ai = 14,
};

// tunnels per connection (tunnel_id 0..31)
// 연결당 터널 수 (tunnel_id 0..31)
inline constexpr std::uint8_t TUNNEL_COUNT = 32;

// Not visible on the wire unless exceeded. Checked before the body is allocated.
// 넘지 않는 한 와이어에 보이지 않는다. 본문을 할당하기 전에 검사한다.
inline constexpr std::uint32_t MAX_FRAME_SIZE = 1024 * 1024;

// client NOOPREQ period
// 클라이언트의 NOOPREQ 주기
inline constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL{3000};
// server drops a silent connection
// 서버가 조용한 연결을 끊는 시간
inline constexpr std::chrono::milliseconds SESSION_TIMEOUT{5000};

// ---- big-endian helpers -------------------------------------------------------------------
// Assembled byte by byte, so the output is the same on any host endianness.
//
// big-endian 헬퍼. 바이트 단위로 조립하므로 호스트의 엔디언과 상관없이 출력이 같다.

inline void put_u16(char* p, std::uint16_t v)
{
    p[0] = static_cast<char>(v >> 8);
    p[1] = static_cast<char>(v);
}

inline void put_u32(char* p, std::uint32_t v)
{
    p[0] = static_cast<char>(v >> 24);
    p[1] = static_cast<char>(v >> 16);
    p[2] = static_cast<char>(v >> 8);
    p[3] = static_cast<char>(v);
}

inline void put_u64(char* p, std::uint64_t v)
{
    put_u32(p, static_cast<std::uint32_t>(v >> 32));
    put_u32(p + 4, static_cast<std::uint32_t>(v));
}

inline std::uint16_t get_u16(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint16_t>((u[0] << 8) | u[1]);
}

inline std::uint32_t get_u32(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (static_cast<std::uint32_t>(u[0]) << 24) | (static_cast<std::uint32_t>(u[1]) << 16) |
           (static_cast<std::uint32_t>(u[2]) << 8) | static_cast<std::uint32_t>(u[3]);
}

inline std::uint64_t get_u64(const char* p)
{
    return (static_cast<std::uint64_t>(get_u32(p)) << 32) | get_u32(p + 4);
}

} // namespace lpn
