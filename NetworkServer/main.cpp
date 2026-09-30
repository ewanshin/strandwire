#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "ServerLib/server.h"
#include "ServerLib/server_log.h"
#include "common/console.h"

namespace
{

// Returns the value following `name` in argv, or nullptr.
const char* option_value(int argc, char* argv[], std::string_view name)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (name == argv[i])
            return argv[i + 1];
    return nullptr;
}

} // namespace

int main(int argc, char* argv[])
{
    console::init_utf8_output(); // player names in the log are UTF-8

    server_options options;
    options.threads = std::thread::hardware_concurrency();

    nslog::configuration log_conf;
    log_conf.module_name = "NetworkServer";

    if (const char* v = option_value(argc, argv, "--port"))
        options.port = static_cast<std::uint16_t>(std::atoi(v));
    if (const char* v = option_value(argc, argv, "--threads"))
        options.threads = static_cast<unsigned>(std::atoi(v));
    if (const char* v = option_value(argc, argv, "--timeout-ms"))
        options.session_timeout = std::chrono::milliseconds(std::atoi(v));
    if (const char* v = option_value(argc, argv, "--sid")) {
        const auto parsed = lpn::sid::parse(v);
        if (!parsed) {
            std::cerr << "invalid --sid, expected domain.idc.type.id (e.g. 0.0.11.1)\n";
            return 1;
        }
        options.sid = *parsed;
    }
    // Logging: console only by default. --log-dir turns the daily file log on.
    if (const char* v = option_value(argc, argv, "--log-level"))
        log_conf.log_level = nslog::parse_level(v, log_conf.log_level);
    if (const char* v = option_value(argc, argv, "--log-dir"))
        log_conf.folder_name = v;

    if (!server_log.start(log_conf))
        return 1;

    int exit_code = 0;
    try {
        server srv(options);
        srv.start();
        {
            // ESC on the console stops the server cleanly, exactly like Ctrl+C / SIGTERM.
            // Inactive when stdin is not an interactive console.
            console::key_watcher esc([&srv] {
                server_log.info("ESC pressed: stopping");
                srv.stop();
            });
            if (esc.active())
                server_log.info("press ESC or Ctrl+C to stop");
            srv.wait();
        } // the watcher thread is joined here, before srv is destroyed
        server_log.info("server stopped");
    } catch (const std::exception& e) {
        server_log.fatal("server failed: ", e.what());
        exit_code = 1;
    }
    server_log.shutdown_async(); // before main() returns, while spdlog's statics are alive
    return exit_code;
}
