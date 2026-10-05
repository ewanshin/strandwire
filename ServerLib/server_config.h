#pragma once

// Server configuration. The command line says who this instance is and where its environment
// file is; the file holds everything else.
//
//   command line   --ip <bind address>  --sid d.i.t.id  --config <file.json>
//   file           log_config {level, dir, console}  listen_config {port, threads, session_timeout_ms}
//
// The two never overlap, so there is no precedence to remember. An absent option or key keeps
// the built-in default. Each server has its own file schema, <server>_config.proto in ProtoLib;
// this one is lobby_config.proto.
//
// 서버 설정. 명령줄은 이 인스턴스가 누구이고 환경 파일이 어디 있는지 말한다.
// 나머지는 모두 파일에 있다.
//
//   명령줄   --ip <bind address>  --sid d.i.t.id  --config <file.json>
//   파일     log_config {level, dir, console}  listen_config {port, threads, session_timeout_ms}
//
// 둘은 겹치지 않으므로 기억할 우선순위가 없다. 없는 옵션이나 키는 내장 기본값을 유지한다.
// 서버마다 자기 파일 스키마가 있다. ProtoLib의 <server>_config.proto이고, 이 서버는
// lobby_config.proto이다.

#include <stdexcept>
#include <string>

#include "LogLib/logger.h"
#include "ServerLib/server.h"
#include "lobby_config.pb.h"

// A configuration problem the operator has to fix: unreadable file, bad JSON, unknown option,
// value out of range. The message names the value and the reason.
//
// 운영자가 고쳐야 하는 설정 문제. 읽을 수 없는 파일, 잘못된 JSON, 모르는 옵션, 범위 밖의 값.
// 메시지는 값과 이유를 이름으로 적는다.
struct config_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// What the command line said. Empty = not given.
// 명령줄이 말한 것. 비어 있으면 주어지지 않은 것이다.
struct command_line
{
    std::string ip;          // --ip
    std::string sid;         // --sid
    std::string config_path; // --config
};

// The fully loaded configuration, ready to start the logger and the server.
// 완전히 로드된 설정. 로거와 서버를 시작할 준비가 됐다.
struct server_config
{
    server_options server;
    nslog::configuration log;
    std::string config_path; // empty when no file was given / 파일이 주어지지 않았으면 비어 있다
};

// The options understood by parse_command_line(). Kept in one place for the usage text.
// parse_command_line()이 이해하는 옵션. usage 텍스트를 위해 한곳에 둔다.
extern const char* const SERVER_USAGE;

// Every option is "--name value". Throws config_error on an unknown option or a missing value.
// 모든 옵션은 "--name value" 꼴이다. 모르는 옵션이나 값이 빠지면 config_error를 던진다.
command_line parse_command_line(int argc, char* const argv[]);

// Reads and parses a JSON file against lobby_config.proto. Throws config_error.
// JSON 파일을 읽어 lobby_config.proto에 맞춰 파싱한다. config_error를 던진다.
config::lobby_config read_config_file(const std::string& path);

// parse_command_line, read_config_file if --config was given, then validate. Throws config_error.
// parse_command_line, --config가 주어졌으면 read_config_file, 그 다음 검증. config_error를 던진다.
server_config load_config(int argc, char* const argv[]);

// Validates both halves and builds the config. Throws config_error listing every bad value.
// 두 쪽을 모두 검증하고 설정을 만든다. 잘못된 값을 모두 나열한 config_error를 던진다.
server_config load_config(const command_line& cli, const config::lobby_config& file);

// One line for the start-up log, e.g. "ip=0.0.0.0 port=10000 threads=8 sid=0.0.11.1 timeout=5000ms log=info".
// 시작 로그용 한 줄, 예: "ip=0.0.0.0 port=10000 threads=8 sid=0.0.11.1 timeout=5000ms log=info".
std::string describe(const server_config& c);
