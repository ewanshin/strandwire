#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <asio.hpp>

#include "chat.pb.h"
#include "common/lpn/dispatcher.h"
#include "common/lpn/frame.h"

// One load-test connection: opens the LOBBY tunnel, logs in, then sends a chat line every second
// and a NOOP heartbeat every lpn::HEARTBEAT_INTERVAL.
class dummy_session : public std::enable_shared_from_this<dummy_session>
{
public:
    dummy_session(asio::io_context& io, int index);

    void start(asio::ip::tcp::endpoint target);
    bool logged_in() const noexcept { return logged_in_; }
    std::size_t chats_received() const noexcept { return chats_received_; }

private:
    asio::awaitable<void> run(asio::ip::tcp::endpoint target);
    asio::awaitable<void> chat_loop();
    asio::awaitable<void> heartbeat_loop();
    void on_frame(const lpn::frame& f);
    void send_message(const google::protobuf::Message& m);
    void close();

    void on_login_res(const chat::login_res& res);
    void on_chat_res(const chat::chat_res& res);
    void on_chat_noti(const chat::chat_noti& noti);

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer chat_timer_;
    asio::steady_timer heartbeat_timer_;
    int index_;
    std::string name_;
    std::uint64_t lobby_sid_ = 0;
    bool logged_in_ = false;
    std::size_t chats_received_ = 0;
    lpn::message_dispatcher<dummy_session> lobby_;
};
