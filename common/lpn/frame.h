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
//
// LPN 패킷 하나의 바이트 배치다. 오프셋을 아는 파일은 이 파일뿐이다.
//
//   오프셋 크기 필드
//     0     4  frame_len   이 필드 뒤에 오는 바이트 수                            [L1 전송]
//     4     1  type_       packet_type: 동작                                     [L2 LPN 헤더]
//     5     1  param_      터널 패킷: 터널 id. 하트비트: heartbeat_command
//     6     8  server_sid  터널 패킷(connect..shift)에만 있다                      [L2b]
//    14     N  payload     type_ == data일 때: uint32 msgid + protobuf 본문       [L3/L4]
//
// 모든 정수는 big-endian이다. 터널 패킷이 아니면 페이로드는 오프셋 6에서 시작한다.

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

inline constexpr std::size_t FRAME_LEN_SIZE = 4;  // L1 frame_len / L1 프레임 길이
inline constexpr std::size_t LPN_HEADER_SIZE = 2; // L2 type_ + param_ / L2의 type_와 param_
inline constexpr std::size_t SERVER_SID_SIZE = 8;

struct frame
{
    packet_type type = packet_type::data;
    // tunnel packet: tunnel id; heartbeat: heartbeat_command
    // 터널 패킷: 터널 id. 하트비트: heartbeat_command
    std::uint8_t param = 0;
    std::uint64_t server_sid = 0; // meaningful only for tunnel packets / 터널 패킷에서만 의미가 있다
    std::vector<char> payload;

    bool is_tunnel() const
    {
        return is_tunnel_packet(type);
    }
    std::uint8_t tunnel_id() const
    {
        return param;
    } // meaningful only for tunnel packets / 터널 패킷에서만 의미가 있다
};

// type must be one of connect..shift.
// type은 connect..shift 중 하나여야 한다.
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
// frame_len을 포함한 전체 패킷이다.
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
// body = L1 길이 뒤에 따라온 frame_len 바이트다. 일관성이 깨지면 protocol_error를 던진다.
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
        // 수신자는 이 값으로 터널 테이블을 인덱싱한다.
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
