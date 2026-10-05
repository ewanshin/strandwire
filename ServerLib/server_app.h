#pragma once

// The server process as a sequence of phases, grouped into the project's lifecycle methods
// (CLAUDE.md "Lifecycle method names"):
//
//   pre_init_instance   1. config       take the config the caller loaded (load_config in main)
//                       2. logger       start server_log as configured
//   init_instance       3. connections  external environment (DB, cache, discovery): add_connection()
//                       4. assets       data loaded before serving: add_asset()
//                       5. listen       bind/listen: the OS completes handshakes, nothing is accepted or read
//   start               6. serve        (later: connect to other servers) accept loop and workers
//   stop / wait            request a stop, wait until the server has stopped
//   exit_instance          tear down every phase that came up, in reverse
//
// Each step runs the phases in order and stops at the first failure; a failure tears down
// everything that came up and returns false. No packet is handled before start(): until then a
// client can connect, but its bytes wait in the kernel.
// Phases 3 and 4 are empty today: the component interface is the hook for later work.
//
// 서버 프로세스를 단계의 순서로 본 것이다. 단계는 프로젝트의 생명주기 메서드로 묶인다
// (CLAUDE.md "Lifecycle method names"):
//
//   pre_init_instance   1. config       호출자가 로드한 설정을 받는다 (main의 load_config)
//                       2. logger       설정대로 server_log를 시작한다
//   init_instance       3. connections  외부 환경 (DB, 캐시, 디스커버리): add_connection()
//                       4. assets       서비스 전에 로드하는 데이터: add_asset()
//                       5. listen       bind/listen: OS가 핸드셰이크를 끝내지만 accept도 read도 하지 않는다
//   start               6. serve        (나중에: 다른 서버에 연결) accept 루프와 워커
//   stop / wait            정지를 요청하고, 서버가 멈출 때까지 기다린다
//   exit_instance          올라온 단계를 모두 역순으로 내린다
//
// 각 메서드는 단계를 순서대로 실행하고 첫 실패에서 멈춘다. 실패하면 올라온 것을 모두 내리고
// false를 돌려준다. start() 전에는 어떤 패킷도 처리하지 않는다. 그때까지 클라이언트는
// 연결할 수 있지만 보낸 바이트는 커널에서 기다린다.
// 단계 3과 4는 지금 비어 있다. 컴포넌트 인터페이스는 이후 작업을 위한 훅이다.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ServerLib/server.h"
#include "ServerLib/server_config.h"

// Something that must be brought up before the server listens and torn down after it stops.
// 서버가 listen하기 전에 올리고 멈춘 뒤에 내려야 하는 것이다.
class server_component
{
public:
    virtual ~server_component() = default;
    virtual const char* name() const = 0;
    // false = phase failed; the reason should already be logged
    // false = 단계 실패. 이유는 이미 로그에 남아 있어야 한다
    virtual bool init_instance() = 0;
    // called only if init_instance() returned true
    // init_instance()가 true를 돌려준 경우에만 호출된다
    virtual void exit_instance() = 0;
};

class server_app
{
public:
    server_app();
    ~server_app(); // calls exit_instance() / exit_instance()를 호출한다

    server_app(const server_app&) = delete;
    server_app& operator=(const server_app&) = delete;

    // Register components before init_instance(). Initialised in registration order, torn down in
    // reverse. A component may hold references to the ones registered before it: they are up when
    // it starts and still up when it shuts down.
    //
    // 컴포넌트는 init_instance() 전에 등록한다. 등록 순서로 초기화하고 역순으로 내린다.
    // 컴포넌트는 먼저 등록된 것의 참조를 가져도 된다. 자신이 시작할 때 그것들은 올라와 있고,
    // 자신이 내려갈 때도 아직 올라와 있다.
    void add_connection(std::unique_ptr<server_component> c);
    void add_asset(std::unique_ptr<server_component> c);

    // Phases 1-2: what everything else needs first. Takes the config, prints it, and starts
    // the logger with them. Loading the config from the command line is the executable's job
    // (load_config in server_config.h), so the app does not care whether they came from
    // argv, a test or an embedder.
    //
    // 단계 1-2: 다른 모든 것에 앞서 필요한 것. 설정을 받아 출력하고 그 설정으로 로거를 시작한다.
    // 명령줄에서 설정을 로드하는 일은 실행 파일의 몫이다 (server_config.h의 load_config).
    // 그래서 앱은 설정이 argv, 테스트, 임베더 중 어디서 왔는지 신경 쓰지 않는다.
    bool pre_init_instance(server_config config);
    // Phases 3-5: connections, assets, listen. Call after a successful pre_init_instance().
    // 단계 3-5: connections, assets, listen. pre_init_instance()가 성공한 뒤에 호출한다.
    bool init_instance();
    // Phase 6: serve. Call after a successful init_instance(). After this the server accepts connections and
    // handles packets; only stop()/wait()/exit_instance() remain.
    //
    // 단계 6: serve. init_instance()가 성공한 뒤에 호출한다. 이후 서버는 연결을 받고 패킷을 처리한다.
    // 남는 것은 stop()/wait()/exit_instance()뿐이다.
    bool start();

