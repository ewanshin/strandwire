// End-to-end test: an in-process server, real TCP sockets, the LPN protocol.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <asio.hpp>

#include "ServerLib/server.h"
#include "ServerLib/server_log.h"
#include "chat.pb.h"
#include "common/lpn/frame.h"
#include "common/lpn/frame_io.h"
#include "common/lpn/msgid.h"
#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "error_code.pb.h"
#include "tests/test_support.h"

#define CHECK(cond)                                                                \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n"; \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

using namespace std::chrono_literals;

namespace
{

constexpr auto LOBBY = lpn::tunnel::lobby;
constexpr lpn::sid LOBBY_ANYCAST{0, 0, 11, 0};
constexpr lpn::sid SERVER_SID{0, 0, 11, 1};

struct test_client
{
    asio::ip::tcp::socket socket;
    std::uint64_t lobby_sid = 0;
    std::int32_t player_id = -1;

    explicit test_client(asio::io_context& io) : socket(io) {}

    asio::awaitable<void> connect(std::uint16_t port)
    {
        co_await socket.async_connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
    }

    asio::awaitable<void> send(const lpn::frame& f) { co_await lpn::async_write_frame(socket, f); }
    asio::awaitable<lpn::frame> read() { co_return co_await lpn::read_frame(socket); }

    // CONNECT (anycast) -> CONNECT carrying the server's real sid.
    asio::awaitable<void> open_lobby()
    {
        co_await send(lpn::make_tunnel(LOBBY, lpn::packet_type::connect, LOBBY_ANYCAST.value()));
        const lpn::frame f = co_await read();
        CHECK(f.is_tunnel());
        CHECK(f.tunnel_id() == 11 && f.type == lpn::packet_type::connect);
        CHECK(lpn::sid::from_value(f.server_sid) == SERVER_SID);
        lobby_sid = f.server_sid;
    }

    asio::awaitable<void> send_msg(const google::protobuf::Message& m)
    {
        co_await send(lpn::make_tunnel(LOBBY, lpn::packet_type::data, lobby_sid, lpn::encode_message(m)));
    }

    // Reads one frame and requires it to be DATA on LOBBY carrying message M.
    template <class M>
    asio::awaitable<M> expect()
    {
        const lpn::frame f = co_await read();
        CHECK(f.is_tunnel());
        CHECK(f.tunnel_id() == 11 && f.type == lpn::packet_type::data);
        CHECK(f.server_sid == lobby_sid);
        CHECK(f.payload.size() >= lpn::MSGID_SIZE);
        CHECK(lpn::get_u32(f.payload.data()) == lpn::msgid_of<M>());
        M m;
        CHECK(m.ParseFromArray(f.payload.data() + lpn::MSGID_SIZE,
                               static_cast<int>(f.payload.size() - lpn::MSGID_SIZE)));
        co_return m;
    }

