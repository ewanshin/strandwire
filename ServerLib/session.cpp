#include "ServerLib/session.h"

#include <algorithm>
#include <system_error>

#include "ServerLib/server_log.h"
#include "ServerLib/session_manager.h"
#include "common/error_text.h"
#include "common/lpn/msgid.h"

// The socket arrives already bound to its strand (server::accept_loop created it there), so every
// asio completion for this session runs on that strand from the first byte on.
//
// 소켓은 이미 자기 strand에 묶인 채로 온다 (server::accept_loop가 거기서 만들었다). 그래서 이 세션의
// 모든 asio 완료는 첫 바이트부터 그 strand에서 실행된다.
session::session(socket_type socket, session_manager& manager, const server_context& ctx)
    : socket_(std::move(socket)),
      manager_(manager),
      ctx_(ctx),
      id_(manager.next_session_id()),
      last_recv_(std::chrono::steady_clock::now()),
      watchdog_timer_(socket_.get_executor())
{
    std::error_code ec;
    // Chat packets are small and latency matters more than throughput: disable Nagle.
    // 채팅 패킷은 작고 처리량보다 지연이 중요하다. Nagle을 끈다.
    socket_.set_option(asio::ip::tcp::no_delay(true), ec);
    // fails if the peer already went away
    // 상대가 이미 떠났으면 실패한다
    const auto ep = socket_.remote_endpoint(ec);
    if (!ec)
        server_log.info("[session ", id_, "] connected from ", ep.address().to_string(), ":", ep.port());
}

// Two coroutines per session, both on the strand: the receive loop and the timeout watchdog.
// Each captures `self`, so the session stays alive at least until both have finished.
//
// 세션마다 코루틴 둘, 모두 strand 위에서: 수신 루프와 타임아웃 워치독.
// 각각 `self`를 잡으므로 세션은 적어도 둘 다 끝날 때까지 살아 있다.
void session::start()
{
    asio::co_spawn(
        strand(),
        [self = shared_from_this()]
        {
            return self->run();
        },
        asio::detached);
    asio::co_spawn(
        strand(),
        [self = shared_from_this()]
        {
            return self->watchdog();
        },
        asio::detached);
}

void session::set_player(std::int32_t player_id, std::string name)
{
    player_id_ = player_id;
    name_ = std::move(name);
}

bool session::tunnel_open(std::uint8_t tunnel_id) const noexcept
{
    return tunnel_id < tunnels_.size() && tunnels_[tunnel_id] != 0;
}

std::uint64_t session::tunnel_sid(std::uint8_t tunnel_id) const noexcept
{
    return tunnel_id < tunnels_.size() ? tunnels_[tunnel_id] : 0;
}

// Receive loop: one frame at a time until the connection ends. Every way out converges on close():
// a clean EOF, a socket error, a protocol violation thrown by read_frame/on_frame, or
// operation_aborted when close() itself cancelled the pending read.
//
// 수신 루프: 연결이 끝날 때까지 한 번에 프레임 하나. 모든 출구는 close()로 모인다.
// 깨끗한 EOF, 소켓 오류, read_frame/on_frame이 던진 프로토콜 위반, 또는 close() 자신이
// 대기 중인 read를 취소했을 때의 operation_aborted.
asio::awaitable<void> session::run()
{
    try
    {
        while (open_)
        {
            const lpn::frame f = co_await lpn::read_frame(socket_);
            last_recv_ = std::chrono::steady_clock::now();
            on_frame(f);
        }
    }
    catch (const std::system_error& e)
    {
        if (e.code() == asio::error::eof)
            close("eof");
        else if (e.code() == asio::error::operation_aborted)
            close("aborted");
        else
            // not e.code().message(): that is localized, non-UTF-8 text
            // e.code().message()는 쓰지 않는다. 지역화된, UTF-8이 아닌 텍스트다
            close(netsys::describe(e.code()));
    }
    catch (const std::exception& e)
    {
        close(e.what()); // lpn::protocol_error and anything else / lpn::protocol_error와 그 밖의 모든 것
    }
}

