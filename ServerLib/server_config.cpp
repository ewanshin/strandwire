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
    "  --config  environment file with the sections log_config {level, dir, console} and\n"
    "            listen_config {port, threads, session_timeout_ms}; absent keys keep the defaults\n"
    "            (see NetworkServer/lobby_config.example.json)\n";

// Only three options, all "--name value". Anything else is a typo, not a request for a default.
// 옵션은 셋뿐이고 모두 "--name value" 꼴이다. 그 밖의 것은 오타이지 기본값 요청이 아니다.
command_line parse_command_line(int argc, char* const argv[])
{
    command_line cli;
    for (int i = 1; i < argc; ++i)
    {
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
//
// 파일 전체를 읽고 protobuf가 lobby_config 스키마에 맞춰 JSON을 파싱하게 한다.
// 스키마가 .proto이므로 새 키는 lobby_config.proto의 필드 하나와 아래의 검증 하나로 끝난다.
// 손으로 쓴 JSON 코드는 없다.
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
    //
    // 잘못 쓴 키("prot": 10000)는 크게 실패해야 한다. 기본값(무시)이면 운영자는 내장 기본값을
    // 얻고 이유는 전혀 알 수 없다.
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
//
// 기본값을 적용한 뒤, 주어진 값이 있고 유효하면 각각 덮어쓴다.
// 문제는 하나씩 던지지 않고 모아 둔다. 그래야 잘못된 파일을 한 번에 고친다.
server_config load_config(const command_line& cli, const config::lobby_config& file)
{
    server_config c;
    c.config_path = cli.config_path;
    // Defaults not expressible in server_options' initialisers: one worker per hardware thread,
    // and the module name that appears in every log line.
    //
    // server_options의 초기화자로 표현할 수 없는 기본값. 하드웨어 스레드당 워커 하나, 그리고 모든
    // 로그 줄에 나타나는 모듈 이름.
    c.server.threads = std::thread::hardware_concurrency();
    c.log.module_name = "NetworkServer";

    std::vector<std::string> problems;

    // --- command line: identity -------------------------------------------------------------
    // --- 명령줄: 인스턴스 신원 ---------------------------------------------------------------
    if (!cli.ip.empty())
    {
        asio::error_code ec;
        asio::ip::make_address(cli.ip, ec);
        if (ec)
            problems.push_back("ip: expected an IPv4 or IPv6 address, got '" + cli.ip + "'");
        else
            c.server.ip = cli.ip;
    }
    if (!cli.sid.empty())
    {
        const auto parsed = lpn::sid::parse(cli.sid);
        if (!parsed)
            problems.push_back("sid: expected domain.idc.type.id (e.g. 0.0.11.1), got '" + cli.sid + "'");
        else
            c.server.sid = *parsed;
    }

    // --- file, section listen_config --------------------------------------------------------
    // --- 파일, listen_config 섹션 ------------------------------------------------------------
    if (file.has_listen_config())
    {
        const config::listen_config& l = file.listen_config();
        if (l.has_port())
        {
            if (l.port() > 65535)
                problems.push_back("listen_config.port: must be 0..65535, got " + std::to_string(l.port()));
            else
                // 0 = ephemeral port, used by tests
                // 0 = 임시 포트, 테스트가 쓴다
                c.server.port = static_cast<std::uint16_t>(l.port());
        }
        if (l.has_threads())
        {
            if (l.threads() < 1 || l.threads() > 1024)
                problems.push_back("listen_config.threads: must be 1..1024, got " + std::to_string(l.threads()));
            else
                c.server.threads = l.threads();
        }
        if (l.has_session_timeout_ms())
        {
            if (l.session_timeout_ms() < 1)
                problems.push_back("listen_config.session_timeout_ms: must be at least 1");
            else
                c.server.session_timeout = std::chrono::milliseconds(l.session_timeout_ms());
        }
    }

    // --- file, section log_config -----------------------------------------------------------
    // --- 파일, log_config 섹션 ---------------------------------------------------------------
    if (file.has_log_config())
    {
        const config::log_config& l = file.log_config();
        if (l.has_level())
        {
            // parse_level() returns the fallback for unknown text, so "off" as fallback is
            // ambiguous with a real "off": tell them apart by comparing the text.
            //
            // parse_level()은 모르는 텍스트에 대해 대체값을 돌려주므로, 대체값 "off"는 진짜 "off"와
            // 구별되지 않는다. 텍스트를 비교해서 가른다.
            const nslog::level lv = nslog::parse_level(l.level(), nslog::level::off);
            if (lv == nslog::level::off && l.level() != "off")
                problems.push_back("log_config.level: expected trace|debug|info|warn|error|fatal|off, got '" +
                                   l.level() + "'");
            else
                c.log.log_level = lv;
        }
        if (l.has_dir())
            c.log.folder_name = l.dir(); // empty keeps the file log off / 비어 있으면 파일 로그는 꺼진 채다
        if (l.has_console())
            c.log.console = l.console();
    }

    if (!problems.empty())
    {
        std::string msg = "invalid configuration:";
        for (const auto& p : problems)
            msg += "\n  " + p;
        throw config_error(msg);
    }
    return c;
}

// The line printed before the logger starts, so an operator can see what the server is
// actually running with.
//
// 로거가 시작되기 전에 출력하는 줄. 운영자가 서버가 실제로 무엇으로 도는지 볼 수 있다.
std::string describe(const server_config& c)
{
    std::string text = "ip=" + c.server.ip + " port=" + std::to_string(c.server.port) +
                       " threads=" + std::to_string(c.server.threads) + " sid=" + c.server.sid.to_string() +
                       " timeout=" + std::to_string(c.server.session_timeout.count()) +
                       "ms log=" + std::string(nslog::to_string(c.log.log_level));
    if (!c.log.folder_name.empty())
        text += " log_dir=" + c.log.folder_name;
    if (!c.log.console)
        text += " console=off";
    if (!c.config_path.empty())
        text += " config=" + c.config_path;
    return text;
}