    asio::awaitable<void> login(const std::string& name)
    {
        chat::login_req req;
        req.set_name(name);
        co_await send_msg(req);
        const auto res = co_await expect<chat::login_res>();
        CHECK(res.error_code_() == nserror::SUCCESS);
        CHECK(res.has_player_id());
        player_id = res.player_id();
    }
};

bool wait_for_count(server& srv, std::size_t expected, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (srv.session_count() == expected)
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return srv.session_count() == expected;
}

asio::awaitable<void> sleep_for(asio::io_context& io, std::chrono::milliseconds d)
{
    asio::steady_timer t(io);
    t.expires_after(d);
    co_await t.async_wait(asio::use_awaitable);
}

// Login, chat broadcast, heartbeat, tunnel errors, unknown message, disconnect bookkeeping.
void test_main_flow()
{
    server_options opt;
    opt.port = 0;
    opt.threads = 2;
    server srv(opt);
    srv.init_instance();
    srv.start();
    const std::uint16_t port = srv.port();
    CHECK(port != 0);

    asio::io_context io;
    test_client a(io);
    test_client b(io);
    bool done = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await a.connect(port);
        co_await b.connect(port);
        CHECK(wait_for_count(srv, 2, 5s));

        // chat before the tunnel is open -> FAILED, connection stays up
        chat::chat_req early;
        early.set_text("too early");
        co_await a.send(lpn::make_tunnel(LOBBY, lpn::packet_type::data, SERVER_SID.value(), lpn::encode_message(early)));
        {
            const lpn::frame f = co_await a.read();
            CHECK(f.type == lpn::packet_type::failed && f.tunnel_id() == 11);
        }

        // a tunnel this server does not serve -> FAILED
        const lpn::sid region_anycast{0, 0, 13, 0};
        co_await a.send(lpn::make_tunnel(lpn::tunnel::region, lpn::packet_type::connect, region_anycast.value()));
        {
            const lpn::frame f = co_await a.read();
            CHECK(f.type == lpn::packet_type::failed && f.tunnel_id() == 13);
            CHECK(f.server_sid == region_anycast.value());
        }

        co_await a.open_lobby();
        co_await b.open_lobby();

        // chat before login -> chat_res with an error code, no broadcast
        co_await a.send_msg(early);
        {
            const auto res = co_await a.expect<chat::chat_res>();
            CHECK(res.error_code_() == nserror::NOT_LOGGED_IN);
        }

        // empty name is rejected
        {
            chat::login_req bad;
            bad.set_name("");
            co_await a.send_msg(bad);
            const auto res = co_await a.expect<chat::login_res>();
            CHECK(res.error_code_() == nserror::INVALID_NAME);
        }

        co_await a.login("alice");
        co_await b.login("bob");
        CHECK(a.player_id != b.player_id);

        // second login on the same session
        {
            chat::login_req again;
            again.set_name("alice2");
            co_await a.send_msg(again);
            const auto res = co_await a.expect<chat::login_res>();
            CHECK(res.error_code_() == nserror::ALREADY_LOGGED_IN);
        }

        // chat: sender gets chat_res then chat_noti, the other client gets chat_noti
        chat::chat_req req;
        req.set_text("hello from alice");
        co_await a.send_msg(req);
        {
            const auto res = co_await a.expect<chat::chat_res>();
            CHECK(res.error_code_() == nserror::SUCCESS);
            const auto na = co_await a.expect<chat::chat_noti>();
            const auto nb = co_await b.expect<chat::chat_noti>();
            CHECK(na.name() == "alice" && na.text() == "hello from alice" && na.player_id() == a.player_id);
            CHECK(nb.name() == "alice" && nb.text() == "hello from alice" && nb.player_id() == a.player_id);
        }

        // Korean text in UTF-8 goes through unchanged
        {
            const std::string korean = "\xEC\x95\x88\xEB\x85\x95"; // "안녕"
            chat::chat_req kr;
            kr.set_text(korean);
            co_await a.send_msg(kr);
            const auto res = co_await a.expect<chat::chat_res>();
            CHECK(res.error_code_() == nserror::SUCCESS);
            const auto na = co_await a.expect<chat::chat_noti>();
            const auto nb = co_await b.expect<chat::chat_noti>();
            CHECK(na.text() == korean && nb.text() == korean);
        }

        // Text that is not UTF-8 (CP949 bytes from a Korean console) is refused and not broadcast.
        // protobuf logs an error when serializing it here; that is expected.
        {
            chat::chat_req cp949;
            cp949.set_text("\xBE\xC8\xB3\xE7"); // "안녕" in CP949
            co_await a.send_msg(cp949);
            const auto res = co_await a.expect<chat::chat_res>();
            CHECK(res.error_code_() == nserror::INVALID_TEXT);
            // b must not have received a chat_noti: its next frame is the heartbeat reply below.
            co_await b.send(lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
            const lpn::frame f = co_await b.read();
            CHECK(f.type == lpn::packet_type::heartbeat);
        }

        // heartbeat
        co_await a.send(lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
        {
            const lpn::frame f = co_await a.read();
            CHECK(f.type == lpn::packet_type::heartbeat);
            CHECK(f.param == static_cast<std::uint8_t>(lpn::heartbeat_command::noop_res));
        }
        co_await a.send(lpn::make_heartbeat(lpn::heartbeat_command::ping_req));
        {
            const lpn::frame f = co_await a.read();
            CHECK(f.param == static_cast<std::uint8_t>(lpn::heartbeat_command::ping_res));
        }

        // unknown msgid and unknown packet types are dropped; the connection survives
        {
            std::vector<char> unknown(lpn::MSGID_SIZE);
            lpn::put_u32(unknown.data(), lpn::fnv1a32("chat.no_such_message"));
            co_await a.send(lpn::make_tunnel(LOBBY, lpn::packet_type::data, a.lobby_sid, unknown));

            lpn::frame unknown_type;
            unknown_type.type = static_cast<lpn::packet_type>(0x7F);
            co_await a.send(unknown_type);

            co_await a.send(lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
            const lpn::frame f = co_await a.read();
            CHECK(f.type == lpn::packet_type::heartbeat);
        }

        // DISCONNECT closes the tunnel; data afterwards -> FAILED
        co_await b.send(lpn::make_tunnel(LOBBY, lpn::packet_type::disconnect, b.lobby_sid));
        {
            const lpn::frame f = co_await b.read();
            CHECK(f.type == lpn::packet_type::disconnect && f.server_sid == b.lobby_sid);
        }
        co_await b.send_msg(req);
        {
            const lpn::frame f = co_await b.read();
            CHECK(f.type == lpn::packet_type::failed);
        }

        // a malformed frame (tunnel packet too short to hold the server sid) gets the connection dropped
        {
            const char bad[] = {0x00, 0x00, 0x00, 0x04, 0x01, 0x0B, 0x00, 0x00};
            co_await asio::async_write(b.socket, asio::buffer(bad), asio::use_awaitable);
            CHECK(wait_for_count(srv, 1, 5s));
        }

        a.socket.close();
        CHECK(wait_for_count(srv, 0, 5s));
        done = true;
    }, asio::detached);

    io.run_for(20s);
    CHECK(done);

    srv.stop();
    srv.wait();
    CHECK(srv.session_count() == 0);
}

// Regression: the server closes a session while one of its writes is still in flight.
// Each round makes the server answer (FAILED for data on a closed tunnel) and, without reading
// that answer, immediately sends a malformed frame so that close() races the write completion.
// Before the fix write_loop() popped from a queue that close() had already cleared.
void test_close_during_write()
{
    server_options opt;
    opt.port = 0;
    opt.threads = 2;
    server srv(opt);
    srv.init_instance();
    srv.start();
    const std::uint16_t port = srv.port();

    asio::io_context io;
    bool done = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        chat::chat_req req;
        req.set_text("x");
        const std::vector<char> data = lpn::encode(
            lpn::make_tunnel(LOBBY, lpn::packet_type::data, SERVER_SID.value(), lpn::encode_message(req)));
        const char bad[] = {0x00, 0x00, 0x00, 0x04, 0x01, 0x0B, 0x00, 0x00}; // CONNECT without a full sid

        for (int round = 0; round < 100; ++round) {
            test_client c(io);
            co_await c.connect(port);

            // Several answers queued, then the malformed frame, all in one write.
            std::vector<char> burst;
            for (int i = 0; i < 4; ++i)
                burst.insert(burst.end(), data.begin(), data.end());
            burst.insert(burst.end(), std::begin(bad), std::end(bad));
            co_await asio::async_write(c.socket, asio::buffer(burst), asio::use_awaitable);

            // Drain until the server drops us.
            try {
                for (;;)
                    co_await c.read();
            } catch (const std::system_error&) {
            }
        }
        CHECK(wait_for_count(srv, 0, 5s));
        done = true;
    }, asio::detached);

