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

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ServerLib/server.h"
#include "ServerLib/server_config.h"

// Something that must be brought up before the server listens and torn down after it stops.
class server_component
{
public:
    virtual ~server_component() = default;
    virtual const char* name() const = 0;
    virtual bool init_instance() = 0; // false = phase failed; the reason should already be logged
    virtual void exit_instance() = 0; // called only if init_instance() returned true
};

class server_app
{
public:
    server_app();
    ~server_app(); // calls exit_instance()

    server_app(const server_app&) = delete;
    server_app& operator=(const server_app&) = delete;

    // Register components before init_instance(). Initialised in registration order, torn down in
    // reverse. A component may hold references to the ones registered before it: they are up when
    // it starts and still up when it shuts down.
    void add_connection(std::unique_ptr<server_component> c);
    void add_asset(std::unique_ptr<server_component> c);

    // Phases 1-2: what everything else needs first. Takes the config, prints it, and starts
    // the logger with them. Loading the config from the command line is the executable's job
    // (load_config in server_config.h), so the app does not care whether they came from
    // argv, a test or an embedder.
    bool pre_init_instance(server_config config);
    // Phases 3-5: connections, assets, listen. Requires pre_init_instance().
    bool init_instance();
    // Phase 6: serve. Requires init_instance(). After this the server accepts connections and
    // handles packets; only stop()/wait()/exit_instance() remain.
    bool start();

    // Typical main(): pre_init_instance -> init_instance -> start -> wait -> exit_instance.
    // stop() is what ESC calls; Ctrl+C and SIGTERM reach server::stop() directly through its
    // signal_set, so wait() returns for both.
    void stop();          // request a stop from any thread; non-blocking
    void wait();          // blocks until the server has stopped
    void exit_instance(); // reverse teardown of every phase that came up; idempotent

    bool listening() const { return server_ != nullptr; } // listen phase up: the port is held
    bool serving() const { return is_up(phase_id::serve); } // serve phase up: packets are handled
    std::uint16_t port() const { return server_ ? server_->port() : 0; }
    const server_config& config() const { return config_; } // valid after pre_init_instance()

    // "up:config", "up:logger", "up:connection:<name>", "fail:asset:<name>", "down:listen", ...
    // in the order they happened. For tests and diagnostics.
    const std::vector<std::string>& trace() const { return trace_; }

private:
    // One row of the phase table (see server_app.cpp): a name for the log and trace, the step
    // that brings the phase up, and the step that undoes it.
    struct phase
    {
        const char* name;
        bool (server_app::*up)();
        void (server_app::*down)();
    };
    // The phases in order. Each value is the row's index in PHASES.
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
    static const phase PHASES[]; // one row per phase_id, in that order
    static constexpr std::size_t index(phase_id p) { return static_cast<std::size_t>(p); }

    // Brings up every phase from the first one not yet up through `last` (inclusive), in order;
    // on failure tears everything down and returns false.
    bool run_phases_through(phase_id last);
    // Whether every phase up to and including p is up.
    bool is_up(phase_id p) const { return phases_up_ > index(p); }
    // The earlier lifecycle method succeeded, so the next one may run.
    bool is_pre_init_success() const { return is_up(phase_id::logger); }
    bool is_init_success() const { return is_up(phase_id::listen); }
    static const char* step_name(std::size_t phase); // "pre_init_instance", "init_instance" or "start"

    bool up_config();
    bool up_logger();
    bool up_connections();
    bool up_assets();
    bool up_listen();
    bool up_serve();
    void down_nothing() {}
    void down_logger();
    void down_connections();
    void down_assets();
    void down_listen();
    void down_serve();

    bool run_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                        const char* kind);
    void stop_components(std::vector<std::unique_ptr<server_component>>& list, std::size_t& started,
                         const char* kind);

    server_config config_;
    bool have_config_ = false; // pre_init_instance() succeeded
    std::vector<std::unique_ptr<server_component>> connections_;
    std::vector<std::unique_ptr<server_component>> assets_;
    std::size_t connections_started_ = 0; // how many of connections_ are up; unwound by down_connections
    std::size_t assets_started_ = 0;      // same for assets_
    std::unique_ptr<server> server_;      // exists only while the listen phase is up
    std::size_t phases_up_ = 0;           // how many leading entries of PHASES are up
    std::vector<std::string> trace_;
};
