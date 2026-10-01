#include "ServerLib/server_app.h"

#include <exception>
#include <iostream>

#include "ServerLib/server_log.h"

// The phase table. Order matters: the lifecycle methods walk it forwards in slices
// (pre_init_instance = [0,2), init_instance = [2,4), start = [4,5)) and exit_instance() walks it
// backwards. Each entry pairs an "up" step with the "down" step that undoes it. Adding a phase
// means adding one row here, the two member functions, and adjusting the PHASE_* indices.
const server_app::phase server_app::PHASES[] = {
    {"config", &server_app::up_config, &server_app::down_nothing},
    {"logger", &server_app::up_logger, &server_app::down_logger},
    {"connections", &server_app::up_connections, &server_app::down_connections},
    {"assets", &server_app::up_assets, &server_app::down_assets},
    {"listen", &server_app::up_listen, &server_app::down_listen},
};

namespace
{

// Milliseconds elapsed since t0, for the "phase X up (N ms)" log lines.
long long ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// The logger exists only between the logger phase's up and down. Everything the app has to say
// before that (config) or after it (the last phases going down) goes to the console instead, so
// the start and the end of the process are never silent. This is the one place that writes a
// log-like line to std::cout/std::cerr; see CLAUDE.md "Logging and strings".
template <class... Args>
void note(const Args&... args)
{
    if (server_log.running()) {
        server_log.info(args...);
    } else {
        std::cout << "[NetworkServer] ";
        (std::cout << ... << args) << std::endl;
    }
}

template <class... Args>
void note_error(const Args&... args)
{
    if (server_log.running()) {
        server_log.fatal(args...);
    } else {
        std::cerr << "[NetworkServer] ";
        (std::cerr << ... << args) << std::endl;
    }
}

} // namespace

// The lifecycle method a phase index belongs to, for the failure log line.
const char* server_app::step_name(std::size_t phase)
{
    if (phase < PHASE_CONNECTIONS)
        return "pre_init_instance";
    if (phase < PHASE_LISTEN)
        return "init_instance";
    return "start";
}

server_app::server_app() = default;

// Destroying the app tears down whatever is still up, so a caller that forgets exit_instance()
// (or leaves through an exception) still stops the server and flushes the logger.
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

// Phases 1-2: settings, then the logger. The settings are already resolved and validated; the
// caller dealt with any error before there was an app (the logger is configured by these very
// settings, so a bad configuration cannot be logged). After this every later phase can log.
bool server_app::pre_init_instance(server_settings settings)
{
    settings_ = std::move(settings);
    have_settings_ = true;
    return run_phases_until(PHASE_CONNECTIONS);
}

// Phases 3-4: external connections, assets.
bool server_app::init_instance()
{
    if (phases_up_ < PHASE_CONNECTIONS)
        return false; // pre_init_instance() did not succeed
    return run_phases_until(PHASE_LISTEN);
}

// Phase 5: listen. Only now does the process accept connections.
bool server_app::start()
{
    if (phases_up_ < PHASE_LISTEN)
        return false; // init_instance() did not succeed
    if (!run_phases_until(PHASE_COUNT))
        return false;
    server_log.info("server ready");
    return true;
}

// Runs every phase from the first one not yet up to `end` (exclusive). On the first failure the
// failed phase is unwound (it may be half up: see run_components), then every earlier phase, and
// false is returned.
//
// Every outcome is logged while the logger is still up: each phase's "up" line, and on failure
// one line naming the lifecycle method and the phase, before the teardown takes the logger down.
// The phase itself has already logged the detailed reason.
bool server_app::run_phases_until(std::size_t end)
{
    for (std::size_t i = phases_up_; i < end; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = (this->*PHASES[i].up)();
        if (!ok) {
            trace_.push_back(std::string("fail:") + PHASES[i].name);
            note_error(step_name(i), " failed at phase '", PHASES[i].name, "' after ", ms_since(t0),
                       "ms: start-up aborted, tearing down");
            // The failed phase may have brought up part of itself (e.g. the first of two
            // connections). Its down() knows what came up; then the earlier phases follow.
            (this->*PHASES[i].down)();
            trace_.push_back(std::string("down:") + PHASES[i].name);
            exit_instance();
            return false;
        }
        // Count the phase as up only after it succeeded, so exit_instance() never undoes a phase
        // that never came up.
        ++phases_up_;
        trace_.push_back(std::string("up:") + PHASES[i].name);
        note("phase '", PHASES[i].name, "' up (", ms_since(t0), "ms)");
    }
    return true;
}

