#include "ServerLib/server_config.h"

#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <google/protobuf/util/json_util.h>

#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "common/server_define.h"

const char* const SERVER_USAGE =
    "usage: NetworkServer [--ip <address>] [--sid d.i.t.id] [--config <file.json>]\n"
    "  --ip      address to listen on (default 0.0.0.0 = every interface)\n"
    "  --sid     this server's identity, domain.idc.type.id (default 0.0.11.1)\n"
    "  --config  environment file: port, threads, session_timeout_ms, log.level, log.dir\n"
    "            (see NetworkServer/lobby_config.example.json; absent keys keep the defaults)\n";

// Only three options, all "--name value". Anything else is a typo, not a request for a default.
command_line parse_command_line(int argc, char* const argv[])
{
    command_line cli;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (i + 1 >= argc)
            throw config_error(std::string(name) + ": missing value");
        const char* value = argv[++i];

        if (name == "--ip")
            cli.ip = value;
        else if (name == "--sid")
            cli.sid = value;
        else if (name == "--config")
            cli.config_path = value;
        else
            throw config_error("unknown option '" + std::string(name) + "'");
    }
    return cli;
}

// Reads the whole file and lets protobuf parse the JSON against the lobby_config schema.
// The schema is the .proto, so a new key is one field in lobby_config.proto plus its
// validation below; no hand-written JSON code.
config::lobby_config read_config_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw config_error("config file '" + path + "' cannot be opened");
    std::stringstream text;
    text << in.rdbuf();

    config::lobby_config c;
    google::protobuf::util::JsonParseOptions options;
    // A misspelt key ("prot": 10000) must fail loudly. With the default (ignore) the operator would
    // get the built-in default and no hint why.
    options.ignore_unknown_fields = false;
    const auto status = google::protobuf::util::JsonStringToMessage(text.str(), &c, options);
    if (!status.ok())
        throw config_error("config file '" + path + "': " + status.ToString());
    return c;
}

server_config load_config(int argc, char* const argv[])
{
    const command_line cli = parse_command_line(argc, argv);
    config::lobby_config file;
    if (!cli.config_path.empty())
        file = read_config_file(cli.config_path);
    return load_config(cli, file);
}

// Applies the defaults, then overrides each with the given value if present and valid.
// Problems are collected instead of thrown one at a time, so a bad file is fixed in one round.
server_config load_config(const command_line& cli, const config::lobby_config& file)
{
    server_config c;
    c.config_path = cli.config_path;
    // Defaults not expressible in server_options' initialisers: one worker per hardware thread,
    // and the module name that appears in every log line.
    c.server.threads = std::thread::hardware_concurrency();
    c.log.module_name = "NetworkServer";

    std::vector<std::string> problems;

    // --- command line: identity -------------------------------------------------------------
    if (!cli.ip.empty()) {
        asio::error_code ec;
        asio::ip::make_address(cli.ip, ec);
        if (ec)
            problems.push_back("ip: expected an IPv4 or IPv6 address, got '" + cli.ip + "'");
        else
            c.server.ip = cli.ip;
    }
    if (!cli.sid.empty()) {
        const auto parsed = lpn::sid::parse(cli.sid);
        if (!parsed)
            problems.push_back("sid: expected domain.idc.type.id (e.g. 0.0.11.1), got '" + cli.sid + "'");
        else
            c.server.sid = *parsed;
    }

    // --- file: environment ------------------------------------------------------------------
    if (file.has_port()) {
        if (file.port() > 65535)
            problems.push_back("port: must be 0..65535, got " + std::to_string(file.port()));
        else
            c.server.port = static_cast<std::uint16_t>(file.port()); // 0 = ephemeral port, used by tests
    }
    if (file.has_threads()) {
        if (file.threads() < 1 || file.threads() > 1024)
            problems.push_back("threads: must be 1..1024, got " + std::to_string(file.threads()));
        else
            c.server.threads = file.threads();
    }
    if (file.has_session_timeout_ms()) {
        if (file.session_timeout_ms() < 1)
            problems.push_back("session_timeout_ms: must be at least 1");
        else
            c.server.session_timeout = std::chrono::milliseconds(file.session_timeout_ms());
    }
    if (file.has_log()) {
        if (file.log().has_level()) {
            // parse_level() returns the fallback for unknown text, so "off" as fallback is
            // ambiguous with a real "off": tell them apart by comparing the text.
            const nslog::level lv = nslog::parse_level(file.log().level(), nslog::level::off);
            if (lv == nslog::level::off && file.log().level() != "off")
                problems.push_back("log.level: expected trace|debug|info|warn|error|fatal|off, got '" +
                                   file.log().level() + "'");
            else
                c.log.log_level = lv;
        }
        if (file.log().has_dir())
            c.log.folder_name = file.log().dir(); // empty keeps the file log off
    }

    if (!problems.empty()) {
        std::string msg = "invalid configuration:";
        for (const auto& p : problems)
            msg += "\n  " + p;
        throw config_error(msg);
    }
    return c;
}

// The line printed before the logger starts, so an operator can see what the server is
// actually running with.
std::string describe(const server_config& c)
{
    std::string text = "ip=" + c.server.ip + " port=" + std::to_string(c.server.port) +
                       " threads=" + std::to_string(c.server.threads) + " sid=" + c.server.sid.to_string() +
                       " timeout=" + std::to_string(c.server.session_timeout.count()) + "ms log=" +
                       std::string(nslog::to_string(c.log.log_level));
    if (!c.log.folder_name.empty())
        text += " log_dir=" + c.log.folder_name;
    if (!c.config_path.empty())
        text += " config=" + c.config_path;
    return text;
}
