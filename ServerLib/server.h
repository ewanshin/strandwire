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
    // address to listen on; 0.0.0.0 = every interface
    // listen할 주소. 0.0.0.0 = 모든 인터페이스
    std::string ip = "0.0.0.0";
    // 0 picks an ephemeral port (see server::port())
    // 0은 임시 포트를 고른다 (server::port() 참고)
    std::uint16_t port = LISTEN_PORT;
    unsigned threads = 1; // clamped to at least 1 / 최소 1로 보정한다
    // This server's own sid. Its type decides which tunnel it serves; anycast CONNECTs for that
    // type are accepted with this value. Default: LOBBY instance 1.
    //
    // 이 서버 자신의 sid. type이 어느 터널을 서비스할지 정한다. 그 type의 anycast CONNECT는
    // 이 값으로 받아들인다. 기본값: LOBBY 인스턴스 1.
    lpn::sid sid{0, 0, static_cast<std::uint16_t>(lpn::tunnel::lobby), 1};
    std::chrono::milliseconds session_timeout = lpn::SESSION_TIMEOUT;
};

// Owns the io_context, acceptor, worker threads, the session registry and the message handlers.
// io_context, acceptor, 워커 스레드, 세션 레지스트리, 메시지 핸들러를 소유한다.
class server
{
public:
    explicit server(server_options options);
    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    // Opens, binds and listens. From here the OS completes TCP handshakes into the backlog, but
    // nothing is accepted or read until start(). Throws on a port in use or no permission.
    //
    // open, bind, listen을 한다. 이제부터 OS가 TCP 핸드셰이크를 끝내 backlog에 넣지만, start()까지
    // 아무것도 accept하거나 읽지 않는다. 사용 중인 포트나 권한 없음이면 throw한다.
    void init_instance();
    // Spawns the signal handler, the accept loop and the worker threads; packets are handled from
    // here. Requires init_instance(). Returns immediately.
    //
    // 시그널 핸들러, accept 루프, 워커 스레드를 띄운다. 이제부터 패킷을 처리한다.
    // init_instance()가 먼저 필요하다. 바로 돌아온다.
    void start();
    // Requests shutdown: closes the acceptor and every session. Non-blocking, idempotent.
    // 종료를 요청한다. acceptor와 모든 세션을 닫는다. 논블로킹이고 멱등이다.
    void stop();
    // Joins the worker threads. Returns after stop() has drained all work.
    // 워커 스레드를 join한다. stop()이 모든 작업을 비운 뒤에 돌아온다.
    void wait();
    // Releases the listen socket. Call after wait() (or without start()); idempotent.
    // listen 소켓을 놓는다. wait() 뒤에 (또는 start() 없이) 호출한다. 멱등이다.
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
