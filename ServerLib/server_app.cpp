#include "ServerLib/server_app.h"

#include <exception>
#include <iostream>
#include <iterator>

#include "ServerLib/server_log.h"

// The phase table. Order matters: the lifecycle methods walk it forwards in slices
// (pre_init_instance through logger, init_instance through listen, start through serve) and exit_instance() walks it
// backwards. Each entry pairs an "up" step with the "down" step that undoes it. Adding a phase
// means adding one row here, the two member functions, and one phase_id value at the same position.
//
// 단계 표. 순서가 중요하다. 생명주기 메서드는 이 표를 앞으로 구간별로 걷고
// (pre_init_instance는 logger까지, init_instance는 listen까지, start는 serve까지) exit_instance()는
// 뒤로 걷는다. 각 항목은 "up" 함수와 그것을 되돌리는 "down" 함수의 쌍이다. 단계를 추가하려면
// 여기에 한 행, 멤버 함수 둘, 같은 위치에 phase_id 값 하나를 더한다.
const server_app::phase server_app::PHASES[] = {
    {"config", &server_app::up_config, &server_app::down_nothing},
    {"logger", &server_app::up_logger, &server_app::down_logger},
    {"connections", &server_app::up_connections, &server_app::down_connections},
    {"assets", &server_app::up_assets, &server_app::down_assets},
    {"listen", &server_app::up_listen, &server_app::down_listen},
    {"serve", &server_app::up_serve, &server_app::down_serve},
};

namespace
{

// Milliseconds elapsed since t0, for the "phase X up (N ms)" log lines.
// t0 이후 지난 밀리초. "phase X up (N ms)" 로그 줄에 쓴다.
long long ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// The logger exists only between the logger phase's up and down. Everything the app has to say
// before that (config) or after it (the last phases going down) goes to the console instead, so
// the start and the end of the process are never silent. This is the one place that writes a
// log-like line to std::cout/std::cerr; see CLAUDE.md "로그와 문자열".
//
// 로거는 logger 단계의 up과 down 사이에만 존재한다. 그 전(설정)이나 그 후(마지막 단계들이 내려갈 때)에
// 앱이 할 말은 대신 콘솔로 간다. 그래서 프로세스의 시작과 끝이 조용히 지나가지 않는다.
// 로그 같은 줄을 std::cout/std::cerr에 쓰는 곳은 여기뿐이다. CLAUDE.md "로그와 문자열" 참고.
template <class... Args>
void note(const Args&... args)
{
    if (server_log.running())
    {
        server_log.info(args...);
    }
    else
    {
        std::cout << "[NetworkServer] ";
        (std::cout << ... << args) << std::endl;
    }
}

template <class... Args>
void note_error(const Args&... args)
{
    if (server_log.running())
    {
        server_log.fatal(args...);
    }
    else
    {
        std::cerr << "[NetworkServer] ";
        (std::cerr << ... << args) << std::endl;
    }
}

} // namespace

// The lifecycle method a phase index belongs to, for the failure log line.
// 단계 인덱스가 속한 생명주기 메서드. 실패 로그 줄에 쓴다.
const char* server_app::step_name(std::size_t phase)
{
    if (phase < index(phase_id::connections))
        return "pre_init_instance";
    if (phase < index(phase_id::serve))
        return "init_instance";
    return "start";
}

server_app::server_app()
{
    // Checked here rather than at namespace scope because PHASES and phase_id are private.
    // PHASES와 phase_id가 private이라 네임스페이스 범위가 아니라 여기서 검사한다.
    static_assert(std::size(PHASES) == PHASE_COUNT, "one PHASES row per phase_id value");
    static_assert(index(phase_id::serve) + 1 == PHASE_COUNT, "serve is the last phase");
}

// Destroying the app tears down whatever is still up, so a caller that forgets exit_instance()
// (or leaves through an exception) still stops the server and flushes the logger.
//
// 앱을 파괴하면 아직 올라와 있는 것을 내린다. 그래서 exit_instance()를 잊은 호출자도
// (또는 예외로 빠져나가도) 서버를 멈추고 로거를 flush한다.
server_app::~server_app()
{
    exit_instance();
}

void server_app::add_connection(std::unique_ptr<server_component> c)
{
    connections_.push_back(std::move(c));
}

void server_app::add_asset(std::unique_ptr<server_component> c)
{
    assets_.push_back(std::move(c));
}