// Asks the server to stop accepting and to close every session. Safe from any thread: server::stop()
// only posts work to the io_context. ESC (main thread) and SIGINT/SIGTERM (inside server) both end here.
void server_app::stop()
{
    if (server_)
        server_->stop();
}

// Blocks until the worker threads have finished, i.e. until stop() was requested and every
// session is closed. Returns immediately when the listen phase never came up.
void server_app::wait()
{
    if (server_)
        server_->wait();
}

// Reverse teardown. Only phases counted in phases_up_ are undone, each exactly once, so calling
// this twice (or after a failed step) is harmless.
void server_app::exit_instance()
{
    while (phases_up_ > 0) {
        --phases_up_;
        const auto t0 = std::chrono::steady_clock::now();
        (this->*PHASES[phases_up_].down)();
        trace_.push_back(std::string("down:") + PHASES[phases_up_].name);
        note("phase '", PHASES[phases_up_].name, "' down (", ms_since(t0), "ms)"); // console once the logger is down
    }
}

// ---- phases ------------------------------------------------------------------------------

// Phase 1. The real work (parse, merge, validate) happened in resolve_settings() before the app
// existed. This step records the phase and shows the result on the console, since the logger is
// not up yet.
bool server_app::up_config()
{
    if (!have_settings_)
        return false;
    note("configuration: ", describe(settings_));
    return true;
}

// Phase 2. Start the logger with the level and folder from the settings. From here on every
// message, including the final configuration, goes through server_log.
bool server_app::up_logger()
{
    if (!server_log.start(settings_.log)) {
        note_error("cannot start the logger (see the message above)");
        return false;
    }
    server_log.info("logger started: level=", nslog::to_string(settings_.log.log_level),
                    settings_.log.folder_name.empty() ? ", console only" : ", file dir=",
                    settings_.log.folder_name);
    return true;
}

// The logger is torn down second to last, so this is the last line in the log. logger::stop()
// flushes and joins the logging thread while spdlog's statics are still alive.
void server_app::down_logger()
{
    server_log.info("stopping the logger: later lines go to the console");
    server_log.stop();
}

// Phases 3 and 4 are the same mechanism over two lists: bring components up in registration
// order, stop at the first failure, and tear them down in reverse. Today both lists are empty;
// they exist so that a database, a cache or an asset loader has a defined place to plug in.
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

// Phase 5, the last one. server's constructor registers the message handlers (a msgid collision
// throws) and start() binds, listens and launches the worker threads (a port in use throws).
// Either exception fails the phase.
bool server_app::up_listen()
{
    try {
        server_ = std::make_unique<server>(settings_.server);
        server_->start();
        return true;
    } catch (const std::exception& e) {
        server_log.fatal("cannot start server: ", e.what());
        server_.reset();
        return false;
    }
}

// Stop accepting, close every session, join the workers, then destroy the server object.
void server_app::down_listen()
{
    if (!server_)
        return;
    server_->stop();
    server_->wait();
    server_.reset();
    server_log.info("server stopped");
}

// Brings up list[started..] one by one. `started` is the number of components whose
// init_instance() succeeded; it is a member (not a local) so that stop_components() knows how far
// to unwind even when this function returns false half way through.
bool server_app::run_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                                const char* kind)
{
    if (list.empty())
        server_log.info("no ", kind, "s configured");
    for (; started < list.size(); ++started) {
        server_component& c = *list[started];
        const auto t0 = std::chrono::steady_clock::now();
        if (!c.init_instance()) {
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
void server_app::stop_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                                 const char* kind)
{
    while (started > 0) {
        --started;
        server_component& c = *list[started];
        c.exit_instance();
        trace_.push_back(std::string("down:") + kind + ":" + c.name());
        server_log.info(kind, " '", c.name(), "' shut down");
    }
}
