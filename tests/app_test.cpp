// Tests for the server start-up structure: config loading (defaults <- file <- command line,
// validation) and the phase sequence of server_app (order, failure teardown, lifecycle).
//
// 서버 시작 구조의 테스트. 설정 로딩(기본값 <- 파일 <- 명령줄, 검증)과 server_app의
// 단계 순서(순서, 실패 시 해제, 수명 주기)를 다룬다.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "ServerLib/server_app.h"
#include "ServerLib/server_config.h"
#include "ServerLib/server_log.h"
#include "tests/test_support.h"

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n";                                  \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

namespace
{

// argv-style array from string literals, so the tests can call load_config() exactly as
// main() does. argv[0] is the program name.
//
// 문자열 리터럴로 만든 argv 형식 배열. 테스트가 main()과 똑같이 load_config()를 부를 수 있다.
// argv[0]은 프로그램 이름이다.
struct args
{
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    args(std::initializer_list<const char*> list)
        : storage{"app_test"}
    {
        storage.insert(storage.end(), list.begin(), list.end());
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }
    int argc() const
    {
        return static_cast<int>(ptrs.size());
    }
    char* const* argv() const
    {
        return ptrs.data();
    }
};

// Writes a throw-away config file in the OS temp folder and returns its path.
// OS 임시 폴더에 일회용 설정 파일을 쓰고 그 경로를 돌려준다.
std::string write_temp(const std::string& name, const std::string& text)
{
    const auto path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

// Runs f and returns the config_error message it threw, or "" if it did not throw.
// f를 실행하고 던져진 config_error의 메시지를 돌려준다. 던지지 않았으면 ""를 돌려준다.
template <class F>
std::string config_error_of(F&& f)
{
    try
    {
        f();
    }
    catch (const config_error& e)
    {
        return e.what();
    }
    return "";
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// No file, no options: every value is the built-in default.
// 파일도 옵션도 없다. 모든 값이 내장 기본값이다.
void test_defaults()
{
    const args a{};
    const server_config c = load_config(a.argc(), a.argv());
    CHECK(c.server.ip == "0.0.0.0");
    CHECK(c.server.port == 10000);
    CHECK(c.server.threads >= 1);
    CHECK(c.server.sid.to_string() == "0.0.11.1");
    CHECK(c.server.session_timeout.count() == 5000);
    CHECK(c.log.log_level == nslog::level::info);
    CHECK(c.log.folder_name.empty());
    CHECK(c.config_path.empty());
}

// Identity from the command line, environment from the file, defaults for what neither gives.
// 신원은 명령줄에서, 환경은 파일에서, 어느 쪽도 주지 않은 값은 기본값에서 온다.
void test_command_line_and_file()
{
    const std::string path =
        write_temp("strandwire_app_test.json",
                   R"({"listen_config": {"port": 12345, "threads": 2}, "log_config": {"level": "debug"}})");
    const args a{"--ip", "127.0.0.1", "--sid", "0.0.11.7", "--config", path.c_str()};
    const server_config c = load_config(a.argc(), a.argv());
    CHECK(c.server.ip == "127.0.0.1");               // command line / 명령줄
    CHECK(c.server.sid.to_string() == "0.0.11.7");   // command line / 명령줄
    CHECK(c.server.port == 12345);                   // file / 파일
    CHECK(c.server.threads == 2);                    // file / 파일
    CHECK(c.log.log_level == nslog::level::debug);   // file / 파일
    CHECK(c.server.session_timeout.count() == 5000); // default: the file did not set it / 기본값: 파일에 없다
    CHECK(c.log.console);                            // default / 기본값
    CHECK(c.config_path == path);
    std::filesystem::remove(path);
}

// Every kind of bad input is refused with a message that names the offending value.
// 모든 종류의 잘못된 입력을 문제의 값을 지목하는 메시지와 함께 거부한다.
void test_rejections()
{
    // the file: a bad value is named, a key the schema does not have is an error
    // 파일: 잘못된 값은 지목되고, 스키마에 없는 키는 오류다
    {
        const std::string path = write_temp("strandwire_app_test_port.json", R"({"listen_config": {"port": 70000}})");
        const args a{"--config", path.c_str()};
        const std::string e = config_error_of(
            [&]
            {
                load_config(a.argc(), a.argv());
            });
        CHECK(contains(e, "port") && contains(e, "70000"));
        std::filesystem::remove(path);
    }
    {
        const std::string path = write_temp("strandwire_app_test_threads.json", R"({"listen_config": {"threads": 0}})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "threads"));
        std::filesystem::remove(path);
    }
    {
        const std::string path = write_temp("strandwire_app_test_level.json", R"({"log_config": {"level": "loud"}})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "log_config.level"));
        std::filesystem::remove(path);
    }
    {
        // a number given as text that is not a number is a JSON error from protobuf
        // 숫자 자리에 숫자가 아닌 텍스트를 주면 protobuf의 JSON 오류다
        const std::string path = write_temp("strandwire_app_test_text.json", R"({"listen_config": {"port": "abc"}})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "config file"));
        std::filesystem::remove(path);
    }
    {
        // sid no longer belongs in the file: it is identity, given on the command line
        // sid는 더 이상 파일에 속하지 않는다. 신원이므로 명령줄로 준다
        const std::string path = write_temp("strandwire_app_test_sid.json", R"({"sid": "0.0.11.1"})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "config file"));
        std::filesystem::remove(path);
    }
    // the command line: identity values are validated, anything else is refused
    // 명령줄: 신원 값은 검증하고, 그 밖의 것은 거부한다
    {
        const args a{"--sid", "1.2.3"};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "sid"));
    }
    {
        const args a{"--ip", "999.0.0.1"};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "ip"));
    }
    {
        const args a{"--port", "10000"}; // environment belongs in the file / 환경은 파일에 속한다
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "unknown option"));
    }
    {
        const args a{"--sid"};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "missing value"));
    }
    {
        const args a{"--config", "no_such_file.json"};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "cannot be opened"));
    }
    {
        const std::string path = write_temp("strandwire_app_test_bad.json", R"({"port": 1,)");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of(
                           [&]
                           {
                               load_config(a.argc(), a.argv());
                           }),
                       "config file"));
        std::filesystem::remove(path);
    }
    {
        // a misspelt key is an error, not silently ignored
        // 철자가 틀린 키는 오류다. 조용히 무시하지 않는다
        const std::string path = write_temp("strandwire_app_test_typo.json", R"({"prot": 1})");
        const args a{"--config", path.c_str()};
        CHECK(!config_error_of(
                   [&]
                   {
                       load_config(a.argc(), a.argv());
                   })
                   .empty());
        std::filesystem::remove(path);
    }
    {
        // every problem is reported at once, from both halves
        // 모든 문제를 양쪽에서 한 번에 보고한다
        const std::string path =
            write_temp("strandwire_app_test_two.json", R"({"listen_config": {"port": 70000, "threads": 0}})");
        const args a{"--sid", "1.2.3", "--config", path.c_str()};
        const std::string e = config_error_of(
            [&]
            {
                load_config(a.argc(), a.argv());
            });
        CHECK(contains(e, "sid") && contains(e, "port") && contains(e, "threads"));
        std::filesystem::remove(path);
    }
}