// Drops the connection when nothing has been received for ctx_.session_timeout.
// Any packet counts, which is why clients send a NOOP heartbeat every 3 seconds.
//
// ctx_.session_timeout 동안 아무것도 받지 못하면 연결을 끊는다.
// 어떤 패킷이든 센다. 그래서 클라이언트가 3초마다 NOOP 하트비트를 보낸다.
asio::awaitable<void> session::watchdog()
{
    // Poll at a quarter of the timeout: a silent connection is dropped at most 25% late, and no
    // timer has to be re-armed on every received packet.
    //
    // 타임아웃의 1/4마다 폴링한다. 조용한 연결은 기껏해야 25% 늦게 끊기고, 패킷을 받을 때마다
    // 타이머를 다시 걸 필요가 없다.
    const auto period = std::max(ctx_.session_timeout / 4, std::chrono::milliseconds(20));
    try
    {
        while (open_)
        {
            watchdog_timer_.expires_after(period);
            co_await watchdog_timer_.async_wait(asio::use_awaitable);
            if (open_ && std::chrono::steady_clock::now() - last_recv_ > ctx_.session_timeout)
                close("timeout");
        }
    }
    catch (const std::system_error&)
    {
        // timer cancelled by close()
        // close()가 타이머를 취소했다
    }
}

// First dispatch level: by packet type. Tunnel bookkeeping (CONNECT/DISCONNECT) is handled here;
// DATA goes on to the msgid dispatcher of the tunnel's service.
//
// 첫 번째 디스패치 단계: 패킷 type으로. 터널 관리(CONNECT/DISCONNECT)는 여기서 처리한다.
// DATA는 그 터널 서비스의 msgid 디스패처로 넘어간다.
void session::on_frame(const lpn::frame& f)
{
    switch (f.type)
    {
    case lpn::packet_type::connect:
        on_connect(f.tunnel_id(), f.server_sid);
        break;
    case lpn::packet_type::disconnect:
        on_disconnect(f.tunnel_id(), f.server_sid);
        break;
    case lpn::packet_type::data:
        on_data(f);
        break;
    case lpn::packet_type::heartbeat:
        on_heartbeat(f);
        break;
    default:
        // failed / shift are server-to-client. Unknown packet types are ignored, not fatal:
        // new type_ values are the extension point.
        //
        // failed / shift는 서버에서 클라이언트로 가는 것이다. 모르는 패킷 type은 치명적이지 않고 무시한다.
        // 새 type_ 값이 확장 지점이다.
        server_log.warn("[session ", id_, "] ignored packet type ",
                        static_cast<int>(static_cast<std::uint8_t>(f.type)));
        break;
    }
}

// Heartbeats carry no sid and need no tunnel. The reply mirrors the request; run() already
// refreshed last_recv_, which is the heartbeat's real purpose.
//
// 하트비트는 sid가 없고 터널도 필요 없다. 응답은 요청을 그대로 비춘다. run()이 이미 last_recv_를
// 갱신했고, 그것이 하트비트의 진짜 목적이다.
void session::on_heartbeat(const lpn::frame& f)
{
    switch (static_cast<lpn::heartbeat_command>(f.param))
    {
    case lpn::heartbeat_command::noop_req:
        send_frame(lpn::make_heartbeat(lpn::heartbeat_command::noop_res));
        break;
    case lpn::heartbeat_command::ping_req:
        send_frame(lpn::make_heartbeat(lpn::heartbeat_command::ping_res));
        break;
    default:
        break; // responses and unknown opcodes need no reply / 응답과 모르는 opcode에는 답하지 않는다
    }
}

void session::on_disconnect(std::uint8_t tunnel_id, std::uint64_t server_sid)
{
    // Valid only for the sid the tunnel is bound to, so a stale DISCONNECT cannot close a rebound tunnel.
    // 터널이 바인딩된 sid에만 유효하다. 그래서 오래된 DISCONNECT가 다시 바인딩된 터널을 닫을 수 없다.
    if (tunnels_[tunnel_id] != 0 && tunnels_[tunnel_id] == server_sid)
    {
        tunnels_[tunnel_id] = 0;
        send_frame(lpn::make_tunnel(tunnel_id, lpn::packet_type::disconnect, server_sid));
        server_log.info("[session ", id_, "] tunnel ", static_cast<int>(tunnel_id), " closed");
    }
    else
    {
        server_log.warn("[session ", id_, "] disconnect ignored: tunnel ", static_cast<int>(tunnel_id),
                        " is not bound to ", lpn::sid::from_value(server_sid).to_string());
    }
}

