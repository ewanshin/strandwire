#pragma once

// Logger class: a thin wrapper around an asynchronous spdlog logger with a coloured console sink
// and an optional daily file.
//
// Arguments are streamed one after another; there is no format string.
//
//     server_log.info("[session ", id, "] closed: ", reason);
//
// Design points:
//   - the file sink is optional and OFF by default (configuration::folder_name empty);
//   - the console sink can be switched off;
//   - spdlog is not visible to users of this header (only logger.cpp includes it), so projects
//     that log do not need spdlog on their include path and do not pay its compile time;
//   - the logger owns its own spdlog thread pool instead of the global registry's.
//
// Lifetime: call start() once from main() and stop() before main() returns. Logging
// before start() or after stop() is silently dropped.

#include <atomic>
#include <memory>
#include <sstream>
#include <string>

namespace spdlog
{
class async_logger;
namespace details
{
class thread_pool;
}
} // namespace spdlog

namespace nslog
{

enum class level : int
{
    trace = 0,
    debug,
    info,
    warn,
    error,
    fatal,
    off,
};

// Parses "trace", "debug", "info", "warn", "error", "fatal", "off". Returns fallback otherwise.
level parse_level(const std::string& text, level fallback);

// The inverse of parse_level: "trace" .. "off".
const char* to_string(level lv);

struct configuration
{
    level log_level = level::info;
    std::string module_name = "app"; // shown in every line; also the file name stem
    bool console = true;             // coloured stdout sink
    std::string folder_name;         // daily file sink in this folder; empty = no file log (default)
};

class logger
{
public:
    logger();
    ~logger();

    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    // Returns false (and logs nothing) if the file sink cannot be created. Restarting is allowed.
    bool start(const configuration& conf);
    void flush();
    // Call from main() before it returns: flushes, then releases the async logger and its worker
    // thread while spdlog's internal statics are still alive. Doing it from a static destructor
    // can touch spdlog statics that were already destroyed.
    void stop();

    bool enabled(level lv) const noexcept
    {
        return static_cast<int>(lv) >= threshold_.load(std::memory_order_relaxed);
    }
    // True between start() and stop(). Lets callers fall back to the console outside that window.
    bool running() const noexcept
    {
        return async_logger_ != nullptr;
    }

    template <class... Args>
    void trace(const Args&... args)
    {
        log(level::trace, args...);
    }
    template <class... Args>
    void debug(const Args&... args)
    {
        log(level::debug, args...);
    }
    template <class... Args>
    void info(const Args&... args)
    {
        log(level::info, args...);
    }
    template <class... Args>
    void warn(const Args&... args)
    {
        log(level::warn, args...);
    }
    template <class... Args>
    void error(const Args&... args)
    {
        log(level::error, args...);
    }
    template <class... Args>
    void fatal(const Args&... args)
    {
        log(level::fatal, args...);
    }

private:
    template <class... Args>
    void log(level lv, const Args&... args)
    {
        if (!enabled(lv))
            return; // also true before start(): threshold_ is `off`
        std::ostringstream stream;
        (stream << ... << args);
        write(lv, stream.str());
    }

    void write(level lv, const std::string& message);

    std::atomic<int> threshold_{static_cast<int>(level::off)};
    std::shared_ptr<spdlog::details::thread_pool> thread_pool_;
    std::shared_ptr<spdlog::async_logger> async_logger_;
};

} // namespace nslog