// A stand-in for a database connection or an asset loader. It records nothing itself:
// server_app::trace() is the record of what came up and went down, and in which order.
//
// 데이터베이스 연결이나 에셋 로더를 대신하는 가짜. 스스로는 아무것도 기록하지 않는다.
// 무엇이 어떤 순서로 올라오고 내려갔는지는 server_app::trace()가 기록한다.
struct fake_component : server_component
{
    std::string name_;
    bool ok_;
    fake_component(std::string name, bool ok)
        : name_(std::move(name)),
          ok_(ok)
    {}
    const char* name() const override
    {
        return name_.c_str();
    }
    bool init_instance() override
    {
        return ok_;
    }
    void exit_instance() override
    {}
};

// Ephemeral port, one worker, logger silent: enough to run the phases without side effects.
// 임시 포트, 워커 하나, 로거 무음. 부작용 없이 단계를 실행하기에 충분하다.
server_config quiet_config()
{
    server_config s;
    s.server.port = 0; // ephemeral / 임시 포트
    s.server.threads = 1;
    s.log.log_level = nslog::level::off;
    s.log.console = false;
    return s;
}

// The happy path: pre_init_instance -> init_instance -> start bring every phase up in order,
// then exit_instance takes every phase down in reverse.
//
// 정상 경로: pre_init_instance -> init_instance -> start가 모든 단계를 순서대로 올리고,
// exit_instance가 모든 단계를 역순으로 내린다.
void test_lifecycle()
{
    server_app app;
    app.add_connection(std::make_unique<fake_component>("db", true));
    app.add_asset(std::make_unique<fake_component>("words", true));

    CHECK(app.pre_init_instance(quiet_config()));
    CHECK(app.init_instance());
    CHECK(app.listening()); // the port is held after init_instance ... / init_instance 뒤에는 포트를 쥔다 ...
    CHECK(!app.serving());  // ... but nothing is accepted before start / ... 그러나 start 전에는 받지 않는다
    CHECK(app.port() != 0);
    CHECK(app.start());
    CHECK(app.serving());

    app.stop();
    app.wait();
    app.exit_instance();
    CHECK(!app.serving());
    CHECK(!app.listening());

    const std::vector<std::string> expected = {
        "up:config",          "up:logger",        "up:connection:db", "up:connections",
        "up:asset:words",     "up:assets",        "up:listen",        "up:serve",
        "down:serve",         "down:listen",      "down:asset:words", "down:assets",
        "down:connection:db", "down:connections", "down:logger",      "down:config",
    };
    CHECK(app.trace() == expected);

    app.exit_instance(); // idempotent / 멱등이다
    CHECK(app.trace() == expected);
}

