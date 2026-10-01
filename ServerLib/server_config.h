#pragma once

// Server configuration. The command line says who this instance is and where its environment
// file is; the file holds everything else.
//
//   command line   --ip <bind address>  --sid d.i.t.id  --config <file.json>
//   file           port, threads, session_timeout_ms, log.level, log.dir
//
// The two never overlap, so there is no precedence to remember. An absent option or key keeps
// the built-in default. Each server has its own file schema, <server>_config.proto in ProtoLib;
// this one is lobby_config.proto.

#include <stdexcept>
#include <string>

#include "LogLib/logger.h"
#include "ServerLib/server.h"
#include "lobby_config.pb.h"

// A configuration problem the operator has to fix: unreadable file, bad JSON, unknown option,
// value out of range. The message names the value and the reason.
struct config_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// What the command line said. Empty = not given.
struct command_line
{
    std::string ip;          // --ip
    std::string sid;         // --sid
    std::string config_path; // --config
};

// The fully loaded configuration, ready to start the logger and the server.
struct server_config
{
    server_options server;
    nslog::configuration log;
    std::string config_path; // empty when no file was given
};

// The options understood by parse_command_line(). Kept in one place for the usage text.
extern const char* const SERVER_USAGE;

// Every option is "--name value". Throws config_error on an unknown option or a missing value.
command_line parse_command_line(int argc, char* const argv[]);

// Reads and parses a JSON file against lobby_config.proto. Throws config_error.
config::lobby_config read_config_file(const std::string& path);

// parse_command_line, read_config_file if --config was given, then validate. Throws config_error.
server_config load_config(int argc, char* const argv[]);

// Validates both halves and builds the config. Throws config_error listing every bad value.
server_config load_config(const command_line& cli, const config::lobby_config& file);

// One line for the start-up log, e.g. "ip=0.0.0.0 port=10000 threads=8 sid=0.0.11.1 timeout=5000ms log=info".
std::string describe(const server_config& c);