    io.run_for(60s);
    CHECK(done);

    srv.stop();
    srv.wait();
}

// An idle connection is dropped after session_timeout; heartbeats keep it alive.
void test_timeout()
{
    server_options opt;
    opt.port = 0;
    opt.threads = 1;
    opt.session_timeout = 300ms;
    server srv(opt);
    srv.init_instance();
    srv.start();
    const std::uint16_t port = srv.port();

    asio::io_context io;
    test_client c(io);
    bool done = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await c.connect(port);
        CHECK(wait_for_count(srv, 1, 5s));

        // Heartbeat every 100 ms for ~700 ms: more than twice the timeout, still connected.
        for (int i = 0; i < 7; ++i) {
            co_await c.send(lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
            const lpn::frame f = co_await c.read();
            CHECK(f.type == lpn::packet_type::heartbeat);
            co_await sleep_for(io, 100ms);
        }
        CHECK(srv.session_count() == 1);

        // Go silent: the server must close the connection.
        bool closed = false;
        try {
            co_await c.read();
        } catch (const std::system_error&) {
            closed = true;
        }
        CHECK(closed);
        CHECK(wait_for_count(srv, 0, 5s));
        done = true;
    }, asio::detached);

    io.run_for(20s);
    CHECK(done);

    srv.stop();
    srv.wait();
}

// Between init_instance() and start() the port is held: a client can connect and send, but the
// server accepts nothing and reads nothing. start() then accepts the queued connection and
// handles the bytes that waited in the kernel.
void test_listen_before_start()
{
    server_options opt;
    opt.port = 0;
    opt.threads = 1;
    server srv(opt);
    srv.init_instance(); // no start() yet
    const std::uint16_t port = srv.port();
    CHECK(port != 0);

    asio::io_context io;
    test_client c(io);
    bool done = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await c.connect(port); // the OS completes the handshake from the backlog
        co_await c.send(lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
        co_await sleep_for(io, 300ms);
        CHECK(srv.session_count() == 0); // not accepted: no session exists

        srv.start();
        const lpn::frame f = co_await c.read(); // the queued connection is accepted, the heartbeat answered
        CHECK(f.type == lpn::packet_type::heartbeat);
        CHECK(f.param == static_cast<std::uint8_t>(lpn::heartbeat_command::noop_res));
        CHECK(wait_for_count(srv, 1, 5s));
        done = true;
    }, asio::detached);

    io.run_for(20s);
    CHECK(done);

    srv.stop();
    srv.wait();
}

} // namespace

int main()
{
    test_support::init();

    // Server log on the console only, so a failing run shows what the server saw.
    nslog::configuration log_conf;
    log_conf.module_name = "smoke_test";
    server_log.start(log_conf);

    test_main_flow();
    test_close_during_write();
    test_timeout();
    test_listen_before_start();

    server_log.stop();
    std::cout << "smoke_test: OK\n";
    return 0;
}
