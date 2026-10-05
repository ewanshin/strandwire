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
// 세 개의 위치 인자 뒤 어디에서든 `name` 다음에 오는 값을 반환한다. 없으면 nullptr.
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
    // protobuf strings must be UTF-8
    // protobuf 문자열은 UTF-8이어야 한다
    const std::string name = console::arg_to_utf8(argv[3]);

    // Logging: console only by default. --log-dir turns the daily file log on.
    // 로깅: 기본은 콘솔만. --log-dir로 일별 파일 로그를 켠다.
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
    //
    // 스레드 두 개: io_context는 백그라운드 스레드에서 소켓을 돌리고, 이 스레드는 stdin에서만 블록한다.
    // work guard는 대기 중인 비동기 작업이 없을 때(예: connect가 완료되기 전)도 io.run()을
    // 살아 있게 한다.
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
    //
    // 모든 줄은 asio::post로 io 스레드에 넘긴다. 그래서 소켓은 항상 한 스레드에서만 건드린다.
    std::cout << "type a message and press enter. 'quit' to exit." << std::endl;
    std::string line;
    while (console::read_line(line))
    { // UTF-8, also when typed into a CP949 console / CP949 콘솔에 입력해도 UTF-8이다
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
    //
    // 정상 종료: io 스레드에서 소켓을 닫고, close가 완료되면 io.run()이 반환하도록 work guard를
    // 놓은 뒤 join한다.
    asio::post(io,
               [session]
               {
                   session->close();
               });
    work.reset();
    io_thread.join();
    // before main() returns, while spdlog's statics are alive
    // main()이 반환하기 전, spdlog의 정적 객체가 살아 있는 동안
    client_log.stop();
    std::cout << "finished" << std::endl;
    return 0;
}
