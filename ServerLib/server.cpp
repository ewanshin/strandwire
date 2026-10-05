#include "ServerLib/server.h"

#include <algorithm>
#include <stdexcept>

#include "ServerLib/lobby_service.h"
#include "ServerLib/server_log.h"
#include "common/error_text.h"

// Construction registers the message handlers but opens nothing: the socket is bound in
// init_instance(). A msgid collision between two handlers throws here, before any port is taken.
//
// 생성은 메시지 핸들러를 등록할 뿐 아무것도 열지 않는다. 소켓은 init_instance()에서 바인딩한다.
// 두 핸들러의 msgid 충돌은 포트를 잡기 전인 여기서 throw한다.
server::server(server_options options)
    : options_(options),
      acceptor_(io_),
      signals_(io_, SIGINT, SIGTERM),
      ctx_{options.sid, options.session_timeout, {}}
{
    options_.threads = std::max(1u, options_.threads);
    // throws std::logic_error on a msgid collision
    // msgid 충돌이면 std::logic_error를 던진다
    register_lobby_handlers(ctx_.lobby);
}

// Destroying a running server stops it first so the worker threads never outlive the io_context
// they run on. All three calls are no-ops if they already happened.
//
// 실행 중인 서버를 파괴하면 먼저 멈춘다. 그래서 워커 스레드가 자신이 도는 io_context보다 오래
// 살지 않는다. 세 호출 모두 이미 일어났다면 아무 일도 하지 않는다.
server::~server()
{
    stop();
    wait();
    exit_instance();
}

// Open, bind, listen. The kernel now completes handshakes and queues the connections (and any
// bytes they send) in the backlog; the process holds the port but touches no connection until
// start(). Throws (port in use, no permission) before any thread exists.
//
// open, bind, listen. 이제 커널이 핸드셰이크를 끝내고 연결(과 그 연결이 보낸 바이트)을 backlog에
// 쌓는다. 프로세스는 포트를 쥐고 있지만 start()까지 어떤 연결도 건드리지 않는다.
// 스레드가 생기기 전에 throw한다 (사용 중인 포트, 권한 없음).
void server::init_instance()
{
    const asio::ip::tcp::endpoint ep(asio::ip::make_address(options_.ip), options_.port);
    acceptor_.open(ep.protocol());
    // Lets the server restart right away while old connections are still in TIME_WAIT.
    // 옛 연결이 아직 TIME_WAIT에 있어도 서버를 바로 재시작할 수 있게 한다.
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    server_log.info("listening on ", options_.ip, ":", port(), ", not accepting yet");
}

// Launch everything that runs on the io_context: the signal handler, the accept loop and the
// worker threads. Returns as soon as the workers are running; the caller waits with wait().
//
// io_context에서 도는 모든 것을 띄운다. 시그널 핸들러, accept 루프, 워커 스레드.
// 워커가 돌기 시작하면 바로 돌아온다. 호출자는 wait()로 기다린다.
void server::start()
{
    if (!acceptor_.is_open())
        throw std::logic_error("server::start() before init_instance()");

    // Ctrl+C / SIGTERM: asio delivers the signal as a completion on the io_context, so stop()
    // runs on a worker thread like any other handler. ec is set when stop() cancels the wait.
    //
    // Ctrl+C / SIGTERM: asio는 시그널을 io_context의 완료로 전달하므로 stop()은 다른 핸들러처럼
    // 워커 스레드에서 실행된다. stop()이 대기를 취소하면 ec가 설정된다.
    signals_.async_wait(
        [this](const std::error_code& ec, int)
        {
            if (!ec)
                stop();
        });

    asio::co_spawn(io_, accept_loop(), asio::detached);

    // N workers share one io_context. Per-session strands (session.h) keep each session's handlers
    // from running concurrently; the workers only decide which session runs on which thread.
    //
    // 워커 N개가 io_context 하나를 공유한다. 세션별 strand(session.h)가 각 세션의 핸들러가 동시에
    // 실행되지 않게 막는다. 워커는 어느 세션이 어느 스레드에서 도는지만 정한다.
    for (unsigned i = 0; i < options_.threads; ++i)
        threads_.emplace_back(
            [this]
            {
                io_.run();
            });

    server_log.info("server start: ip=", options_.ip, " port=", port(), " threads=", options_.threads,
                    " sid=", options_.sid.to_string(), " timeout=", options_.session_timeout.count(), "ms");
}

