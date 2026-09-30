#include "ServerLib/server_app.h"

#include <cstdio>
#include <exception>

#include "ServerLib/server_log.h"

// The phase table. Order matters: start() walks it forwards, shutdown() walks it backwards.
// Each entry pairs an "up" step with the "down" step that undoes it. Adding a phase means adding
// one row here plus the two member functions; nothing else has to change.
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

} // namespace

server_app::server_app() = default;

// Destroying the app tears down whatever is still up, so a caller that forgets shutdown() (or
// leaves through an exception) still stops the server and flushes the logger.
server_app::~server_app()
{
    shutdown();
}

void server_app::add_connection(std::unique_ptr<server_component> c)
{
    connections_.push_back(std::move(c));
}

void server_app::add_asset(std::unique_ptr<server_component> c)
{
    assets_.push_back(std::move(c));
}

// Entry point for main(): turn argv into settings, then run the phases.
// A configuration error is the one failure that cannot be logged, because the logger is
// configured by the very settings that failed. It goes to stderr together with the usage text.
int server_app::start(int argc, char* const argv[])
{
    try {
        settings_ = resolve_settings(argc, argv);
    } catch (const config_error& e) {
        std::fprintf(stderr, "%s\n%s", e.what(), SERVER_USAGE);
        trace_.push_back("fail:config");
        return 1;
    }
    have_settings_ = true;
    return start(settings_);
}

// Entry point for tests and embedders that already hold settings.
// Runs every phase that is not up yet. On the first failure the failed phase is unwound
// (it may be half up: see run_components), then every earlier phase, and 1 is returned.
int server_app::start(server_settings settings)
{
    settings_ = std::move(settings);
    have_settings_ = true;

    const std::size_t count = sizeof(PHASES) / sizeof(PHASES[0]);
    for (std::size_t i = phases_up_; i < count; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = (this->*PHASES[i].up)();
        if (!ok) {
            trace_.push_back(std::string("fail:") + PHASES[i].name);
            server_log.fatal("phase '", PHASES[i].name, "' failed after ", ms_since(t0), "ms: shutting down");
            // The failed phase may have brought up part of itself (e.g. the first of two
            // connections). Its down() knows what came up; then the earlier phases follow.
            (this->*PHASES[i].down)();
            trace_.push_back(std::string("down:") + PHASES[i].name);
            shutdown();
            return 1;
        }
        // Count the phase as up only after it succeeded, so shutdown() never undoes a phase
        // that never came up.
        ++phases_up_;
        trace_.push_back(std::string("up:") + PHASES[i].name);
        server_log.info("phase '", PHASES[i].name, "' up (", ms_since(t0), "ms)");
    }
    server_log.info("server ready");
    return 0;
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
// this twice (or after a failed start) is harmless.
void server_app::shutdown()
{
    while (phases_up_ > 0) {
        --phases_up_;
        const auto t0 = std::chrono::steady_clock::now();
        (this->*PHASES[phases_up_].down)();
        trace_.push_back(std::string("down:") + PHASES[phases_up_].name);
        // After down_logger() this line is dropped silently (the logger is off); every other
        // phase's line lands normally.
        server_log.info("phase '", PHASES[phases_up_].name, "' down (", ms_since(t0), "ms)");
    }
}

// ---- phases ------------------------------------------------------------------------------

// Phase 1. The real work (parse, merge, validate) happened in start() so that its error could be
// reported before the logger exists. This step only records the phase in the sequence.
bool server_app::up_config()
{
    return have_settings_;
}

// Phase 2. Start the logger with the level and folder from the settings. From here on every
// message, including the final configuration, goes through server_log.
bool server_app::up_logger()
{
    if (!server_log.start(settings_.log))
        return false;
    server_log.info("configuration: ", describe(settings_));
    return true;
}

// The logger is torn down last (its phase came up second), so "server stopped" is the last line.
// shutdown_async() flushes and joins the logging thread while spdlog's statics are still alive.
void server_app::down_logger()
{
    server_log.info("server stopped");
    server_log.shutdown_async();
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

// Phase 5, the last one: only now does the process accept connections. server's constructor
// registers the message handlers (a msgid collision throws) and start() binds, listens and
// launches the worker threads (a port in use throws). Either exception fails the phase.
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
}

// Brings up list[started..] one by one. `started` is the number of components whose init()
// succeeded; it is a member (not a local) so that stop_components() knows how far to unwind
// even when this function returns false half way through.
bool server_app::run_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                                const char* kind)
{
    if (list.empty())
        server_log.info("no ", kind, "s configured");
    for (; started < list.size(); ++started) {
        server_component& c = *list[started];
        const auto t0 = std::chrono::steady_clock::now();
        if (!c.init()) {
            trace_.push_back(std::string("fail:") + kind + ":" + c.name());
            server_log.error(kind, " '", c.name(), "' failed to initialise");
            return false;
        }
        trace_.push_back(std::string("up:") + kind + ":" + c.name());
        server_log.info(kind, " '", c.name(), "' ready (", ms_since(t0), "ms)");
    }
    return true;
}

// Shuts down exactly the components that came up, last first. A component whose init() failed
// is never shut down: its shutdown() may assume init() succeeded.
void server_app::stop_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                                 const char* kind)
{
    while (started > 0) {
        --started;
        server_component& c = *list[started];
        c.shutdown();
        trace_.push_back(std::string("down:") + kind + ":" + c.name());
        server_log.info(kind, " '", c.name(), "' shut down");
    }
}