// The second connection fails: the first one is shut down, later phases never run, and the
// app is not listening. This is the case that showed a half-up phase must unwind itself.
//
// 두 번째 연결이 실패한다. 첫 번째는 내려가고, 뒤의 단계는 실행되지 않으며, 앱은 듣지 않는다.
// 반쯤 올라온 단계가 스스로 되감아야 한다는 것을 보여 준 사례다.
void test_failed_phase_tears_down_in_reverse()
{
    server_app app;
    app.add_connection(std::make_unique<fake_component>("db", true));
    app.add_connection(std::make_unique<fake_component>("cache", false)); // fails / 실패한다
    app.add_asset(std::make_unique<fake_component>("words", true));       // never reached / 도달하지 않는다

    CHECK(app.pre_init_instance(quiet_config()));
    CHECK(!app.init_instance()); // the failure tears everything down / 실패가 모든 것을 내린다
    CHECK(!app.listening());

    const std::vector<std::string> expected = {
        "up:config",          "up:logger",        "up:connection:db", "fail:connection:cache", "fail:connections",
        "down:connection:db", "down:connections", "down:logger",      "down:config",
    };
    CHECK(app.trace() == expected);
}

void test_failed_asset_after_connections()
{
    // connections all came up; an asset fails: assets torn down partially, then connections, logger
    // 연결은 모두 올라왔고 에셋 하나가 실패한다. 에셋은 부분적으로, 그 다음 연결과 로거가 내려간다
    server_app app;
    app.add_connection(std::make_unique<fake_component>("db", true));
    app.add_asset(std::make_unique<fake_component>("words", true));
    app.add_asset(std::make_unique<fake_component>("rooms", false));

    CHECK(app.pre_init_instance(quiet_config()));
    CHECK(!app.init_instance()); // the failure tears everything down / 실패가 모든 것을 내린다
    CHECK(!app.listening());
    CHECK(std::find(app.trace().begin(), app.trace().end(), "up:listen") == app.trace().end());

    const std::vector<std::string> expected = {
        "up:config",        "up:logger",   "up:connection:db", "up:connections", "up:asset:words",
        "fail:asset:rooms", "fail:assets", "down:asset:words", "down:assets",    "down:connection:db",
        "down:connections", "down:logger", "down:config",
    };
    CHECK(app.trace() == expected);
}

// The logger belongs to pre_init_instance: when it cannot start (the log folder is a path under a
// regular file, so create_directories throws), pre_init_instance fails, the config phase is undone
// and init_instance refuses to run. The one "[nslog] cannot start logger" line on stderr is expected.
//
// 로거는 pre_init_instance에 속한다. 로거가 시작하지 못하면(로그 폴더가 일반 파일 아래의 경로라서
// create_directories가 던진다) pre_init_instance가 실패하고, config 단계가 되돌려지며,
// init_instance는 실행을 거부한다. stderr에 "[nslog] cannot start logger" 한 줄이 찍히는 것은 정상이다.
void test_failed_logger_fails_pre_init()
{
    const std::string blocker = write_temp("strandwire_app_test_blocker", "not a folder");
    server_config s = quiet_config();
    s.log.folder_name = blocker + "/logs";

    server_app app;
    CHECK(!app.pre_init_instance(std::move(s)));
    CHECK(!app.listening());
    const std::vector<std::string> expected = {"up:config", "fail:logger", "down:logger", "down:config"};
    CHECK(app.trace() == expected);
    std::filesystem::remove(blocker);
}

// The config main() loads is what the app runs with: the sid from the command line and the port
// from the file are what the server starts with.
//
// main()이 로드한 설정 그대로 앱이 돈다. 명령줄의 sid와 파일의 포트로 서버가 시작한다.
void test_loaded_config_reaches_the_app()
{
    const std::string path =
        write_temp("strandwire_app_test_app.json",
                   R"({"listen_config": {"port": 0, "threads": 1}, "log_config": {"level": "off", "console": false}})");
    const args a{"--ip", "127.0.0.1", "--sid", "0.0.11.7", "--config", path.c_str()};
    server_config c = load_config(a.argc(), a.argv());
    std::filesystem::remove(path);
    CHECK(!c.log.console); // file / 파일

    server_app app;
    CHECK(app.pre_init_instance(std::move(c)));
    CHECK(app.init_instance());
    CHECK(app.start());
    CHECK(app.config().server.ip == "127.0.0.1");
    CHECK(app.config().server.sid.to_string() == "0.0.11.7");
    CHECK(app.config().server.threads == 1);
    CHECK(app.config().log.log_level == nslog::level::off);
    CHECK(app.port() != 0); // 0 asked for an ephemeral port and got one / 0은 임시 포트를 요청했고 받았다
    app.stop();
    app.wait();
    app.exit_instance();
}

} // namespace

int main()
{
    test_support::init();
    test_defaults();
    test_command_line_and_file();
    test_rejections();
    test_lifecycle();
    test_failed_phase_tears_down_in_reverse();
    test_failed_asset_after_connections();
    test_failed_logger_fails_pre_init();
    test_loaded_config_reaches_the_app();
    std::cout << "app_test: OK\n";
    return 0;
}
