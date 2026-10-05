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
//
// 프로세스 시작부터 "server ready"까지 일어나는 모든 일은 server_app(ServerLib/server_app.h)에 있다.
// 이 파일은 콘솔을 거기에 연결하기만 한다.
//
// 종료 코드. 0은 성공이다. 실패할 수 있는 설정 단계마다 작은 양수 코드를 하나씩 가지므로, 스크립트나
// 서비스 관리자가 로그를 읽지 않고도 구분할 수 있다. 양수인 이유: 종료 상태는 POSIX에서 8비트,
// Windows에서 부호 없는 32비트 값이라 음수는 255나 4294967295로 나타난다.
enum exit_code : int
{
    EXIT_OK = 0,
    // load_config or pre_init_instance: invalid config, or the logger did not start
    // load_config 또는 pre_init_instance: 잘못된 설정이거나 로거가 시작하지 않았다
    EXIT_CONFIG = 1,
    // init_instance: a connection, an asset or the listen socket did not come up
    // init_instance: 연결, 에셋 또는 listen 소켓이 올라오지 않았다
    EXIT_INIT = 2,
    EXIT_START = 3, // start: could not start serving / start: 서비스를 시작할 수 없었다
};

// Reading the command line is the executable's job; server_app takes a finished config. The
// three set-up steps are then called one by one so that each failure can be named here. A failed
// step has already torn down what came up, including the logger, so these messages go to
// std::cerr; server_app itself already printed the detailed reason.
//
// 명령줄을 읽는 것은 실행 파일의 일이다. server_app은 완성된 설정을 받는다. 그다음 세 설정 단계를
// 하나씩 호출하여 각 실패를 여기서 이름 지을 수 있게 한다. 실패한 단계는 로거를 포함해 올라온 것을
// 이미 해체했으므로, 이 메시지들은 std::cerr로 간다. 자세한 이유는 server_app이 이미 출력했다.
int main(int argc, char* argv[])
{
    console::init_utf8_output(); // player names in the log are UTF-8 / 로그의 플레이어 이름은 UTF-8이다

    // defaults <- --config file <- command line, validated. Every problem is reported at once.
    // 기본값 <- --config 파일 <- 명령줄, 검증 완료. 모든 문제를 한 번에 보고한다.
    server_config config;
    try
    {
        config = load_config(argc, argv);
    }
    catch (const config_error& e)
    {
        std::cerr << e.what() << '\n' << SERVER_USAGE;
        std::cerr << "[NetworkServer] invalid configuration, exit code " << EXIT_CONFIG << std::endl;
        return EXIT_CONFIG;
    }

    server_app app;

    if (!app.pre_init_instance(std::move(config)))
    {
        std::cerr << "[NetworkServer] pre_init_instance failed: the logger did not start, exit code " << EXIT_CONFIG
                  << std::endl;
        return EXIT_CONFIG;
    }
    if (!app.init_instance())
    {
        std::cerr << "[NetworkServer] init_instance failed: a connection, an asset or the listen socket did not "
                     "come up, exit code "
                  << EXIT_INIT << std::endl;
        return EXIT_INIT;
    }
    if (!app.start())
    {
        std::cerr << "[NetworkServer] start failed: could not start serving, exit code " << EXIT_START << std::endl;
        return EXIT_START;
    }

    {
        // ESC on the console stops the server cleanly, exactly like Ctrl+C / SIGTERM.
        // Inactive when stdin is not an interactive console.
        //
        // 콘솔의 ESC는 Ctrl+C / SIGTERM과 똑같이 서버를 깔끔하게 멈춘다.
        // stdin이 대화형 콘솔이 아니면 비활성이다.
        console::key_watcher esc(
            [&app]
            {
                server_log.info("ESC pressed: stopping");
                app.stop();
            });
        if (esc.active())
            server_log.info("press ESC or Ctrl+C to stop");
        app.wait();
    } // the watcher thread is joined here, before the app shuts down / 감시 스레드는 앱 종료 전에 여기서 join된다

    app.exit_instance();
    std::cout << "[NetworkServer] exited with code " << EXIT_OK << std::endl;
    return EXIT_OK;
}