// Phases 1-2: config, then the logger. The config are already resolved and validated; the
// caller dealt with any error before there was an app (the logger is configured by these very
// config, so a bad configuration cannot be logged). After this every later phase can log.
//
// 단계 1-2: 설정, 그 다음 로거. 설정은 이미 해석되고 검증된 상태다. 앱이 생기기 전에 호출자가
// 오류를 처리했다 (로거는 바로 이 설정으로 구성되므로 잘못된 설정은 로그에 남길 수 없다).
// 이후 모든 단계가 로그를 쓸 수 있다.
bool server_app::pre_init_instance(server_config config)
{
    config_ = std::move(config);
    have_config_ = true;
    if (!run_phases_through(phase_id::logger))
        return false;
    server_log.info("pre_init_instance success");
    return true;
}

// Phases 3-5: external connections, assets, listen socket. The caller checked pre_init_instance's
// result, so nothing is re-checked here; every lifecycle method only reports its own outcome.
//
// 단계 3-5: 외부 연결, 에셋, listen 소켓. 호출자가 pre_init_instance의 결과를 확인했으므로
// 여기서 다시 확인하지 않는다. 각 생명주기 메서드는 자기 결과만 보고한다.
bool server_app::init_instance()
{
    if (!run_phases_through(phase_id::listen))
        return false;
    server_log.info("init_instance success");
    return true;
}

// Phase 6: serve. Only now does the process accept connections and handle packets.
// 단계 6: serve. 이제부터 프로세스가 연결을 받고 패킷을 처리한다.
bool server_app::start()
{
    if (!run_phases_through(phase_id::serve))
        return false;
    server_log.info("start success: server ready");
    return true;
}

// Runs every phase from the first one not yet up through `last`. On the first failure the
// failed phase is unwound (it may be half up: see run_components), then every earlier phase, and
// false is returned.
//
// Every outcome is logged while the logger is still up: each phase's "up" line, and on failure
// one line naming the lifecycle method and the phase, before the teardown takes the logger down.
// The phase itself has already logged the detailed reason.
//
// 아직 올라오지 않은 첫 단계부터 `last`까지 모두 실행한다. 첫 실패에서 실패한 단계를 되감고
// (절반만 올라왔을 수 있다: run_components 참고), 그 다음 앞선 단계들을 되감고 false를 돌려준다.
//
// 모든 결과는 로거가 아직 살아 있을 때 로그에 남긴다. 각 단계의 "up" 줄, 그리고 실패 시
// 생명주기 메서드와 단계를 이름으로 적은 한 줄을, teardown이 로거를 내리기 전에 쓴다.
// 자세한 이유는 단계 자신이 이미 로그에 남겼다.
bool server_app::run_phases_through(phase_id last)
{
    for (std::size_t i = phases_up_; i <= index(last); ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = (this->*PHASES[i].up)();
        if (!ok)
        {
            trace_.push_back(std::string("fail:") + PHASES[i].name);
            note_error(step_name(i), " failed at phase '", PHASES[i].name, "' after ", ms_since(t0),
                       "ms: start-up aborted, tearing down");
            // The failed phase may have brought up part of itself (e.g. the first of two
            // connections). Its down() knows what came up; then the earlier phases follow.
            //
            // 실패한 단계가 자신의 일부를 올렸을 수 있다 (예: 연결 둘 중 첫째). 그 단계의 down()은
            // 무엇이 올라왔는지 안다. 그 다음 앞선 단계들이 따른다.
            (this->*PHASES[i].down)();
            trace_.push_back(std::string("down:") + PHASES[i].name);
            exit_instance();
            return false;
        }
        // Count the phase as up only after it succeeded, so exit_instance() never undoes a phase
        // that never came up.
        //
        // 단계는 성공한 뒤에만 올라온 것으로 센다. 그래야 exit_instance()가 올라온 적 없는 단계를
        // 되돌리지 않는다.
        ++phases_up_;
        trace_.push_back(std::string("up:") + PHASES[i].name);
        note("phase '", PHASES[i].name, "' up (", ms_since(t0), "ms)");
    }
    return true;
}

