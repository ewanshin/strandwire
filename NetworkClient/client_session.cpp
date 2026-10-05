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
//
// 클라이언트는 서버가 LOBBY 터널로 보내는 메시지를 위해 자체 msgid 디스패처를 둔다.
// 핸들러는 멤버 함수이고 msgid는 매개변수 타입에서 나온다.
client_session::client_session(asio::io_context& io, std::string name)
    : io_(io),
      socket_(io),
      heartbeat_timer_(io),
      name_(std::move(name))
{
    lobby_.regist(&client_session::on_login_res);
    lobby_.regist(&client_session::on_chat_res);
    lobby_.regist(&client_session::on_chat_noti);
}

// Launches the connection coroutine. Everything after this runs on the io_context thread; the
// stdin thread in main() reaches the session only through asio::post (see send_chat).
//
// 연결 코루틴을 띄운다. 이후의 모든 것은 io_context 스레드에서 실행된다. main()의 stdin 스레드는
// asio::post를 통해서만 세션에 닿는다 (send_chat 참고).
void client_session::start(std::string host, std::uint16_t port)
{
    asio::co_spawn(
        io_,
        [self = shared_from_this(), host = std::move(host), port]
        {
            return self->run(host, port);
        },
        asio::detached);
}

// One console line -> one chat_req. Called on the io_context thread via asio::post from main().
// 콘솔 한 줄 -> chat_req 하나. main()에서 asio::post를 거쳐 io_context 스레드에서 호출된다.
void client_session::send_chat(std::string_view text)
{
    if (!socket_.is_open())
        return;
    if (player_id_ == INVALID_ID)
    {
        client_log.warn("not logged in yet");
        return;
    }
    // main() already converts console input to UTF-8 (common/console.h). This catches input
    // piped in with another encoding before protobuf and the server complain about it.
    //
    // main()이 이미 콘솔 입력을 UTF-8로 변환한다 (common/console.h). 이 검사는 다른 인코딩으로
    // 파이프된 입력을 protobuf와 서버가 불평하기 전에 잡아낸다.
    if (!utf8::is_valid(text))
    {
        client_log.warn("input is not valid UTF-8, not sent");
        return;
    }
    chat::chat_req req;
    req.set_text(std::string(text));
    send_message(req);
}

// Synchronous write: the client sends little and only from the io thread, so a blocking write is
// simpler than a send queue and never interleaves with another write.
//
// 동기 쓰기: 클라이언트는 보내는 양이 적고 io 스레드에서만 보내므로, 블로킹 쓰기가 송신 큐보다
// 단순하고 다른 쓰기와 섞이지도 않는다.
void client_session::send_message(const google::protobuf::Message& m)
{
    try
    {
        lpn::write_frame(socket_, lpn::make_tunnel(LOBBY, lpn::packet_type::data, lobby_sid_, lpn::encode_message(m)));
    }
    catch (const std::system_error& e)
    {
        client_log.error("send failed: ", netsys::describe(e.code()));
        close();
    }
}

// Idempotent. Cancelling the timer ends heartbeat_loop; closing the socket ends run() with
// operation_aborted, which is not reported as an error.
//
// 멱등이다. 타이머를 취소하면 heartbeat_loop가 끝나고, 소켓을 닫으면 run()이 operation_aborted로
// 끝나는데 이는 오류로 보고하지 않는다.
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
//
// 연결하고 LOBBY 터널을 연 뒤 연결이 끝날 때까지 프레임을 읽는다. 로그인은 서버의 CONNECT 응답이
// 어떤 sid를 쓸지 알려준 뒤 on_frame에서 한다.
asio::awaitable<void> client_session::run(std::string host, std::uint16_t port)
{
    try
    {
        asio::ip::tcp::resolver resolver(io_);
        const auto endpoints = co_await resolver.async_resolve(host, std::to_string(port), asio::use_awaitable);
        co_await asio::async_connect(socket_, endpoints, asio::use_awaitable);
        socket_.set_option(asio::ip::tcp::no_delay(true));

        asio::co_spawn(
            io_,
            [self = shared_from_this()]
            {
                return self->heartbeat_loop();
            },
            asio::detached);

        // Open the LOBBY tunnel. id 0 = anycast: the server answers with its real sid.
        // LOBBY 터널을 연다. id 0 = anycast: 서버는 실제 sid로 응답한다.
        const lpn::sid anycast{0, 0, LOBBY_ID, 0};
        lpn::write_frame(socket_, lpn::make_tunnel(LOBBY, lpn::packet_type::connect, anycast.value()));

        for (;;)
        {
            const lpn::frame f = co_await lpn::read_frame(socket_);
            on_frame(f);
        }
    }
    catch (const std::system_error& e)
    {
        if (e.code() != asio::error::operation_aborted)
            client_log.error("disconnected: ", netsys::describe(e.code()));
    }
    catch (const std::exception& e)
    {
        client_log.error("error: ", e.what());
    }
    close();
}

