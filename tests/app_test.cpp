// Tests for the server start-up structure: config loading (defaults <- file <- command line,
// validation) and the phase sequence of server_app (order, failure teardown, lifecycle).

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

#define CHECK(cond)                                                                \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n"; \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace
{

// argv-style array from string literals, so the tests can call load_config() exactly as
// main() does. argv[0] is the program name.
struct args
{
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    args(std::initializer_list<const char*> list) : storage{"app_test"}
    {
        storage.insert(storage.end(), list.begin(), list.end());
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }
    int argc() const { return static_cast<int>(ptrs.size()); }
    char* const* argv() const { return ptrs.data(); }
};

// Writes a throw-away config file in the OS temp folder and returns its path.
std::string write_temp(const std::string& name, const std::string& text)
{
    const auto path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

// Runs f and returns the config_error message it threw, or "" if it did not throw.
template <class F>
std::string config_error_of(F&& f)
{
    try {
        f();
    } catch (const config_error& e) {
        return e.what();
    }
    return "";
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// No file, no options: every value is the built-in default.
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
void test_command_line_and_file()
{
    const std::string path = write_temp("strandwire_app_test.json",
                                        R"({"port": 12345, "threads": 2, "log": {"level": "debug"}})");
    const args a{"--ip", "127.0.0.1", "--sid", "0.0.11.7", "--config", path.c_str()};
    const server_config c = load_config(a.argc(), a.argv());
    CHECK(c.server.ip == "127.0.0.1");                 // command line
    CHECK(c.server.sid.to_string() == "0.0.11.7");     // command line
    CHECK(c.server.port == 12345);                     // file
    CHECK(c.server.threads == 2);                      // file
    CHECK(c.log.log_level == nslog::level::debug);     // file
    CHECK(c.server.session_timeout.count() == 5000);   // default: the file did not set it
    CHECK(c.config_path == path);
    std::filesystem::remove(path);
}

// Every kind of bad input is refused with a message that names the offending value.
void test_rejections()
{
    // the file: a bad value is named, a key the schema does not have is an error
    {
        const std::string path = write_temp("strandwire_app_test_port.json", R"({"port": 70000})");
        const args a{"--config", path.c_str()};
        const std::string e = config_error_of([&] { load_config(a.argc(), a.argv()); });
        CHECK(contains(e, "port") && contains(e, "70000"));
        std::filesystem::remove(path);
    }
    {
        const std::string path = write_temp("strandwire_app_test_threads.json", R"({"threads": 0})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "threads"));
        std::filesystem::remove(path);
    }
    {
        const std::string path = write_temp("strandwire_app_test_level.json", R"({"log": {"level": "loud"}})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "log.level"));
        std::filesystem::remove(path);
    }
    {
        // a number given as text that is not a number is a JSON error from protobuf
        const std::string path = write_temp("strandwire_app_test_text.json", R"({"port": "abc"})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "config file"));
        std::filesystem::remove(path);
    }
    {
        // sid no longer belongs in the file: it is identity, given on the command line
        const std::string path = write_temp("strandwire_app_test_sid.json", R"({"sid": "0.0.11.1"})");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "config file"));
        std::filesystem::remove(path);
    }
    // the command line: identity values are validated, anything else is refused
    {
        const args a{"--sid", "1.2.3"};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "sid"));
    }
    {
        const args a{"--ip", "999.0.0.1"};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "ip"));
    }
    {
        const args a{"--port", "10000"}; // environment belongs in the file
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "unknown option"));
    }
    {
        const args a{"--sid"};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "missing value"));
    }
    {
        const args a{"--config", "no_such_file.json"};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "cannot be opened"));
    }
    {
        const std::string path = write_temp("strandwire_app_test_bad.json", R"({"port": 1,)");
        const args a{"--config", path.c_str()};
        CHECK(contains(config_error_of([&] { load_config(a.argc(), a.argv()); }), "config file"));
        std::filesystem::remove(path);
    }
    {
        // a misspelt key is an error, not silently ignored
        const std::string path = write_temp("strandwire_app_test_typo.json", R"({"prot": 1})");
        const args a{"--config", path.c_str()};
        CHECK(!config_error_of([&] { load_config(a.argc(), a.argv()); }).empty());
        std::filesystem::remove(path);
    }
    {
        // every problem is reported at once, from both halves
        const std::string path = write_temp("strandwire_app_test_two.json", R"({"port": 70000, "threads": 0})");
        const args a{"--sid", "1.2.3", "--config", path.c_str()};
        const std::string e = config_error_of([&] { load_config(a.argc(), a.argv()); });
        CHECK(contains(e, "sid") && contains(e, "port") && contains(e, "threads"));
        std::filesystem::remove(path);
    }
}

// A stand-in for a database connection or an asset loader. It records nothing itself:
// server_app::trace() is the record of what came up and went down, and in which order.
struct fake_component : server_component
{
    std::string name_;
    bool ok_;
    fake_component(std::string name, bool ok) : name_(std::move(name)), ok_(ok) {}
    const char* name() const override { return name_.c_str(); }
    bool init_instance() override { return ok_; }
    void exit_instance() override {}
};