// Asks the server to stop accepting and to close every session. Safe from any thread: server::stop()
// only posts work to the io_context. ESC (main thread) and SIGINT/SIGTERM (inside server) both end here.
//
// 서버에 accept 중단과 모든 세션 종료를 요청한다. 어느 스레드에서든 안전하다. server::stop()은
// io_context에 작업을 post할 뿐이다. ESC(메인 스레드)와 SIGINT/SIGTERM(server 내부) 모두 여기로 온다.
void server_app::stop()
{
    if (server_)
        server_->stop();
}

// Blocks until the worker threads have finished, i.e. until stop() was requested and every
// session is closed. Returns immediately when the listen phase never came up.
//
// 워커 스레드가 끝날 때까지, 즉 stop()이 요청되고 모든 세션이 닫힐 때까지 블로킹한다.
// listen 단계가 올라온 적 없으면 바로 돌아온다.
void server_app::wait()
{
    if (server_)
        server_->wait();
}

// Reverse teardown. Only phases counted in phases_up_ are undone, each exactly once, so calling
// this twice (or after a failed step) is harmless.
//
// 역순 teardown. phases_up_에 센 단계만 각각 정확히 한 번 되돌린다. 그래서 두 번 호출해도
// (또는 실패한 메서드 뒤에 호출해도) 무해하다.
void server_app::exit_instance()
{
    while (phases_up_ > 0)
    {
        --phases_up_;
        const auto t0 = std::chrono::steady_clock::now();
        (this->*PHASES[phases_up_].down)();
        trace_.push_back(std::string("down:") + PHASES[phases_up_].name);
        // console once the logger is down
        // 로거가 내려간 뒤에는 콘솔로 간다
        note("phase '", PHASES[phases_up_].name, "' down (", ms_since(t0), "ms)");
    }
}

// ---- phases ------------------------------------------------------------------------------
// ---- 단계 --------------------------------------------------------------------------------

// Phase 1. The real work (parse, merge, validate) happened in load_config() before the app
// existed. This step records the phase and shows the result on the console, since the logger is
// not up yet.
//
// 단계 1. 실제 작업(파싱, 병합, 검증)은 앱이 생기기 전 load_config()에서 끝났다. 이 함수는
// 단계를 기록하고 결과를 콘솔에 보여준다. 로거가 아직 올라오지 않았기 때문이다.
bool server_app::up_config()
{
    if (!have_config_)
        return false;
    note("configuration: ", describe(config_));
    return true;
}

// Phase 2. Start the logger with the level and folder from the config. From here on every
// message, including the final configuration, goes through server_log.
//
// 단계 2. 설정의 레벨과 폴더로 로거를 시작한다. 이후 모든 메시지는, 최종 설정을 포함해,
// server_log를 거친다.
bool server_app::up_logger()
{
    if (!server_log.start(config_.log))
    {
        note_error("cannot start the logger (see the message above)");
        return false;
    }
    server_log.info("logger started: level=", nslog::to_string(config_.log.log_level),
                    config_.log.folder_name.empty() ? ", console only" : ", file dir=", config_.log.folder_name);
    return true;
}

// The logger is torn down second to last, so this is the last line in the log. logger::stop()
// flushes and joins the logging thread while spdlog's statics are still alive.
//
// 로거는 끝에서 두 번째로 내려가므로 이것이 로그의 마지막 줄이다. logger::stop()은 spdlog의
// 정적 객체가 아직 살아 있는 동안 flush하고 로깅 스레드를 join한다.
void server_app::down_logger()
{
    server_log.info("stopping the logger: later lines go to the console");
    server_log.stop();
}

// Phases 3 and 4 are the same mechanism over two lists: bring components up in registration
// order, stop at the first failure, and tear them down in reverse. Today both lists are empty;
// they exist so that a database, a cache or an asset loader has a defined place to plug in.
//
// 단계 3과 4는 두 목록에 같은 방식을 적용한다. 컴포넌트를 등록 순서로 올리고, 첫 실패에서 멈추고,
// 역순으로 내린다. 지금은 두 목록 모두 비어 있다. 데이터베이스, 캐시, 에셋 로더가 끼어들 자리를
// 정해 두기 위해 존재한다.
bool server_app::up_connections()
{
    return run_components(connections_, connections_started_, "connection");
}

void server_app::down_connections()
{
    stop_components(connections_, connections_started_, "connection");
}

bool server_app::up_assets()
{
    return run_components(assets_, assets_started_, "asset");
}

void server_app::down_assets()
{
    stop_components(assets_, assets_started_, "asset");
}

