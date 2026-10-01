#include "ServerLib/server.h"

#include <algorithm>
#include <stdexcept>

#include "ServerLib/lobby_service.h"
#include "ServerLib/server_log.h"
#include "common/error_text.h"

// Construction registers the message handlers but opens nothing: the socket is bound in
// init_instance(). A msgid collision between two handlers throws here, before any port is taken.
server::server(server_options options)
    : options_(options),
      acceptor_(io_),
      signals_(io_, SIGINT, SIGTERM),
      ctx_{options.sid, options.session_timeout, {}}
{
    options_.threads = std::max(1u, options_.threads);
    register_lobby_handlers(ctx_.lobby); // throws std::logic_error on a msgid collision
}

// Destroying a running server stops it first so the worker threads never outlive the io_context
// they run on. All three calls are no-ops if they already happened.
server::~server()
{
    stop();
    wait();
    exit_instance();
}

// Open, bind, listen. The kernel now completes handshakes and queues the connections (and any
// bytes they send) in the backlog; the process holds the port but touches no connection until
// start(). Throws (port in use, no permission) before any thread exists.
void server::init_instance()
{
    const asio::ip::tcp::endpoint ep(asio::ip::make_address(options_.ip), options_.port);
    acceptor_.open(ep.protocol());
    // Lets the server restart right away while old connections are still in TIME_WAIT.
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    server_log.info("listening on ", options_.ip, ":", port(), ", not accepting yet");
}

// Launch everything that runs on the io_context: the signal handler, the accept loop and the
// worker threads. Returns as soon as the workers are running; the caller waits with wait().
void server::start()
{
    if (!acceptor_.is_open())
        throw std::logic_error("server::start() before init_instance()");

    // Ctrl+C / SIGTERM: asio delivers the signal as a completion on the io_context, so stop()
    // runs on a worker thread like any other handler. ec is set when stop() cancels the wait.
    signals_.async_wait([this](const std::error_code& ec, int) {
        if (!ec)
            stop();
    });

    asio::co_spawn(io_, accept_loop(), asio::detached);

    // N workers share one io_context. Per-session strands (session.h) keep each session's handlers
    // from running concurrently; the workers only decide which session runs on which thread.
    for (unsigned i = 0; i < options_.threads; ++i)
        threads_.emplace_back([this] { io_.run(); });

    server_log.info("server start: ip=", options_.ip, " port=", port(), " threads=", options_.threads,
                    " sid=", options_.sid.to_string(), " timeout=", options_.session_timeout.count(), "ms");
}

// Closes the listen socket directly, so it must run when no worker thread is alive: after wait(),
// or when start() never happened. The acceptor was already closed by stop() on a started server.
void server::exit_instance()
{
    std::error_code ec;
    acceptor_.close(ec);
}

// Request shutdown from any thread. Non-blocking: the work is posted to the io_context so the
// acceptor and the signal set are only touched on their own executor. Once the acceptor and every
// session are closed nothing is pending, io_context::run() returns in every worker, and wait()
// completes. Idempotent through the atomic flag.
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

// Joins the workers. Returns only after stop() drained all work; on a running server without a
// stop() request it blocks until a signal stops it.
void server::wait()
{
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
}

// The port actually bound. Differs from options_.port only when that was 0 (ephemeral, tests).
std::uint16_t server::port() const
{
    std::error_code ec;
    const auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

// One accept at a time until the acceptor is closed. Each accepted socket is created on a fresh
// strand, which becomes that session's executor for its whole life.
asio::awaitable<void> server::accept_loop()
{
    for (;;) {
        auto strand = asio::make_strand(io_);
        std::error_code ec;
        session::socket_type socket =
            co_await acceptor_.async_accept(strand, asio::redirect_error(asio::use_awaitable, ec));
        if (ec == asio::error::operation_aborted)
            co_return; // acceptor closed by stop(): the loop ends here
        if (ec) {
            // Transient failure (e.g. out of descriptors): log it and keep accepting.
            server_log.error("accept failed: ", netsys::describe(ec));
            continue;
        }
        // Register before start() so a session that closes immediately still finds itself in the map.
        auto s = std::make_shared<session>(std::move(socket), manager_, ctx_);
        manager_.add(s);
        s->start();
    }
}
