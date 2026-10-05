#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <asio.hpp>

#include "chat.pb.h"
#include "common/lpn/dispatcher.h"
#include "common/lpn/frame.h"
#include "common/server_define.h"

// Interactive chat client. All methods after start() must run on the io_context thread.
// Flow: TCP connect -> CONNECT(LOBBY, anycast) -> server's CONNECT with its sid -> login_req
//       -> login_res -> chat. A NOOP heartbeat goes out every lpn::HEARTBEAT_INTERVAL.
//
// 대화형 채팅 클라이언트. start() 이후의 모든 메서드는 io_context 스레드에서 실행해야 한다.
// 흐름: TCP 연결 -> CONNECT(LOBBY, anycast) -> 서버의 sid를 담은 CONNECT -> login_req
//       -> login_res -> 채팅. NOOP 하트비트는 lpn::HEARTBEAT_INTERVAL마다 나간다.
class client_session : public std::enable_shared_from_this<client_session>
{
public:
    client_session(asio::io_context& io, std::string name);

    void start(std::string host, std::uint16_t port);
    void send_chat(std::string_view text);
    void close();

private:
    asio::awaitable<void> run(std::string host, std::uint16_t port);
    asio::awaitable<void> heartbeat_loop();
    void on_frame(const lpn::frame& f);
    void send_message(const google::protobuf::Message& m);

    // LOBBY message handlers (registered in the constructor)
    // LOBBY 메시지 핸들러 (생성자에서 등록한다)
    void on_login_res(const chat::login_res& res);
    void on_chat_res(const chat::chat_res& res);
    void on_chat_noti(const chat::chat_noti& noti);

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer heartbeat_timer_;
    std::string name_;
    std::uint64_t lobby_sid_ = 0; // 0 until the server accepts the tunnel / 서버가 터널을 수락할 때까지 0이다
    std::int32_t player_id_ = INVALID_ID;
    lpn::message_dispatcher<client_session> lobby_;
};
