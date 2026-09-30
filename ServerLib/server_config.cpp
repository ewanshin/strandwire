#include "ServerLib/server_config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <google/protobuf/util/json_util.h>

#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "common/server_define.h"

const char* const SERVER_USAGE =
    "usage: NetworkServer [--config <file.json>] [--port N] [--threads N] [--sid d.i.t.id]\n"
    "                     [--timeout-ms N] [--log-level LEVEL] [--log-dir DIR]\n"
    "  command-line options override the config file, which overrides the defaults\n"
    "  LEVEL: trace|debug|info|warn|error|fatal|off\n";

namespace
{

// Strict decimal parse for numeric options: the whole string must be digits. std::atoi would
// silently turn "12abc" into 12 and "abc" into 0, hiding typos.
unsigned parse_unsigned(std::string_view option, const char* text)
{
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0')
        throw config_error(std::string(option) + ": expected a number, got '" + text + "'");
    if (v > 0xFFFFFFFFul)
        throw config_error(std::string(option) + ": value too large");
    return static_cast<unsigned>(v);
}

} // namespace

// Reads the whole file and lets protobuf parse the JSON against the server_config schema.
// The schema is the .proto, so a new setting is one field in server_config.proto plus its
// validation below; no hand-written JSON code.
config::server_config load_config_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw config_error("config file '" + path + "' cannot be opened");
    std::stringstream text;
    text << in.rdbuf();

    config::server_config c;
    google::protobuf::util::JsonParseOptions options;
    // A misspelt key ("prot": 10000) must fail loudly. With the default (ignore) the operator would
    // get the built-in default and no hint why.
    options.ignore_unknown_fields = false;
    const auto status = google::protobuf::util::JsonStringToMessage(text.str(), &c, options);
    if (!status.ok())
        throw config_error("config file '" + path + "': " + status.ToString());
    return c;
}

// Every option is "--name value". Setting a field on the proto message (rather than on the final
// settings) is what makes the precedence work: MergeFrom() later copies only the fields that were
// actually given. "--config" is special: it names the file the other options will be merged onto.
void apply_command_line(int argc, char* const argv[], config::server_config& out, std::string& config_path)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (i + 1 >= argc)
            throw config_error(std::string(name) + ": missing value");
        const char* value = argv[++i];

        if (name == "--config")
            config_path = value;
        else if (name == "--port")
            out.set_port(parse_unsigned(name, value));
        else if (name == "--threads")
            out.set_threads(parse_unsigned(name, value));
        else if (name == "--sid")
            out.set_sid(value);
        else if (name == "--timeout-ms")
            out.set_session_timeout_ms(parse_unsigned(name, value));
        else if (name == "--log-level")
            out.mutable_log()->set_level(value);
        else if (name == "--log-dir")
            out.mutable_log()->set_dir(value);
        else
            throw config_error("unknown option '" + std::string(name) + "'");
    }
}

// The three layers, lowest first: built-in defaults (applied in the overload below), the config
// file, the command line. Each layer is a server_config with only the fields it sets.
server_settings resolve_settings(int argc, char* const argv[])
{
    config::server_config from_cli;
    std::string config_path;
    apply_command_line(argc, argv, from_cli, config_path);

    config::server_config merged;
    if (!config_path.empty())
        merged = load_config_file(config_path);
    // proto2 MergeFrom copies only fields that are set on the source, so an option that was not
    // given leaves the file's value (or the default) alone. Nested `log` merges field by field too.
    merged.MergeFrom(from_cli);
    return resolve_settings(merged, config_path);
}

// Applies the defaults, then overrides each with the merged value if present and valid.
// Problems are collected instead of thrown one at a time, so a bad file is fixed in one round.
server_settings resolve_settings(const config::server_config& c, std::string config_path)
{
    server_settings s;
    s.config_path = std::move(config_path);
    // Defaults not expressible in server_options' initialisers: one worker per hardware thread,
    // and the module name that appears in every log line.
    s.server.threads = std::thread::hardware_concurrency();
    s.log.module_name = "NetworkServer";

    std::vector<std::string> problems;

    if (c.has_port()) {
        if (c.port() > 65535)
            problems.push_back("port: must be 0..65535, got " + std::to_string(c.port()));
        else
            s.server.port = static_cast<std::uint16_t>(c.port()); // 0 = ephemeral port, used by tests
    }
    if (c.has_threads()) {
        if (c.threads() < 1 || c.threads() > 1024)
            problems.push_back("threads: must be 1..1024, got " + std::to_string(c.threads()));
        else
            s.server.threads = c.threads();
    }
    if (c.has_sid()) {
        const auto parsed = lpn::sid::parse(c.sid());
        if (!parsed)
            problems.push_back("sid: expected domain.idc.type.id (e.g. 0.0.11.1), got '" + c.sid() + "'");
        else
            s.server.sid = *parsed;
    }
    if (c.has_session_timeout_ms()) {
        if (c.session_timeout_ms() < 1)
            problems.push_back("session_timeout_ms: must be at least 1");
        else
            s.server.session_timeout = std::chrono::milliseconds(c.session_timeout_ms());
    }
    if (c.has_log()) {
        if (c.log().has_level()) {
            // parse_level() returns the fallback for unknown text, so "off" as fallback is
            // ambiguous with a real "off": tell them apart by comparing the text.
            const nslog::level lv = nslog::parse_level(c.log().level(), nslog::level::off);
            if (lv == nslog::level::off && c.log().level() != "off")
                problems.push_back("log.level: expected trace|debug|info|warn|error|fatal|off, got '" +
                                   c.log().level() + "'");
            else
                s.log.log_level = lv;
        }
        if (c.log().has_dir())
            s.log.folder_name = c.log().dir(); // empty keeps the file log off
    }

    if (!problems.empty()) {
        std::string msg = "invalid configuration:";
        for (const auto& p : problems)
            msg += "\n  " + p;
        throw config_error(msg);
    }
    return s;
}

// The line logged right after the logger starts, so an operator can see what the server is
// actually running with after all three layers were merged.
std::string describe(const server_settings& s)
{
    std::string text = "port=" + std::to_string(s.server.port) + " threads=" + std::to_string(s.server.threads) +
                       " sid=" + s.server.sid.to_string() + " timeout=" +
                       std::to_string(s.server.session_timeout.count()) + "ms log=" +
                       std::string(nslog::to_string(s.log.log_level));
    if (!s.log.folder_name.empty())
        text += " log_dir=" + s.log.folder_name;
    if (!s.config_path.empty())
        text += " config=" + s.config_path;
    return text;
}
