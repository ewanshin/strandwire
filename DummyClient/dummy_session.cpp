#include "DummyClient/dummy_session.h"

#include <chrono>
#include <random>
#include <system_error>

#include "DummyClient/client_log.h"
#include "common/error_text.h"
#include "common/lpn/frame_io.h"
#include "common/lpn/msgid.h"
#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "error_code.pb.h"

using namespace std::chrono_literals;

namespace
{

constexpr auto LOBBY = lpn::tunnel::lobby;
constexpr auto LOBBY_ID = static_cast<std::uint8_t>(lpn::tunnel::lobby);

// 0..999 ms before the first chat line, so N sessions started together do not all chat in the
// same millisecond every second. thread_local because the generator is not thread-safe.
std::chrono::milliseconds random_delay()
{
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 999);
    return std::chrono::milliseconds(dist(rng));
}

} // namespace

// index becomes the player name "UserName<index>"; main() numbers sessions from 1001.
dummy_session::dummy_session(asio::io_context& io, int index)
    : io_(io),
      socket_(io),
      chat_timer_(io),
      heartbeat_timer_(io),
      index_(index),
      name_("UserName" + std::to_string(index))
{
    lobby_.regist(&dummy_session::on_login_res);
    lobby_.regist(&dummy_session::on_chat_res);
    lobby_.regist(&dummy_session::on_chat_noti);
}

void dummy_session::start(asio::ip::tcp::endpoint target)
{
    asio::co_spawn(io_, [self = shared_from_this(), target] { return self->run(target); }, asio::detached);
}

// Same flow as NetworkClient: connect -> heartbeat -> CONNECT(LOBBY, anycast) -> read frames.
// All N sessions share one io_context thread, so nothing here needs synchronisation.
asio::awaitable<void> dummy_session::run(asio::ip::tcp::endpoint target)
{
    try {
        co_await socket_.async_connect(target, asio::use_awaitable);
        asio::co_spawn(io_, [self = shared_from_this()] { return self->heartbeat_loop(); }, asio::detached);

        const lpn::sid anycast{0, 0, LOBBY_ID, 0};
        lpn::write_frame(socket_, lpn::make_tunnel(LOBBY, lpn::packet_type::connect, anycast.value()));

        for (;;) {
            const lpn::frame f = co_await lpn::read_frame(socket_);
            on_frame(f);
        }
    } catch (const std::system_error& e) {
        if (e.code() != asio::error::operation_aborted)
            client_log.error("[", name_, "] disconnected: ", netsys::describe(e.code()));
    } catch (const std::exception& e) {
        client_log.error("[", name_, "] error: ", e.what());
    }
    close();
}

// The load itself: one chat_req per second per session, started after login succeeds. With N
// sessions the server broadcasts N*N chat_noti per second (see chats_received in the statistics).
asio::awaitable<void> dummy_session::chat_loop()
{
    try {
        chat_timer_.expires_after(random_delay());
        co_await chat_timer_.async_wait(asio::use_awaitable);
        while (socket_.is_open()) {
            chat::chat_req req;
            req.set_text("TEST CHAT: HELLO~~ This is Player " + std::to_string(index_));
            send_message(req);
            chat_timer_.expires_after(1s);
            co_await chat_timer_.async_wait(asio::use_awaitable);
        }
    } catch (const std::system_error&) {
        // timer cancelled or socket closed: stop chatting
    }
}

asio::awaitable<void> dummy_session::heartbeat_loop()
{
    try {
        while (socket_.is_open()) {
            heartbeat_timer_.expires_after(lpn::HEARTBEAT_INTERVAL);
            co_await heartbeat_timer_.async_wait(asio::use_awaitable);
            if (socket_.is_open())
                lpn::write_frame(socket_, lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
        }
    } catch (const std::system_error&) {
        // timer cancelled or socket closed
    }
}

// Synchronous write on the single io thread. A write error throws std::system_error, which the
// calling coroutine (run or chat_loop) turns into a close.
void dummy_session::send_message(const google::protobuf::Message& m)
{
    lpn::write_frame(socket_, lpn::make_tunnel(LOBBY, lpn::packet_type::data, lobby_sid_, lpn::encode_message(m)));
}

// Tunnel-level handling; DATA goes to the msgid dispatcher (on_login_res etc.).
void dummy_session::on_frame(const lpn::frame& f)
{
    if (!f.is_tunnel() || f.tunnel_id() != LOBBY_ID)
        return;

    switch (f.type) {
    case lpn::packet_type::connect: {
        lobby_sid_ = f.server_sid;
        chat::login_req req;
        req.set_name(name_);
        send_message(req);
        break;
    }
    case lpn::packet_type::data:
        lobby_.dispatch(*this, f.payload);
        break;
    case lpn::packet_type::shift:
        lobby_sid_ = f.server_sid;
        break;
    case lpn::packet_type::disconnect:
    case lpn::packet_type::failed:
        client_log.error("[", name_, "] tunnel closed by server (type ", static_cast<int>(f.type), ")");
        close();
        break;
    default:
        break;
    }
}

void dummy_session::on_login_res(const chat::login_res& res)
{
    if (res.error_code_() != nserror::SUCCESS) {
        client_log.error("[", name_, "] login failed: ", res.error_code_());
        close();
        return;
    }
    logged_in_ = true;
    client_log.debug("LOGIN OK: ID[", res.player_id(), "] Name[", name_, "]"); // one per session: debug level
    asio::co_spawn(io_, [self = shared_from_this()] { return self->chat_loop(); }, asio::detached);
}

void dummy_session::on_chat_res(const chat::chat_res& res)
{
    if (res.error_code_() != nserror::SUCCESS)
        client_log.warn("[", name_, "] chat failed: ", res.error_code_());
}

// Every broadcast that reaches this session, including its own lines. Summed by main() into the
// chats_received statistic, which is the throughput number of the load test.
void dummy_session::on_chat_noti(const chat::chat_noti&)
{
    ++chats_received_;
}

// Idempotent. Cancelling the timers ends chat_loop and heartbeat_loop; closing the socket ends run().
void dummy_session::close()
{
    std::error_code ec;
    chat_timer_.cancel();
    heartbeat_timer_.cancel();
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}
