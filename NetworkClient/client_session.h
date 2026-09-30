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
    void on_login_res(const chat::login_res& res);
    void on_chat_res(const chat::chat_res& res);
    void on_chat_noti(const chat::chat_noti& noti);

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer heartbeat_timer_;
    std::string name_;
    std::uint64_t lobby_sid_ = 0; // 0 until the server accepts the tunnel
    std::int32_t player_id_ = INVALID_ID;
    lpn::message_dispatcher<client_session> lobby_;
};
