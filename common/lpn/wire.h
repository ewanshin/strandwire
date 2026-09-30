#pragma once

// Every constant of the LPN wire protocol lives in this file, and the byte layout lives in frame.h.
// Nothing else in the repository may know a wire value or an offset.
// Reference and change guide: docs/protocol.md

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lpn
{

// L2 type_ : what the packet does. 0 is never sent. Unknown values are ignored by the receiver,
// which makes new values the extension point (version handshake, compressed packets, ...).
// connect..shift are tunnel packets: param_ is the tunnel id and a 64-bit server sid follows.
enum class packet_type : std::uint8_t
{
    connect = 1,    // open a tunnel (client -> server) / accepted (server -> client)
    disconnect = 2, // close a tunnel
    data = 3,       // payload: uint32 msgid + protobuf body; empty payload = keep-alive
    failed = 4,     // server -> client: the request on this tunnel failed
    shift = 5,      // server -> client: the tunnel moved to another server; payload = previous sid
    heartbeat = 6,  // param_ is a heartbeat_command; no sid
};

inline constexpr bool is_tunnel_packet(packet_type t)
{
    return t >= packet_type::connect && t <= packet_type::shift;
}

// param_ when type_ == heartbeat. 0 is never sent.
enum class heartbeat_command : std::uint8_t
{
    ping_req = 1,
    ping_res = 2,
    noop_req = 3,
    noop_res = 4,
};

// Tunnel id: param_ of a tunnel packet. Equals the server type (the `type` part of a sid).
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

inline constexpr std::uint8_t TUNNEL_COUNT = 32; // tunnels per connection (tunnel_id 0..31)

// Not visible on the wire unless exceeded. Checked before the body is allocated.
inline constexpr std::uint32_t MAX_FRAME_SIZE = 1024 * 1024;

inline constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL{3000}; // client NOOPREQ period
inline constexpr std::chrono::milliseconds SESSION_TIMEOUT{5000};    // server drops a silent connection

// ---- big-endian helpers -------------------------------------------------------------------
// Assembled byte by byte, so the output is the same on any host endianness.

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