// NOOPREQ every HEARTBEAT_INTERVAL (3 s). The server drops a connection after SESSION_TIMEOUT
// (5 s) without any packet, so one lost heartbeat is tolerated, two are not.
//
// HEARTBEAT_INTERVAL(3초)마다 NOOPREQ를 보낸다. 서버는 SESSION_TIMEOUT(5초) 동안 패킷이 없으면
// 연결을 끊으므로, 하트비트 하나를 잃는 것은 괜찮지만 둘은 아니다.
asio::awaitable<void> client_session::heartbeat_loop()
{
    try
    {
        while (socket_.is_open())
        {
            heartbeat_timer_.expires_after(lpn::HEARTBEAT_INTERVAL);
            co_await heartbeat_timer_.async_wait(asio::use_awaitable);
            if (socket_.is_open())
                lpn::write_frame(socket_, lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
        }
    }
    catch (const std::system_error&)
    {
        // timer cancelled or socket closed
        // 타이머가 취소되었거나 소켓이 닫혔다
    }
}

// Tunnel-level handling of one received frame. DATA goes to the msgid dispatcher; the other
// tunnel packets change the tunnel state (bound sid) or end the session.
//
// 수신한 프레임 하나의 터널 수준 처리. DATA는 msgid 디스패처로 간다. 다른 터널 패킷은
// 터널 상태(바인딩된 sid)를 바꾸거나 세션을 끝낸다.
void client_session::on_frame(const lpn::frame& f)
{
    if (!f.is_tunnel())
        // heartbeat responses and unknown types need no action
        // 하트비트 응답과 알 수 없는 타입은 조치가 필요 없다
        return;
    if (f.tunnel_id() != LOBBY_ID)
        return;

    switch (f.type)
    {
    // The server accepted the tunnel and told us its real sid: from now on DATA carries it.
    // This is also the moment to log in.
    //
    // 서버가 터널을 수락하고 실제 sid를 알려 주었다: 이제부터 DATA는 그 sid를 싣는다.
    // 로그인할 순간이기도 하다.
    case lpn::packet_type::connect:
    {
        lobby_sid_ = f.server_sid;
        client_log.info("LOBBY tunnel open [server:", lpn::sid::from_value(lobby_sid_).to_string(), "]");
        chat::login_req req;
        req.set_name(name_);
        send_message(req);
        break;
    }
    case lpn::packet_type::data:
    {
        std::uint32_t msgid = 0;
        const auto r = lobby_.dispatch(*this, f.payload, &msgid);
        if (r != lpn::dispatch_result::ok && !f.payload.empty())
            client_log.warn("dropped message: ", lpn::to_string(r), " msgid=", msgid);
        break;
    }
    // The server moved us to another instance: only the sid changes, the tunnel stays open.
    // 서버가 우리를 다른 인스턴스로 옮겼다: sid만 바뀌고 터널은 열린 채로 남는다.
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
    if (res.error_code_() != nserror::SUCCESS)
    {
        client_log.error("LOGIN FAILED [",
                         nserror::error_code_Name(static_cast<nserror::error_code>(res.error_code_())), "]");
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
    // 채팅은 로그 줄이 아니라 프로그램의 출력이다: 그냥 stdout.
    std::cout << "CHAT from [" << noti.name() << "]: " << noti.text() << std::endl;
}
