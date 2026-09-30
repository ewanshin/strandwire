#pragma once

// asio helpers that move whole LPN frames over a stream.

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
template <class AsyncWriteStream>
asio::awaitable<void> async_write_frame(AsyncWriteStream& stream, const frame& f)
{
    const std::vector<char> bytes = encode(f);
    co_await asio::async_write(stream, asio::buffer(bytes), asio::use_awaitable);
}

// Blocking variant for the clients, which send little and only from one thread.
template <class SyncWriteStream>
void write_frame(SyncWriteStream& stream, const frame& f)
{
    const std::vector<char> bytes = encode(f);
    asio::write(stream, asio::buffer(bytes));
}

// An encoded packet owned by reference count. A broadcast encodes once and hands the same buffer
// to every recipient's send queue; the bytes are freed when the last write completes.
using shared_buffer = std::shared_ptr<const std::vector<char>>;

// Encoded packet that can be queued on many sessions without copying.
inline shared_buffer make_shared_buffer(const frame& f)
{
    return std::make_shared<const std::vector<char>>(encode(f));
}

} // namespace lpn