// Phase 5, the last of init_instance. server's constructor registers the message handlers (a
// msgid collision throws) and init_instance() binds and listens (a port in use throws). Either
// exception fails the phase. From here the port is held and clients can connect, but no
// connection is accepted and no byte is read: that is phase 6.
//
// 단계 5, init_instance의 마지막. server의 생성자는 메시지 핸들러를 등록하고 (msgid 충돌은 throw)
// init_instance()는 bind와 listen을 한다 (사용 중인 포트는 throw). 어느 예외든 이 단계를 실패시킨다.
// 이제부터 포트를 쥐고 있어 클라이언트가 연결할 수 있지만, 어떤 연결도 accept하지 않고 어떤
// 바이트도 읽지 않는다. 그것은 단계 6이다.
bool server_app::up_listen()
{
    try
    {
        server_ = std::make_unique<server>(config_.server);
        server_->init_instance();
        return true;
    }
    catch (const std::exception& e)
    {
        server_log.fatal("cannot listen: ", e.what());
        server_.reset();
        return false;
    }
}

// Release the listen socket and destroy the server object. Runs after down_serve (or instead of
// it, when serve never came up), so no worker thread is alive.
//
// listen 소켓을 놓고 server 객체를 파괴한다. down_serve 뒤에 (또는 serve가 올라온 적 없으면 그 대신)
// 실행되므로 살아 있는 워커 스레드가 없다.
void server_app::down_listen()
{
    if (!server_)
        return;
    server_->exit_instance();
    server_.reset();
    server_log.info("listen socket closed");
}

// Phase 6, the only phase of start(). Later this is also where the server connects to other
// servers it depends on, before it starts handling packets. The accept loop and the worker
// threads start here; the connections queued since phase 5 are accepted now.
//
// 단계 6, start()의 유일한 단계. 나중에는 패킷 처리를 시작하기 전에 의존하는 다른 서버에 연결하는
// 곳이기도 하다. accept 루프와 워커 스레드가 여기서 시작한다. 단계 5 이후 쌓인 연결을 이제 accept한다.
bool server_app::up_serve()
{
    try
    {
        server_->start();
        return true;
    }
    catch (const std::exception& e)
    {
        server_log.fatal("cannot start serving: ", e.what());
        return false;
    }
}

// Stop accepting, close every session, join the workers. The listen socket stays with phase 5.
// accept를 멈추고, 모든 세션을 닫고, 워커를 join한다. listen 소켓은 단계 5의 몫이다.
void server_app::down_serve()
{
    server_->stop();
    server_->wait();
    server_log.info("server stopped");
}

// Brings up list[started..] one by one. `started` is the number of components whose
// init_instance() succeeded; it is a member (not a local) so that stop_components() knows how far
// to unwind even when this function returns false half way through.
//
// list[started..]를 하나씩 올린다. `started`는 init_instance()가 성공한 컴포넌트의 수다. 지역 변수가
// 아니라 멤버인 이유는, 이 함수가 도중에 false를 돌려줘도 stop_components()가 어디까지 되감을지
// 알아야 하기 때문이다.
bool server_app::run_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                                const char* kind)
{
    if (list.empty())
        server_log.info("no ", kind, "s configured");
    for (; started < list.size(); ++started)
    {
        server_component& c = *list[started];
        const auto t0 = std::chrono::steady_clock::now();
        if (!c.init_instance())
        {
            trace_.push_back(std::string("fail:") + kind + ":" + c.name());
            server_log.error(kind, " '", c.name(), "' failed to initialise");
            return false;
        }
        trace_.push_back(std::string("up:") + kind + ":" + c.name());
        server_log.info(kind, " '", c.name(), "' ready (", ms_since(t0), "ms)");
    }
    return true;
}

// Tears down exactly the components that came up, last first. A component whose init_instance()
// failed is never torn down: its exit_instance() may assume init_instance() succeeded.
//
// 올라온 컴포넌트만 정확히 그만큼, 마지막 것부터 내린다. init_instance()가 실패한 컴포넌트는
// 절대 내리지 않는다. 그 exit_instance()는 init_instance()가 성공했다고 가정해도 된다.
void server_app::stop_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                                 const char* kind)
{
    while (started > 0)
    {
        --started;
        server_component& c = *list[started];
        c.exit_instance();
        trace_.push_back(std::string("down:") + kind + ":" + c.name());
        server_log.info(kind, " '", c.name(), "' shut down");
    }
}
