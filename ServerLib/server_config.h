#pragma once

// Server configuration: built-in defaults, overridden by a JSON config file, overridden by the
// command line. The file schema is ProtoLib/server_config.proto; presence of a field decides
// whether it overrides, so an absent field never clobbers a lower layer.

#include <stdexcept>
#include <string>

#include "LogLib/logger.h"
#include "ServerLib/server.h"
#include "server_config.pb.h"

// A configuration problem the operator has to fix: unreadable file, bad JSON, unknown option,
// value out of range. The message names the value and the reason.
struct config_error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// The fully loaded configuration, ready to start the logger and the server.
struct server_config
{
    server_options server;
    nslog::configuration log;
    std::string config_path; // empty when no file was given
};

// Options understood by apply_command_line(). Kept in one place for the usage text.
extern const char* const SERVER_USAGE;

// Reads and parses a JSON file. Throws config_error.
config::server_config read_config_file(const std::string& path);

// Applies "--name value" options on top of `out`. "--config <file>" is returned through
// `config_path` instead of being applied. Throws config_error on an unknown option or a missing value.
void apply_command_line(int argc, char* const argv[], config::server_config& out, std::string& config_path);

// Defaults <- file (if config_path is set) <- command line, then validation. Throws config_error.
server_config load_config(int argc, char* const argv[]);

// Validates and converts one merged config. Throws config_error listing every bad value.
server_config load_config(const config::server_config& merged, std::string config_path);

// One line for the start-up log, e.g. "port=10000 threads=8 sid=0.0.11.1 timeout=5000ms log=info".
std::string describe(const server_config& s);
