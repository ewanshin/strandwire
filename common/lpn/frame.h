#pragma once

// Byte layout of one LPN packet. This is the only file that knows offsets.
//
//   off  size  field
//     0     4  frame_len   bytes that follow this field                       [L1 transport]
//     4     1  type_       packet_type: the operation                         [L2 LPN header]
//     5     1  param_      tunnel packet: tunnel id; heartbeat: heartbeat_command
//     6     8  server_sid  only for tunnel packets (connect..shift)           [L2b]
//    14     N  payload     for type_ == data: uint32 msgid + protobuf body    [L3/L4]
//
// All integers are big-endian. For non-tunnel packets the payload starts at offset 6.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "common/lpn/wire.h"

namespace lpn
{

struct protocol_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

inline constexpr std::size_t FRAME_LEN_SIZE = 4;  // L1 frame_len
inline constexpr std::size_t LPN_HEADER_SIZE = 2; // L2 type_ + param_
inline constexpr std::size_t SERVER_SID_SIZE = 8;

struct frame
{
    packet_type type = packet_type::data;
    std::uint8_t param = 0;       // tunnel packet: tunnel id; heartbeat: heartbeat_command
    std::uint64_t server_sid = 0; // meaningful only for tunnel packets
    std::vector<char> payload;

    bool is_tunnel() const
    {
        return is_tunnel_packet(type);
    }
    std::uint8_t tunnel_id() const
    {
        return param;
    } // meaningful only for tunnel packets
};

// type must be one of connect..shift.
inline frame make_tunnel(std::uint8_t tunnel_id, packet_type type, std::uint64_t server_sid,
                         std::span<const char> payload = {})
{
    if (!is_tunnel_packet(type))
        throw protocol_error("not a tunnel packet type");
    if (tunnel_id >= TUNNEL_COUNT)
        throw protocol_error("tunnel id out of range");
    frame f;
    f.type = type;
    f.param = tunnel_id;
    f.server_sid = server_sid;
    f.payload.assign(payload.begin(), payload.end());
    return f;
}

inline frame make_tunnel(tunnel t, packet_type type, std::uint64_t server_sid, std::span<const char> payload = {})
{
    return make_tunnel(static_cast<std::uint8_t>(t), type, server_sid, payload);
}

inline frame make_heartbeat(heartbeat_command cmd)
{
    frame f;
    f.type = packet_type::heartbeat;
    f.param = static_cast<std::uint8_t>(cmd);
    return f;
}

// Whole packet, frame_len included.
inline std::vector<char> encode(const frame& f)
{
    const bool tunnel = f.is_tunnel();
    const std::size_t frame_len = LPN_HEADER_SIZE + (tunnel ? SERVER_SID_SIZE : 0) + f.payload.size();
    if (frame_len > MAX_FRAME_SIZE)
        throw protocol_error("frame too large to encode");

    std::vector<char> out(FRAME_LEN_SIZE + frame_len);
    char* p = out.data();
    put_u32(p, static_cast<std::uint32_t>(frame_len));
    p += FRAME_LEN_SIZE;
    *p++ = static_cast<char>(f.type);
    *p++ = static_cast<char>(f.param);
    if (tunnel)
    {
        put_u64(p, f.server_sid);
        p += SERVER_SID_SIZE;
    }
    if (!f.payload.empty())
        std::copy(f.payload.begin(), f.payload.end(), p);
    return out;
}

// body = the frame_len bytes that followed the L1 length. Throws protocol_error on any inconsistency.
inline frame decode_body(std::span<const char> body)
{
    if (body.size() < LPN_HEADER_SIZE)
        throw protocol_error("frame shorter than the LPN header");

    frame f;
    f.type = static_cast<packet_type>(static_cast<std::uint8_t>(body[0]));
    f.param = static_cast<std::uint8_t>(body[1]);

    std::size_t offset = LPN_HEADER_SIZE;
    if (f.is_tunnel())
    {
        // The receiver indexes its tunnel table with this value.
        if (f.param >= TUNNEL_COUNT)
            throw protocol_error("tunnel id out of range");
        if (body.size() < offset + SERVER_SID_SIZE)
            throw protocol_error("tunnel packet shorter than the server sid");
        f.server_sid = get_u64(body.data() + offset);
        offset += SERVER_SID_SIZE;
    }
    f.payload.assign(body.begin() + static_cast<std::ptrdiff_t>(offset), body.end());
    return f;
}

} // namespace lpn
