#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "ServerLib/session.h"
#include "ServerLib/session_manager.h"
#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "common/server_define.h"

struct server_options
{
    std::string ip = "0.0.0.0";       // address to listen on; 0.0.0.0 = every interface
    std::uint16_t port = LISTEN_PORT; // 0 picks an ephemeral port (see server::port())
    unsigned threads = 1;             // clamped to at least 1
    // This server's own sid. Its type decides which tunnel it serves; anycast CONNECTs for that
    // type are accepted with this value. Default: LOBBY instance 1.
    lpn::sid sid{0, 0, static_cast<std::uint16_t>(lpn::tunnel::lobby), 1};
    std::chrono::milliseconds session_timeout = lpn::SESSION_TIMEOUT;
};

// Owns the io_context, acceptor, worker threads, the session registry and the message handlers.
class server
{
public:
    explicit server(server_options options);
    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    // Opens, binds and listens. From here the OS completes TCP handshakes into the backlog, but
    // nothing is accepted or read until start(). Throws on a port in use or no permission.
    void init_instance();
    // Spawns the signal handler, the accept loop and the worker threads; packets are handled from
    // here. Requires init_instance(). Returns immediately.
    void start();
    // Requests shutdown: closes the acceptor and every session. Non-blocking, idempotent.
    void stop();
    // Joins the worker threads. Returns after stop() has drained all work.
    void wait();
    // Releases the listen socket. Call after wait() (or without start()); idempotent.
    void exit_instance();

    std::uint16_t port() const;
    std::size_t session_count() const
    {
        return manager_.count();
    }
    session_manager& manager()
    {
        return manager_;
    }
    const server_options& options() const
    {
        return options_;
    }

private:
    asio::awaitable<void> accept_loop();

    server_options options_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::signal_set signals_;
    session_manager manager_;
    server_context ctx_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stopping_{false};
};
