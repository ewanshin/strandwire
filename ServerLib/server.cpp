#include "ServerLib/server.h"

#include <algorithm>

#include "ServerLib/lobby_service.h"
#include "ServerLib/server_log.h"
#include "common/error_text.h"

server::server(server_options options)
    : options_(options),
      acceptor_(io_),
      signals_(io_, SIGINT, SIGTERM),
      ctx_{options.sid, options.session_timeout, {}}
{
    options_.threads = std::max(1u, options_.threads);
    register_lobby_handlers(ctx_.lobby); // throws std::logic_error on a msgid collision
}

server::~server()
{
    stop();
    wait();
}

void server::start()
{
    const asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), options_.port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();

    signals_.async_wait([this](const std::error_code& ec, int) {
        if (!ec)
            stop();
    });

    asio::co_spawn(io_, accept_loop(), asio::detached);

    for (unsigned i = 0; i < options_.threads; ++i)
        threads_.emplace_back([this] { io_.run(); });

    server_log.info("server start: port=", port(), " threads=", options_.threads, " sid=", options_.sid.to_string(),
                    " timeout=", options_.session_timeout.count(), "ms");
}

void server::stop()
{
    if (stopping_.exchange(true))
        return;
    asio::post(io_, [this] {
        std::error_code ec;
        signals_.cancel(ec);
        acceptor_.close(ec);
        manager_.close_all();
    });
}

void server::wait()
{
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
}

std::uint16_t server::port() const
{
    std::error_code ec;
    const auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

asio::awaitable<void> server::accept_loop()
{
    for (;;) {
        auto strand = asio::make_strand(io_);
        std::error_code ec;
        session::socket_type socket =
            co_await acceptor_.async_accept(strand, asio::redirect_error(asio::use_awaitable, ec));
        if (ec == asio::error::operation_aborted)
            co_return;
        if (ec) {
            server_log.error("accept failed: ", netsys::describe(ec));
            continue;
        }
        auto s = std::make_shared<session>(std::move(socket), manager_, ctx_);
        manager_.add(s);
        s->start();
    }
}
