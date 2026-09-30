#include "NetworkClient/client_session.h"

#include <iostream>
#include <system_error>

#include "NetworkClient/client_log.h"
#include "common/error_text.h"
#include "common/lpn/frame_io.h"
#include "common/lpn/msgid.h"
#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "common/utf8.h"
#include "error_code.pb.h"

namespace
{
constexpr auto LOBBY = lpn::tunnel::lobby;
constexpr auto LOBBY_ID = static_cast<std::uint8_t>(lpn::tunnel::lobby);
} // namespace

// The client keeps its own msgid dispatcher for the messages the server sends on the LOBBY
// tunnel; handlers are member functions and the msgid comes from the parameter type.
client_session::client_session(asio::io_context& io, std::string name)
    : io_(io), socket_(io), heartbeat_timer_(io), name_(std::move(name))
{
    lobby_.regist(&client_session::on_login_res);
    lobby_.regist(&client_session::on_chat_res);
    lobby_.regist(&client_session::on_chat_noti);
}

// Launches the connection coroutine. Everything after this runs on the io_context thread; the
// stdin thread in main() reaches the session only through asio::post (see send_chat).
void client_session::start(std::string host, std::uint16_t port)
{
    asio::co_spawn(io_, [self = shared_from_this(), host = std::move(host), port] {
        return self->run(host, port);
    }, asio::detached);
}

// One console line -> one chat_req. Called on the io_context thread via asio::post from main().
void client_session::send_chat(std::string_view text)
{
    if (!socket_.is_open())
        return;
    if (player_id_ == INVALID_ID) {
        client_log.warn("not logged in yet");
        return;
    }
    // main() already converts console input to UTF-8 (common/console.h). This catches input
    // piped in with another encoding before protobuf and the server complain about it.
    if (!utf8::is_valid(text)) {
        client_log.warn("input is not valid UTF-8, not sent");
        return;
    }
    chat::chat_req req;
    req.set_text(std::string(text));
    send_message(req);
}

// Synchronous write: the client sends little and only from the io thread, so a blocking write is
// simpler than a send queue and never interleaves with another write.
void client_session::send_message(const google::protobuf::Message& m)
{
    try {
        lpn::write_frame(socket_, lpn::make_tunnel(LOBBY, lpn::packet_type::data, lobby_sid_, lpn::encode_message(m)));
    } catch (const std::system_error& e) {
        client_log.error("send failed: ", netsys::describe(e.code()));
        close();
    }
}

// Idempotent. Cancelling the timer ends heartbeat_loop; closing the socket ends run() with
// operation_aborted, which is not reported as an error.
void client_session::close()
{
    heartbeat_timer_.cancel();
    if (!socket_.is_open())
        return;
    std::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

// Connect, open the LOBBY tunnel, then read frames until the connection ends. Login happens in
// on_frame once the server's CONNECT reply tells us which sid to use.
asio::awaitable<void> client_session::run(std::string host, std::uint16_t port)
{
    try {
        asio::ip::tcp::resolver resolver(io_);
        const auto endpoints = co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);
        co_await asio::async_connect(socket_, endpoints, asio::use_awaitable);
        socket_.set_option(asio::ip::tcp::no_delay(true));

        asio::co_spawn(io_, [self = shared_from_this()] { return self->heartbeat_loop(); }, asio::detached);

        // Open the LOBBY tunnel. id 0 = anycast: the server answers with its real sid.
        const lpn::sid anycast{0, 0, LOBBY_ID, 0};
        lpn::write_frame(socket_, lpn::make_tunnel(LOBBY, lpn::packet_type::connect, anycast.value()));

        for (;;) {
            const lpn::frame f = co_await lpn::read_frame(socket_);
            on_frame(f);
        }
    } catch (const std::system_error& e) {
        if (e.code() != asio::error::operation_aborted)
            client_log.error("disconnected: ", netsys::describe(e.code()));
    } catch (const std::exception& e) {
        client_log.error("error: ", e.what());
    }
    close();
}

// NOOPREQ every HEARTBEAT_INTERVAL (3 s). The server drops a connection after SESSION_TIMEOUT
// (5 s) without any packet, so one lost heartbeat is tolerated, two are not.
asio::awaitable<void> client_session::heartbeat_loop()
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

// Tunnel-level handling of one received frame. DATA goes to the msgid dispatcher; the other
// tunnel packets change the tunnel state (bound sid) or end the session.
void client_session::on_frame(const lpn::frame& f)
{
    if (!f.is_tunnel())
        return; // heartbeat responses and unknown types need no action
    if (f.tunnel_id() != LOBBY_ID)
        return;

    switch (f.type) {
    // The server accepted the tunnel and told us its real sid: from now on DATA carries it.
    // This is also the moment to log in.
    case lpn::packet_type::connect: {
        lobby_sid_ = f.server_sid;
        client_log.info("LOBBY tunnel open [server:", lpn::sid::from_value(lobby_sid_).to_string(), "]");
        chat::login_req req;
        req.set_name(name_);
        send_message(req);
        break;
    }
    case lpn::packet_type::data: {
        std::uint32_t msgid = 0;
        const auto r = lobby_.dispatch(*this, f.payload, &msgid);
        if (r != lpn::dispatch_result::ok && !f.payload.empty())
            client_log.warn("dropped message: ", lpn::to_string(r), " msgid=", msgid);
        break;
    }
    // The server moved us to another instance: only the sid changes, the tunnel stays open.
    case lpn::packet_type::shift:
        lobby_sid_ = f.server_sid;
        client_log.info("LOBBY tunnel moved [server:", lpn::sid::from_value(lobby_sid_).to_string(), "]");
        break;
    case lpn::packet_type::disconnect:
        client_log.info("LOBBY tunnel closed by server");
        close();
        break;
    case lpn::packet_type::failed:
        client_log.error("LOBBY tunnel failed [server:", lpn::sid::from_value(f.server_sid).to_string(), "]");
        close();
        break;
    default:
        break;
    }
}

void client_session::on_login_res(const chat::login_res& res)
{
    if (res.error_code_() != nserror::SUCCESS) {
        client_log.error("LOGIN FAILED [", nserror::error_code_Name(static_cast<nserror::error_code>(res.error_code_())),
                         "]");
        close();
        return;
    }
    player_id_ = res.player_id();
    client_log.info("LOGIN OK [id:", player_id_, "][name:", name_, "]");
}

void client_session::on_chat_res(const chat::chat_res& res)
{
    if (res.error_code_() != nserror::SUCCESS)
        client_log.warn("CHAT FAILED [", nserror::error_code_Name(static_cast<nserror::error_code>(res.error_code_())),
                        "]");
}

void client_session::on_chat_noti(const chat::chat_noti& noti)
{
    // Chat is the program's output, not a log line: plain stdout.
    std::cout << "CHAT from [" << noti.name() << "]: " << noti.text() << std::endl;
}