// CONNECT: the client asks to bind a tunnel to a server. The reply carries the sid actually bound
// (this server's own), which the client must use in its DATA packets from then on.
//
// CONNECT: 클라이언트가 터널을 서버에 바인딩해 달라고 요청한다. 응답은 실제로 바인딩된 sid
// (이 서버 자신의 것)를 담는다. 클라이언트는 그때부터 DATA 패킷에 그 sid를 써야 한다.
void session::on_connect(std::uint8_t tunnel_id, std::uint64_t requested_sid)
{
    const lpn::sid requested = lpn::sid::from_value(requested_sid);
    const lpn::sid& mine = ctx_.server_sid;

    // This process serves exactly one server type. Accept an anycast request for that type,
    // or a request that names this instance exactly.
    //
    // 이 프로세스는 정확히 한 서버 type을 서비스한다. 그 type의 anycast 요청, 또는 이 인스턴스를
    // 정확히 지목한 요청을 받아들인다.
    const bool right_tunnel = (tunnel_id == mine.type) && (requested.type == mine.type);
    const bool right_instance = requested.is_anycast() || requested == mine;
    if (!right_tunnel || !right_instance)
    {
        server_log.warn("[session ", id_, "] connect refused: tunnel ", static_cast<int>(tunnel_id), " sid ",
                        requested.to_string());
        send_frame(lpn::make_tunnel(tunnel_id, lpn::packet_type::failed, requested_sid));
        return;
    }

    tunnels_[tunnel_id] = mine.value(); // re-connect simply rebinds / 다시 CONNECT하면 그냥 다시 바인딩한다
    send_frame(lpn::make_tunnel(tunnel_id, lpn::packet_type::connect, mine.value()));
    server_log.info("[session ", id_, "] tunnel ", static_cast<int>(tunnel_id), " open -> ", mine.to_string());
}

// DATA: the message itself. Routing is by the tunnel table, the msgid picks the handler.
// The decoder already guaranteed tunnel_id() < TUNNEL_COUNT, so indexing tunnels_ is safe.
//
// DATA: 메시지 자체. 라우팅은 터널 표로, 핸들러는 msgid로 고른다.
// 디코더가 이미 tunnel_id() < TUNNEL_COUNT를 보장했으므로 tunnels_ 인덱싱은 안전하다.
void session::on_data(const lpn::frame& f)
{
    const std::uint8_t t = f.tunnel_id();
    if (tunnels_[t] == 0)
    {
        // Tell the client instead of dropping silently, so it can reopen the tunnel.
        // 조용히 버리지 않고 클라이언트에 알린다. 그래야 터널을 다시 열 수 있다.
        server_log.warn("[session ", id_, "] data on closed tunnel ", static_cast<int>(t));
        send_frame(lpn::make_tunnel(t, lpn::packet_type::failed, f.server_sid));
        return;
    }
    if (f.server_sid != tunnels_[t])
    {
        // Logged but still routed by the table: the tunnel table is the source of truth.
        // 로그만 남기고 표대로 라우팅한다. 터널 표가 기준이다.
        server_log.warn("[session ", id_, "] sid mismatch on tunnel ", static_cast<int>(t), ": packet ",
                        lpn::sid::from_value(f.server_sid).to_string(), ", bound ",
                        lpn::sid::from_value(tunnels_[t]).to_string());
    }
    if (f.payload.empty())
        return; // zero-length DATA is a liveness refresh / 길이 0인 DATA는 생존 갱신이다

    // The only tunnel that can be open is the one this server serves: LOBBY.
    // 열릴 수 있는 터널은 이 서버가 서비스하는 것뿐이다: LOBBY.
    std::uint32_t msgid = 0;
    const lpn::dispatch_result r = ctx_.lobby.dispatch(*this, f.payload, &msgid);
    switch (r)
    {
    case lpn::dispatch_result::ok:
        break;
    // A message this server does not know, or one missing a required field, is the client's
    // problem, not a broken stream: drop it and keep the connection.
    //
    // 이 서버가 모르는 메시지나 required 필드가 빠진 메시지는 클라이언트의 문제이지 스트림이
    // 깨진 것이 아니다. 버리고 연결은 유지한다.
    case lpn::dispatch_result::unknown_msgid:
    case lpn::dispatch_result::not_initialized:
        server_log.warn("[session ", id_, "] dropped message: ", lpn::to_string(r), " msgid=", msgid, ' ',
                        ctx_.lobby.name_of(msgid));
        break;
    // Bytes that do not even parse mean the stream is corrupt or the peer is not our client.
    // 파싱조차 안 되는 바이트는 스트림이 손상됐거나 상대가 우리 클라이언트가 아니라는 뜻이다.
    case lpn::dispatch_result::too_short:
    case lpn::dispatch_result::parse_error:
        close(std::string("malformed message: ") + lpn::to_string(r));
        break;
    }
}