// Ephemeral port, one worker, logger silent: enough to run the phases without side effects.
server_config quiet_config()
{
    server_config s;
    s.server.port = 0; // ephemeral
    s.server.threads = 1;
    s.log.log_level = nslog::level::off;
    s.log.console = false;
    return s;
}

// The happy path: pre_init_instance -> init_instance -> start bring every phase up in order,
// then exit_instance takes every phase down in reverse.
void test_lifecycle()
{
    server_app app;
    app.add_connection(std::make_unique<fake_component>("db", true));
    app.add_asset(std::make_unique<fake_component>("words", true));

    CHECK(app.pre_init_instance(quiet_config()));
    CHECK(app.init_instance());
    CHECK(app.start());
    CHECK(app.listening());
    CHECK(app.port() != 0);

    app.stop();
    app.wait();
    app.exit_instance();
    CHECK(!app.listening());

    const std::vector<std::string> expected = {
        "up:config", "up:logger", "up:connection:db", "up:connections", "up:asset:words", "up:assets", "up:listen",
        "down:listen", "down:asset:words", "down:assets", "down:connection:db", "down:connections", "down:logger",
        "down:config",
    };
    CHECK(app.trace() == expected);

    app.exit_instance(); // idempotent
    CHECK(app.trace() == expected);
}

// The second connection fails: the first one is shut down, later phases never run, and the
// app is not listening. This is the case that showed a half-up phase must unwind itself.
void test_failed_phase_tears_down_in_reverse()
{
    server_app app;
    app.add_connection(std::make_unique<fake_component>("db", true));
    app.add_connection(std::make_unique<fake_component>("cache", false)); // fails
    app.add_asset(std::make_unique<fake_component>("words", true));       // never reached

    CHECK(app.pre_init_instance(quiet_config()));
    CHECK(!app.init_instance()); // the failure tears everything down
    CHECK(!app.listening());

    const std::vector<std::string> expected = {
        "up:config", "up:logger", "up:connection:db", "fail:connection:cache", "fail:connections",
        "down:connection:db", "down:connections", "down:logger", "down:config",
    };
    CHECK(app.trace() == expected);
}

void test_failed_asset_after_connections()
{
    // connections all came up; an asset fails: assets torn down partially, then connections, logger
    server_app app;
    app.add_connection(std::make_unique<fake_component>("db", true));
    app.add_asset(std::make_unique<fake_component>("words", true));
    app.add_asset(std::make_unique<fake_component>("rooms", false));

    CHECK(app.pre_init_instance(quiet_config()));
    CHECK(!app.init_instance()); // the failure tears everything down
    CHECK(!app.listening());
    CHECK(std::find(app.trace().begin(), app.trace().end(), "up:listen") == app.trace().end());

    const std::vector<std::string> expected = {
        "up:config", "up:logger", "up:connection:db", "up:connections", "up:asset:words", "fail:asset:rooms",
        "fail:assets", "down:asset:words", "down:assets", "down:connection:db", "down:connections",
        "down:logger", "down:config",
    };
    CHECK(app.trace() == expected);
}

// The logger belongs to pre_init_instance: when it cannot start (the log folder is a path under a
// regular file, so create_directories throws), pre_init_instance fails, the config phase is undone
// and init_instance refuses to run. The one "[nslog] cannot start logger" line on stderr is expected.
void test_failed_logger_fails_pre_init()
{
    const std::string blocker = write_temp("strandwire_app_test_blocker", "not a folder");
    server_config s = quiet_config();
    s.log.folder_name = blocker + "/logs";

    server_app app;
    CHECK(!app.pre_init_instance(std::move(s)));
    CHECK(!app.init_instance());
    CHECK(!app.listening());
    const std::vector<std::string> expected = {"up:config", "fail:logger", "down:logger", "down:config"};
    CHECK(app.trace() == expected);
    std::filesystem::remove(blocker);
}

// The config main() loads is what the app runs with: the sid from the command line and the port
// from the file are what the server starts with.
void test_loaded_config_reaches_the_app()
{
    const std::string path = write_temp("strandwire_app_test_app.json",
                                        R"({"port": 0, "threads": 1, "log": {"level": "off"}})");
    const args a{"--ip", "127.0.0.1", "--sid", "0.0.11.7", "--config", path.c_str()};
    server_config c = load_config(a.argc(), a.argv());
    std::filesystem::remove(path);
    c.log.console = false;

    server_app app;
    CHECK(app.pre_init_instance(std::move(c)));
    CHECK(app.init_instance());
    CHECK(app.start());
    CHECK(app.config().server.ip == "127.0.0.1");
    CHECK(app.config().server.sid.to_string() == "0.0.11.7");
    CHECK(app.config().server.threads == 1);
    CHECK(app.config().log.log_level == nslog::level::off);
    CHECK(app.port() != 0); // 0 asked for an ephemeral port and got one
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
