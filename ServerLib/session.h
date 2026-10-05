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
// 모든 세션이 자신이 속한 서버에 대해 알아야 하는 것. `server`가 소유한다.
struct server_context
{
    // this server's own sid (its type is the tunnel it serves)
    // 이 서버 자신의 sid (type이 이 서버가 서비스하는 터널이다)
    lpn::sid server_sid;
    // drop a connection after this long without any packet
    // 이 시간 동안 패킷이 하나도 없으면 연결을 끊는다
    std::chrono::milliseconds session_timeout;
    lpn::message_dispatcher<session> lobby; // handlers for the LOBBY tunnel / LOBBY 터널의 핸들러
};

// One TCP connection. Every member is touched only on strand().
// The server plays gateway and backend in one process, so the session also keeps the
// per-connection tunnel table a separate gateway would keep.
//
// TCP 연결 하나. 모든 멤버는 strand()에서만 만진다.
// 서버가 한 프로세스에서 게이트웨이와 백엔드를 겸하므로, 별도 게이트웨이가 가질 연결별
// 터널 표도 세션이 가진다.
class session : public std::enable_shared_from_this<session>
{
public:
    using strand_type = asio::strand<asio::io_context::executor_type>;
    using socket_type = asio::basic_stream_socket<asio::ip::tcp, strand_type>;

    static constexpr std::size_t MAX_SEND_QUEUE = 256;

    session(socket_type socket, session_manager& manager, const server_context& ctx);

    // Spawns the receive loop and the timeout watchdog. Call once after the manager registered it.
    // 수신 루프와 타임아웃 워치독을 띄운다. 매니저가 등록한 뒤 한 번만 호출한다.
    void start();

    // Strand only. Queues an encoded packet; disconnects on queue overflow.
    // strand 전용. 인코딩된 패킷을 큐에 넣는다. 큐가 넘치면 연결을 끊는다.
    void send(lpn::shared_buffer buf);
    // Strand only. Encodes and queues one frame.
    // strand 전용. 프레임 하나를 인코딩해 큐에 넣는다.
    void send_frame(const lpn::frame& f);
    // Strand only. Sends msg as DATA on an open tunnel. Returns false if the tunnel is closed.
    // strand 전용. 열린 터널로 msg를 DATA로 보낸다. 터널이 닫혀 있으면 false를 돌려준다.
    bool send_message(lpn::tunnel t, const google::protobuf::Message& msg);
    // Strand only. Idempotent.
    // strand 전용. 멱등이다.
    void close(std::string_view reason);

    strand_type strand()
    {
        return socket_.get_executor();
    }
    std::uint32_t id() const noexcept
    {
        return id_;
    }
    bool is_open() const noexcept
    {
        return open_;
    }

    // Tunnel table: tunnel id -> bound server sid, 0 = closed.
    // 터널 표: 터널 id -> 바인딩된 서버 sid, 0 = 닫힘.
    bool tunnel_open(std::uint8_t tunnel_id) const noexcept;
    std::uint64_t tunnel_sid(std::uint8_t tunnel_id) const noexcept;

    bool logged_in() const noexcept
    {
        return player_id_ != INVALID_ID;
    }
    std::int32_t player_id() const noexcept
    {
        return player_id_;
    }
    const std::string& name() const noexcept
    {
        return name_;
    }
    void set_player(std::int32_t player_id, std::string name);

    session_manager& manager() noexcept
    {
        return manager_;
    }

private:
    // receive loop: read_frame -> on_frame until closed
    // 수신 루프: 닫힐 때까지 read_frame -> on_frame
    asio::awaitable<void> run();
    // drains send_queue_ with one async_write at a time
    // send_queue_를 한 번에 async_write 하나씩 비운다
    asio::awaitable<void> write_loop();
    // closes the session after session_timeout without input
    // session_timeout 동안 입력이 없으면 세션을 닫는다
    asio::awaitable<void> watchdog();

    void on_frame(const lpn::frame& f);
    void on_heartbeat(const lpn::frame& f);
    void on_connect(std::uint8_t tunnel_id, std::uint64_t requested_sid);
    void on_disconnect(std::uint8_t tunnel_id, std::uint64_t server_sid);
    void on_data(const lpn::frame& f);

    // its executor is this session's strand
    // executor가 이 세션의 strand이다
    socket_type socket_;
    // registry; only touched through its thread-safe methods
    // 레지스트리. 스레드 안전한 메서드로만 만진다
    session_manager& manager_;
    // shared, read-only server settings and handler table
    // 공유하는 읽기 전용 서버 설정과 핸들러 표
    const server_context& ctx_;
    std::uint32_t id_; // connection number, for logs / 연결 번호, 로그용
    // INVALID_ID until login_req succeeds
    // login_req가 성공할 때까지 INVALID_ID
    std::int32_t player_id_ = INVALID_ID;
    // player name from login_req, UTF-8
    // login_req에서 받은 플레이어 이름, UTF-8
    std::string name_;
    // cleared by close(); every loop checks it
    // close()가 지운다. 모든 루프가 이 값을 확인한다
    bool open_ = true;
    // a write_loop coroutine is draining send_queue_
    // write_loop 코루틴이 send_queue_를 비우는 중이다
    bool writing_ = false;
    // tunnel id -> bound sid, 0 = closed
    // 터널 id -> 바인딩된 sid, 0 = 닫힘
    std::array<std::uint64_t, lpn::TUNNEL_COUNT> tunnels_{};
    // refreshed by every received frame
    // 수신한 프레임마다 갱신한다
    std::chrono::steady_clock::time_point last_recv_;
    // drives watchdog(); cancelled by close()
    // watchdog()을 움직인다. close()가 취소한다
    asio::steady_timer watchdog_timer_;
    // encoded packets waiting for write_loop
    // write_loop를 기다리는 인코딩된 패킷
    std::deque<lpn::shared_buffer> send_queue_;
};
