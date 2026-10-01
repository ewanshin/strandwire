#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <asio.hpp>

#include "NetworkClient/client_log.h"
#include "NetworkClient/client_session.h"
#include "common/console.h"

namespace
{

// Returns the value following `name` anywhere after the three positional arguments, or nullptr.
const char* option_value(int argc, char* argv[], std::string_view name)
{
    for (int i = 4; i + 1 < argc; ++i)
        if (name == argv[i])
            return argv[i + 1];
    return nullptr;
}

} // namespace

int main(int argc, char* argv[])
{
    console::init_utf8_output();
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << " <host> <port> <name> [--log-level info] [--log-dir <folder>]\n";
        return 1;
    }
    const std::string host = argv[1];
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const std::string name = console::arg_to_utf8(argv[3]); // protobuf strings must be UTF-8

    // Logging: console only by default. --log-dir turns the daily file log on.
    nslog::configuration log_conf;
    log_conf.module_name = "NetworkClient";
    if (const char* v = option_value(argc, argv, "--log-level"))
        log_conf.log_level = nslog::parse_level(v, log_conf.log_level);
    if (const char* v = option_value(argc, argv, "--log-dir"))
        log_conf.folder_name = v;
    if (!client_log.start(log_conf))
        return 1;

    // Two threads: the io_context runs the socket on a background thread, and this thread only
    // blocks on stdin. The work guard keeps io.run() alive while no async operation is pending
    // (e.g. before the connect completes).
    asio::io_context io;
    auto work = asio::make_work_guard(io);
    auto session = std::make_shared<client_session>(io, name);
    session->start(host, port);
    std::thread io_thread(
        [&io]
        {
            io.run();
        });

    // Every line is handed to the io thread with asio::post, so the socket is only ever touched
    // from one thread.
    std::cout << "type a message and press enter. 'quit' to exit." << std::endl;
    std::string line;
    while (console::read_line(line))
    { // UTF-8, also when typed into a CP949 console
        if (line == "quit")
            break;
        if (line.empty())
            continue;
        asio::post(io,
                   [session, line]
                   {
                       session->send_chat(line);
                   });
    }

    // Orderly exit: close the socket on the io thread, drop the work guard so io.run() returns
    // once the close has completed, then join.
    asio::post(io,
               [session]
               {
                   session->close();
               });
    work.reset();
    io_thread.join();
    client_log.stop(); // before main() returns, while spdlog's statics are alive
    std::cout << "finished" << std::endl;
    return 0;
}