// Encodes once and hands the bytes to the queue. For a reply to one session; broadcasts encode
// once in the caller and share the buffer (session_manager::broadcast).
//
// 한 번 인코딩해서 바이트를 큐에 넘긴다. 한 세션에 대한 응답용이다. 브로드캐스트는 호출자가
// 한 번 인코딩해서 버퍼를 공유한다 (session_manager::broadcast).
void session::send_frame(const lpn::frame& f)
{
    send(lpn::make_shared_buffer(f));
}

// Wraps a protobuf message in a DATA packet on tunnel t, using the sid the tunnel is bound to.
// protobuf 메시지를 터널 t의 DATA 패킷으로 감싼다. 터널에 바인딩된 sid를 쓴다.
bool session::send_message(lpn::tunnel t, const google::protobuf::Message& msg)
{
    const auto tunnel_id = static_cast<std::uint8_t>(t);
    if (!tunnel_open(tunnel_id))
        return false;
    send_frame(lpn::make_tunnel(tunnel_id, lpn::packet_type::data, tunnels_[tunnel_id], lpn::encode_message(msg)));
    return true;
}

// Queue a packet for sending. Writes are serialised through the queue because asio allows only
// one async_write in flight per socket. A single write_loop coroutine drains the queue; it is
// spawned on demand and ends when the queue is empty.
//
// 패킷을 송신 큐에 넣는다. asio는 소켓당 async_write를 하나만 허용하므로 쓰기는 큐로 직렬화한다.
// write_loop 코루틴 하나가 큐를 비운다. 필요할 때 띄우고 큐가 비면 끝난다.
void session::send(lpn::shared_buffer buf)
{
    if (!open_)
        return;
    // A client that cannot keep up with what it is sent (a slow reader in a busy broadcast) would
    // otherwise grow the queue without bound. Drop it instead.
    //
    // 받는 속도를 따라오지 못하는 클라이언트(바쁜 브로드캐스트의 느린 수신자)는 그냥 두면 큐를
    // 한없이 키운다. 대신 끊는다.
    if (send_queue_.size() >= MAX_SEND_QUEUE)
    {
        close("send queue overflow");
        return;
    }
    send_queue_.push_back(std::move(buf));
    if (!writing_)
    {
        writing_ = true;
        asio::co_spawn(
            strand(),
            [self = shared_from_this()]
            {
                return self->write_loop();
            },
            asio::detached);
    }
}

asio::awaitable<void> session::write_loop()
{
    try
    {
        while (open_ && !send_queue_.empty())
        {
            // Take the buffer out of the queue BEFORE suspending. close() may run while this write
            // is in flight and clears the queue; popping after the await would then hit an empty
            // deque (undefined behaviour: it crashed about one run in five).
            //
            // 중단하기 전에 버퍼를 큐에서 꺼낸다. 이 쓰기가 진행 중일 때 close()가 실행되어 큐를
            // 비울 수 있다. await 뒤에 pop하면 빈 deque를 건드린다 (정의되지 않은 동작: 다섯 번에
            // 한 번꼴로 크래시했다).
            const lpn::shared_buffer buf = std::move(send_queue_.front());
            send_queue_.pop_front();
            co_await asio::async_write(socket_, asio::buffer(*buf), asio::use_awaitable);
        }
    }
    catch (const std::system_error& e)
    {
        // aborted = close() cancelled the write
        // aborted = close()가 쓰기를 취소했다
        if (e.code() != asio::error::operation_aborted)
            close(netsys::describe(e.code()));
    }
    writing_ = false; // the next send() spawns a new loop / 다음 send()가 새 루프를 띄운다
}

// Tears the connection down exactly once. Cancelling the socket and the timer makes the pending
// async operations of run(), write_loop() and watchdog() complete with operation_aborted, so the
// three coroutines end and release their `self` references; removing the map entry releases the
// last one. The tunnel table is cleared so a late DATA cannot be routed.
//
// 연결을 정확히 한 번 내린다. 소켓과 타이머를 취소하면 run(), write_loop(), watchdog()의 대기 중인
// 비동기 작업이 operation_aborted로 완료되고, 세 코루틴이 끝나며 `self` 참조를 놓는다. 맵 항목을
// 지우면 마지막 참조가 풀린다. 늦게 온 DATA가 라우팅되지 않도록 터널 표를 비운다.
void session::close(std::string_view reason)
{
    if (!open_)
        return;
    open_ = false;

    std::error_code ec;
    watchdog_timer_.cancel();
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    send_queue_.clear();
    tunnels_.fill(0);
    manager_.remove(id_);
    server_log.info("[session ", id_, "] closed: ", reason);
}
