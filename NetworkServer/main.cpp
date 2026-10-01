#include <iostream>

#include "ServerLib/server_app.h"
#include "ServerLib/server_config.h"
#include "ServerLib/server_log.h"
#include "common/console.h"

// Everything that happens between process start and "server ready" lives in server_app
// (ServerLib/server_app.h). This file only wires the console to it.
//
// Exit codes. 0 is success; each set-up step that can fail has its own small positive code, so a
// script or a service manager can tell them apart without reading the log. Positive because an
// exit status is 8 bits on POSIX and an unsigned 32-bit value on Windows: a negative number would
// show up as 255 or 4294967295.
enum exit_code : int
{
    EXIT_OK = 0,
    EXIT_CONFIG = 1, // load_config or pre_init_instance: invalid config, or the logger did not start
    EXIT_INIT = 2,   // init_instance: a connection or an asset did not come up
    EXIT_START = 3,  // start: could not listen
};

// Reading the command line is the executable's job; server_app takes a finished config. The
// three set-up steps are then called one by one so that each failure can be named here. A failed
// step has already torn down what came up, including the logger, so these messages go to
// std::cerr; server_app itself already printed the detailed reason.
int main(int argc, char* argv[])
{
    console::init_utf8_output(); // player names in the log are UTF-8

    // defaults <- --config file <- command line, validated. Every problem is reported at once.
    server_config config;
    try {
        config = load_config(argc, argv);
    } catch (const config_error& e) {
        std::cerr << e.what() << '\n' << SERVER_USAGE;
        std::cerr << "[NetworkServer] invalid configuration, exit code " << EXIT_CONFIG << std::endl;
        return EXIT_CONFIG;
    }

    server_app app;

    if (!app.pre_init_instance(std::move(config))) {
        std::cerr << "[NetworkServer] pre_init_instance failed: the logger did not start, exit code " << EXIT_CONFIG
                  << std::endl;
        return EXIT_CONFIG;
    }
    if (!app.init_instance()) {
        std::cerr << "[NetworkServer] init_instance failed: a connection or an asset did not come up, exit code "
                  << EXIT_INIT << std::endl;
        return EXIT_INIT;
    }
    if (!app.start()) {
        std::cerr << "[NetworkServer] start failed: could not listen, exit code " << EXIT_START << std::endl;
        return EXIT_START;
    }

    {
        // ESC on the console stops the server cleanly, exactly like Ctrl+C / SIGTERM.
        // Inactive when stdin is not an interactive console.
        console::key_watcher esc([&app] {
            server_log.info("ESC pressed: stopping");
            app.stop();
        });
        if (esc.active())
            server_log.info("press ESC or Ctrl+C to stop");
        app.wait();
    } // the watcher thread is joined here, before the app shuts down

    app.exit_instance();
    std::cout << "[NetworkServer] exited with code " << EXIT_OK << std::endl;
    return EXIT_OK;
}
