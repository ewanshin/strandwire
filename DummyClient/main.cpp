#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <asio.hpp>

#include "DummyClient/client_log.h"
#include "DummyClient/dummy_session.h"
#include "common/server_define.h"

namespace
{

// Returns the value following `name` in argv, or nullptr. Options are all "--name value".
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
    std::string ip = "127.0.0.1";
    std::uint16_t port = LISTEN_PORT;
    int sessions = 100;
    int duration_sec = 0;

    if (argc < 2)
        std::cout
            << "Usage: " << argv[0]
            << " --ip 127.0.0.1 --port 10000 --session 100 --duration 10 [--log-level info] [--log-dir <folder>]\n";
    if (const char* v = option_value(argc, argv, "--ip"))
        ip = v;
    if (const char* v = option_value(argc, argv, "--port"))
        port = static_cast<std::uint16_t>(std::atoi(v));
    if (const char* v = option_value(argc, argv, "--session"))
        sessions = std::atoi(v);
    if (const char* v = option_value(argc, argv, "--duration"))
        duration_sec = std::atoi(v);

    // Logging: console only by default. --log-dir turns the daily file log on.
    // Per-session "LOGIN OK" lines are debug level; use --log-level debug to see them.
    nslog::configuration log_conf;
    log_conf.module_name = "DummyClient";
    if (const char* v = option_value(argc, argv, "--log-level"))
        log_conf.log_level = nslog::parse_level(v, log_conf.log_level);
    if (const char* v = option_value(argc, argv, "--log-dir"))
        log_conf.folder_name = v;
    if (!client_log.start(log_conf))
        return 1;

    // One io_context, one thread, N sessions. Sessions are kept in `list` only so their
    // statistics can be read after io.run() returns; the io_context keeps them alive meanwhile.
    asio::io_context io;
    std::vector<std::shared_ptr<dummy_session>> list;
    try
    {
        const asio::ip::tcp::endpoint target(asio::ip::make_address(ip), port);
        for (int i = 0; i < sessions; ++i)
        {
            auto s = std::make_shared<dummy_session>(io, 1001 + i);
            s->start(target);
            list.push_back(std::move(s));
        }

        // --duration N stops the whole run after N seconds; 0 runs until Ctrl+C.
        asio::steady_timer stop_timer(io);
        if (duration_sec > 0)
        {
            stop_timer.expires_after(std::chrono::seconds(duration_sec));
            stop_timer.async_wait(
                [&io](const std::error_code& ec)
                {
                    if (!ec)
                        io.stop();
                });
        }
        io.run();
    }
    catch (const std::exception& e)
    {
        client_log.fatal(e.what());
        client_log.stop();
        return 1;
    }
    client_log.stop(); // flush the log before the statistics line; statics are still alive

    // The statistics line is the program's output (scripts parse it), so it goes to stdout, not the
    // log. Exit code 0 only if every session logged in: a pass/fail signal for scripts.
    std::size_t logged_in = 0;
    std::size_t chats = 0;
    for (const auto& s : list)
    {
        if (s->logged_in())
            ++logged_in;
        chats += s->chats_received();
    }
    std::cout << "sessions=" << sessions << " logged_in=" << logged_in << " chats_received=" << chats << '\n';
    return logged_in == static_cast<std::size_t>(sessions) ? 0 : 1;
}
