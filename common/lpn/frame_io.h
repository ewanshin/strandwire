#pragma once

// asio helpers that move whole LPN frames over a stream.
// 스트림 위로 LPN 프레임을 통째로 옮기는 asio 헬퍼다.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>

#include "common/lpn/frame.h"
#include "common/lpn/wire.h"

namespace lpn
{

// Reads exactly one frame. Throws std::system_error on I/O error and protocol_error on a bad frame.
// The L1 length is checked against MAX_FRAME_SIZE before anything is allocated.
//
// 정확히 프레임 하나를 읽는다. I/O 오류에는 std::system_error를, 잘못된 프레임에는 protocol_error를 던진다.
// 무엇이든 할당하기 전에 L1 길이를 MAX_FRAME_SIZE와 비교한다.
template <class AsyncReadStream>
asio::awaitable<frame> read_frame(AsyncReadStream& stream)
{
    char len_buf[FRAME_LEN_SIZE];
    co_await asio::async_read(stream, asio::buffer(len_buf), asio::use_awaitable);

    const std::uint32_t frame_len = get_u32(len_buf);
    if (frame_len > MAX_FRAME_SIZE)
        throw protocol_error("frame_len " + std::to_string(frame_len) + " exceeds MAX_FRAME_SIZE");
    if (frame_len < LPN_HEADER_SIZE)
        throw protocol_error("frame_len shorter than the LPN header");

    std::vector<char> body(frame_len);
    co_await asio::async_read(stream, asio::buffer(body), asio::use_awaitable);
    co_return decode_body(body);
}

// Encodes and writes one frame. asio::async_write only returns after every byte was accepted, so
// one call never leaves a partial packet on the wire. Callers must not overlap two writes on the
// same stream (the server's session serialises them through its send queue).
//
// 프레임 하나를 인코딩해 쓴다. asio::async_write는 모든 바이트가 받아들여진 뒤에야 반환하므로
// 한 번의 호출이 와이어에 패킷 일부만 남기는 일은 없다. 호출자는 같은 스트림에 두 쓰기를 겹쳐서는
// 안 된다 (서버의 세션은 송신 큐로 쓰기를 직렬화한다).
template <class AsyncWriteStream>
asio::awaitable<void> async_write_frame(AsyncWriteStream& stream, const frame& f)
{
    const std::vector<char> bytes = encode(f);
    co_await asio::async_write(stream, asio::buffer(bytes), asio::use_awaitable);
}

// Blocking variant for the clients, which send little and only from one thread.
// 클라이언트용 블로킹 버전이다. 클라이언트는 조금만 보내고 한 스레드에서만 보낸다.
template <class SyncWriteStream>
void write_frame(SyncWriteStream& stream, const frame& f)
{
    const std::vector<char> bytes = encode(f);
    asio::write(stream, asio::buffer(bytes));
}

// An encoded packet owned by reference count. A broadcast encodes once and hands the same buffer
// to every recipient's send queue; the bytes are freed when the last write completes.
//
// 참조 카운트로 소유하는 인코딩된 패킷이다. 브로드캐스트는 한 번만 인코딩하고 같은 버퍼를
// 모든 수신자의 송신 큐에 넘긴다. 마지막 쓰기가 끝나면 바이트가 해제된다.
using shared_buffer = std::shared_ptr<const std::vector<char>>;

// Encoded packet that can be queued on many sessions without copying.
// 복사 없이 여러 세션에 큐잉할 수 있는 인코딩된 패킷이다.
inline shared_buffer make_shared_buffer(const frame& f)
{
    return std::make_shared<const std::vector<char>>(encode(f));
}

} // namespace lpn