// Closes the listen socket directly, so it must run when no worker thread is alive: after wait(),
// or when start() never happened. The acceptor was already closed by stop() on a started server.
//
// listen 소켓을 직접 닫으므로 살아 있는 워커 스레드가 없을 때 실행해야 한다. wait() 뒤, 또는
// start()가 일어나지 않았을 때. 시작된 서버에서는 stop()이 이미 acceptor를 닫았다.
void server::exit_instance()
{
    std::error_code ec;
    acceptor_.close(ec);
}

// Request shutdown from any thread. Non-blocking: the work is posted to the io_context so the
// acceptor and the signal set are only touched on their own executor. Once the acceptor and every
// session are closed nothing is pending, io_context::run() returns in every worker, and wait()
// completes. Idempotent through the atomic flag.
//
// 어느 스레드에서든 종료를 요청한다. 논블로킹이다. 작업을 io_context에 post하므로 acceptor와
// signal set은 자기 executor에서만 만진다. acceptor와 모든 세션이 닫히면 대기 중인 것이 없어
// 모든 워커에서 io_context::run()이 돌아오고 wait()가 끝난다. atomic 플래그로 멱등이다.
void server::stop()
{
    if (stopping_.exchange(true))
        return;
    asio::post(io_,
               [this]
               {
                   std::error_code ec;
                   signals_.cancel(ec);
                   acceptor_.close(ec);
                   manager_.close_all();
               });
}

// Joins the workers. Returns only after stop() drained all work; on a running server without a
// stop() request it blocks until a signal stops it.
//
// 워커를 join한다. stop()이 모든 작업을 비운 뒤에만 돌아온다. stop() 요청 없이 실행 중인 서버에서는
// 시그널이 멈출 때까지 블로킹한다.
void server::wait()
{
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
}

// The port actually bound. Differs from options_.port only when that was 0 (ephemeral, tests).
// 실제로 바인딩된 포트. options_.port가 0일 때(임시 포트, 테스트)만 그 값과 다르다.
std::uint16_t server::port() const
{
    std::error_code ec;
    const auto ep = acceptor_.local_endpoint(ec);
    return ec ? 0 : ep.port();
}

// One accept at a time until the acceptor is closed. Each accepted socket is created on a fresh
// strand, which becomes that session's executor for its whole life.
//
// acceptor가 닫힐 때까지 한 번에 accept 하나. accept된 소켓마다 새 strand 위에 만들어지고,
// 그 strand가 세션의 평생 executor가 된다.
asio::awaitable<void> server::accept_loop()
{
    for (;;)
    {
        auto strand = asio::make_strand(io_);
        std::error_code ec;
        session::socket_type socket =
            co_await acceptor_.async_accept(strand, asio::redirect_error(asio::use_awaitable, ec));
        if (ec == asio::error::operation_aborted)
            // acceptor closed by stop(): the loop ends here
            // stop()이 acceptor를 닫았다. 루프는 여기서 끝난다
            co_return;
        if (ec)
        {
            // Transient failure (e.g. out of descriptors): log it and keep accepting.
            // 일시적 실패 (예: 디스크립터 소진). 로그만 남기고 계속 accept한다.
            server_log.error("accept failed: ", netsys::describe(ec));
            continue;
        }
        // Register before start() so a session that closes immediately still finds itself in the map.
        // start() 전에 등록한다. 그래야 바로 닫히는 세션도 맵에서 자신을 찾는다.
        auto s = std::make_shared<session>(std::move(socket), manager_, ctx_);
        manager_.add(s);
        s->start();
    }
}
