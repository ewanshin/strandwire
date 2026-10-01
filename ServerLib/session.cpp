#include "ServerLib/session.h"

#include <algorithm>
#include <system_error>

#include "ServerLib/server_log.h"
#include "ServerLib/session_manager.h"
#include "common/error_text.h"
#include "common/lpn/msgid.h"

// The socket arrives already bound to its strand (server::accept_loop created it there), so every
// asio completion for this session runs on that strand from the first byte on.
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
    socket_.set_option(asio::ip::tcp::no_delay(true), ec);
    const auto ep = socket_.remote_endpoint(ec); // fails if the peer already went away
    if (!ec)
        server_log.info("[session ", id_, "] connected from ", ep.address().to_string(), ":", ep.port());
}

// Two coroutines per session, both on the strand: the receive loop and the timeout watchdog.
// Each captures `self`, so the session stays alive at least until both have finished.
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
            close(netsys::describe(e.code())); // not e.code().message(): that is localized, non-UTF-8 text
    }
    catch (const std::exception& e)
    {
        close(e.what()); // lpn::protocol_error and anything else
    }
}

// Drops the connection when nothing has been received for ctx_.session_timeout.
// Any packet counts, which is why clients send a NOOP heartbeat every 3 seconds.
asio::awaitable<void> session::watchdog()
{
    // Poll at a quarter of the timeout: a silent connection is dropped at most 25% late, and no
    // timer has to be re-armed on every received packet.
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
    }
}

// First dispatch level: by packet type. Tunnel bookkeeping (CONNECT/DISCONNECT) is handled here;
// DATA goes on to the msgid dispatcher of the tunnel's service.
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
        server_log.warn("[session ", id_, "] ignored packet type ",
                        static_cast<int>(static_cast<std::uint8_t>(f.type)));
        break;
    }
}

// Heartbeats carry no sid and need no tunnel. The reply mirrors the request; run() already
// refreshed last_recv_, which is the heartbeat's real purpose.
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
        break; // responses and unknown opcodes need no reply
    }
}

void session::on_disconnect(std::uint8_t tunnel_id, std::uint64_t server_sid)
{
    // Valid only for the sid the tunnel is bound to, so a stale DISCONNECT cannot close a rebound tunnel.
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
void session::on_connect(std::uint8_t tunnel_id, std::uint64_t requested_sid)
{
    const lpn::sid requested = lpn::sid::from_value(requested_sid);
    const lpn::sid& mine = ctx_.server_sid;

    // This process serves exactly one server type. Accept an anycast request for that type,
    // or a request that names this instance exactly.
    const bool right_tunnel = (tunnel_id == mine.type) && (requested.type == mine.type);
    const bool right_instance = requested.is_anycast() || requested == mine;
    if (!right_tunnel || !right_instance)
    {
        server_log.warn("[session ", id_, "] connect refused: tunnel ", static_cast<int>(tunnel_id), " sid ",
                        requested.to_string());
        send_frame(lpn::make_tunnel(tunnel_id, lpn::packet_type::failed, requested_sid));
        return;
    }

    tunnels_[tunnel_id] = mine.value(); // re-connect simply rebinds
    send_frame(lpn::make_tunnel(tunnel_id, lpn::packet_type::connect, mine.value()));
    server_log.info("[session ", id_, "] tunnel ", static_cast<int>(tunnel_id), " open -> ", mine.to_string());
}

// DATA: the message itself. Routing is by the tunnel table, the msgid picks the handler.
// The decoder already guaranteed tunnel_id() < TUNNEL_COUNT, so indexing tunnels_ is safe.
void session::on_data(const lpn::frame& f)
{
    const std::uint8_t t = f.tunnel_id();
    if (tunnels_[t] == 0)
    {
        // Tell the client instead of dropping silently, so it can reopen the tunnel.
        server_log.warn("[session ", id_, "] data on closed tunnel ", static_cast<int>(t));
        send_frame(lpn::make_tunnel(t, lpn::packet_type::failed, f.server_sid));
        return;
    }
    if (f.server_sid != tunnels_[t])
    {
        // Logged but still routed by the table: the tunnel table is the source of truth.
        server_log.warn("[session ", id_, "] sid mismatch on tunnel ", static_cast<int>(t), ": packet ",
                        lpn::sid::from_value(f.server_sid).to_string(), ", bound ",
                        lpn::sid::from_value(tunnels_[t]).to_string());
    }
    if (f.payload.empty())
        return; // zero-length DATA is a liveness refresh

    // The only tunnel that can be open is the one this server serves: LOBBY.
    std::uint32_t msgid = 0;
    const lpn::dispatch_result r = ctx_.lobby.dispatch(*this, f.payload, &msgid);
    switch (r)
    {
    case lpn::dispatch_result::ok:
        break;
    // A message this server does not know, or one missing a required field, is the client's
    // problem, not a broken stream: drop it and keep the connection.
    case lpn::dispatch_result::unknown_msgid:
    case lpn::dispatch_result::not_initialized:
        server_log.warn("[session ", id_, "] dropped message: ", lpn::to_string(r), " msgid=", msgid, ' ',
                        ctx_.lobby.name_of(msgid));
        break;
    // Bytes that do not even parse mean the stream is corrupt or the peer is not our client.
    case lpn::dispatch_result::too_short:
    case lpn::dispatch_result::parse_error:
        close(std::string("malformed message: ") + lpn::to_string(r));
        break;
    }
}

// Encodes once and hands the bytes to the queue. For a reply to one session; broadcasts encode
// once in the caller and share the buffer (session_manager::broadcast).
void session::send_frame(const lpn::frame& f)
{
    send(lpn::make_shared_buffer(f));
}

// Wraps a protobuf message in a DATA packet on tunnel t, using the sid the tunnel is bound to.
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
void session::send(lpn::shared_buffer buf)
{
    if (!open_)
        return;
    // A client that cannot keep up with what it is sent (a slow reader in a busy broadcast) would
    // otherwise grow the queue without bound. Drop it instead.
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
            const lpn::shared_buffer buf = std::move(send_queue_.front());
            send_queue_.pop_front();
            co_await asio::async_write(socket_, asio::buffer(*buf), asio::use_awaitable);
        }
    }
    catch (const std::system_error& e)
    {
        if (e.code() != asio::error::operation_aborted) // aborted = close() cancelled the write
            close(netsys::describe(e.code()));
    }
    writing_ = false; // the next send() spawns a new loop
}

// Tears the connection down exactly once. Cancelling the socket and the timer makes the pending
// async operations of run(), write_loop() and watchdog() complete with operation_aborted, so the
// three coroutines end and release their `self` references; removing the map entry releases the
// last one. The tunnel table is cleared so a late DATA cannot be routed.
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