    // Typical main(): pre_init_instance -> init_instance -> start -> wait -> exit_instance.
    // stop() is what ESC calls; Ctrl+C and SIGTERM reach server::stop() directly through its
    // signal_set, so wait() returns for both.
    //
    // 전형적인 main(): pre_init_instance -> init_instance -> start -> wait -> exit_instance.
    // ESC는 stop()을 호출한다. Ctrl+C와 SIGTERM은 signal_set을 통해 server::stop()에 직접 닿는다.
    // 그래서 wait()는 둘 모두에서 돌아온다.
    void stop(); // request a stop from any thread; non-blocking / 어느 스레드에서든 정지를 요청한다. 논블로킹
    void wait(); // blocks until the server has stopped / 서버가 멈출 때까지 블로킹한다
    // reverse teardown of every phase that came up; idempotent
    // 올라온 단계를 모두 역순으로 내린다. 멱등이다
    void exit_instance();

    bool listening() const
    {
        return server_ != nullptr;
    } // listen phase up: the port is held / listen 단계가 올라옴. 포트를 쥐고 있다
    bool serving() const
    {
        return is_up(phase_id::serve);
    } // serve phase up: packets are handled / serve 단계가 올라옴. 패킷을 처리한다
    std::uint16_t port() const
    {
        return server_ ? server_->port() : 0;
    }
    const server_config& config() const
    {
        return config_;
    } // valid after pre_init_instance() / pre_init_instance() 뒤에 유효하다

    // "up:config", "up:logger", "up:connection:<name>", "fail:asset:<name>", "down:listen", ...
    // in the order they happened. For tests and diagnostics.
    //
    // "up:config", "up:logger", "up:connection:<name>", "fail:asset:<name>", "down:listen", ...
    // 일어난 순서대로. 테스트와 진단용이다.
    const std::vector<std::string>& trace() const
    {
        return trace_;
    }

private:
    // One row of the phase table (see server_app.cpp): a name for the log and trace, the step
    // that brings the phase up, and the step that undoes it.
    //
    // 단계 표의 한 행이다 (server_app.cpp 참고): 로그와 trace에 쓰는 이름, 단계를 올리는 함수,
    // 그것을 되돌리는 함수.
    struct phase
    {
        const char* name;
        bool (server_app::*up)();
        void (server_app::*down)();
    };
    // The phases in order. Each value is the row's index in PHASES.
    // 순서대로 나열한 단계. 각 값은 PHASES에서 그 행의 인덱스이다.
    enum class phase_id : std::size_t
    {
        config,      // pre_init_instance
        logger,      //
        connections, // init_instance
        assets,      //
        listen,      //
        serve,       // start
    };
    static constexpr std::size_t PHASE_COUNT = 6;
    static const phase PHASES[]; // one row per phase_id, in that order / phase_id마다 한 행, 같은 순서
    static constexpr std::size_t index(phase_id p)
    {
        return static_cast<std::size_t>(p);
    }

    // Brings up every phase from the first one not yet up through `last` (inclusive), in order;
    // on failure tears everything down and returns false.
    //
    // 아직 올라오지 않은 첫 단계부터 `last`까지 (포함) 순서대로 올린다.
    // 실패하면 모두 내리고 false를 돌려준다.
    bool run_phases_through(phase_id last);
    // Whether every phase up to and including p is up.
    // p까지의 모든 단계가 (p 포함) 올라와 있는지.
    bool is_up(phase_id p) const
    {
        return phases_up_ > index(p);
    }
    // "pre_init_instance", "init_instance" or "start"
    // "pre_init_instance", "init_instance" 또는 "start"
    static const char* step_name(std::size_t phase);

    bool up_config();
    bool up_logger();
    bool up_connections();
    bool up_assets();
    bool up_listen();
    bool up_serve();
    void down_nothing()
    {}
    void down_logger();
    void down_connections();
    void down_assets();
    void down_listen();
    void down_serve();

    bool run_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started, const char* kind);
    void stop_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started, const char* kind);

    server_config config_;
    bool have_config_ = false; // pre_init_instance() succeeded / pre_init_instance()가 성공했다
    std::vector<std::unique_ptr<server_component>> connections_;
    std::vector<std::unique_ptr<server_component>> assets_;
    // how many of connections_ are up; unwound by down_connections
    // connections_ 중 몇 개가 올라와 있는지. down_connections가 되감는다
    std::size_t connections_started_ = 0;
    std::size_t assets_started_ = 0; // same for assets_ / assets_도 같다
    // exists only while the listen phase is up
    // listen 단계가 올라와 있는 동안만 존재한다
    std::unique_ptr<server> server_;
    // how many leading entries of PHASES are up
    // PHASES의 앞에서부터 몇 개가 올라와 있는지
    std::size_t phases_up_ = 0;
    std::vector<std::string> trace_;
};
