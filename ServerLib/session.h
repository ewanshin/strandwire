#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

#include <asio.hpp>
#include <google/protobuf/message.h>

#include "common/lpn/dispatcher.h"
#include "common/lpn/frame.h"
#include "common/lpn/frame_io.h"
#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "common/server_define.h"

class session;
class session_manager;

// What every session needs to know about the server it belongs to. Owned by `server`.
struct server_context
{
    lpn::sid server_sid;                            // this server's own sid (its type is the tunnel it serves)
    std::chrono::milliseconds session_timeout;      // drop a connection after this long without any packet
    lpn::message_dispatcher<session> lobby;         // handlers for the LOBBY tunnel
};

// One TCP connection. Every member is touched only on strand().
// The server plays gateway and backend in one process, so the session also keeps the
// per-connection tunnel table a separate gateway would keep.
class session : public std::enable_shared_from_this<session>
{
public:
    using strand_type = asio::strand<asio::io_context::executor_type>;
    using socket_type = asio::basic_stream_socket<asio::ip::tcp, strand_type>;

    static constexpr std::size_t MAX_SEND_QUEUE = 256;

    session(socket_type socket, session_manager& manager, const server_context& ctx);

    // Spawns the receive loop and the timeout watchdog. Call once after the manager registered it.
    void start();

    // Strand only. Queues an encoded packet; disconnects on queue overflow.
    void send(lpn::shared_buffer buf);
    // Strand only. Encodes and queues one frame.
    void send_frame(const lpn::frame& f);
    // Strand only. Sends msg as DATA on an open tunnel. Returns false if the tunnel is closed.
    bool send_message(lpn::tunnel t, const google::protobuf::Message& msg);
    // Strand only. Idempotent.
    void close(std::string_view reason);

    strand_type strand() { return socket_.get_executor(); }
    std::uint32_t id() const noexcept { return id_; }
    bool is_open() const noexcept { return open_; }

    // Tunnel table: tunnel id -> bound server sid, 0 = closed.
    bool tunnel_open(std::uint8_t tunnel_id) const noexcept;
    std::uint64_t tunnel_sid(std::uint8_t tunnel_id) const noexcept;

    bool logged_in() const noexcept { return player_id_ != INVALID_ID; }
    std::int32_t player_id() const noexcept { return player_id_; }
    const std::string& name() const noexcept { return name_; }
    void set_player(std::int32_t player_id, std::string name);

    session_manager& manager() noexcept { return manager_; }

private:
    asio::awaitable<void> run();        // receive loop: read_frame -> on_frame until closed
    asio::awaitable<void> write_loop(); // drains send_queue_ with one async_write at a time
    asio::awaitable<void> watchdog();   // closes the session after session_timeout without input

    void on_frame(const lpn::frame& f);
    void on_heartbeat(const lpn::frame& f);
    void on_connect(std::uint8_t tunnel_id, std::uint64_t requested_sid);
    void on_disconnect(std::uint8_t tunnel_id, std::uint64_t server_sid);
    void on_data(const lpn::frame& f);

    socket_type socket_;               // its executor is this session's strand
    session_manager& manager_;         // registry; only touched through its thread-safe methods
    const server_context& ctx_;        // shared, read-only server settings and handler table
    std::uint32_t id_;                 // connection number, for logs
    std::int32_t player_id_ = INVALID_ID; // INVALID_ID until login_req succeeds
    std::string name_;                 // player name from login_req, UTF-8
    bool open_ = true;                 // cleared by close(); every loop checks it
    bool writing_ = false;             // a write_loop coroutine is draining send_queue_
    std::array<std::uint64_t, lpn::TUNNEL_COUNT> tunnels_{}; // tunnel id -> bound sid, 0 = closed
    std::chrono::steady_clock::time_point last_recv_;        // refreshed by every received frame
    asio::steady_timer watchdog_timer_;                      // drives watchdog(); cancelled by close()
    std::deque<lpn::shared_buffer> send_queue_;              // encoded packets waiting for write_loop
};
