#include "LogLib/logger.h"

#include <cstdio>
#include <filesystem>
#include <vector>

// spdlog is used header-only and only in this translation unit, so its options are set here
// rather than in two build systems.
#ifdef _WIN32
// Write to the Windows console with WriteConsoleW after converting from UTF-8. Log text (player
// names, chat) is UTF-8; this makes it display correctly whatever the console code page is.
#define SPDLOG_WCHAR_TO_UTF8_SUPPORT
#define SPDLOG_UTF8_TO_WCHAR_CONSOLE
#endif
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace nslog
{

namespace
{

spdlog::level::level_enum to_spdlog(level lv)
{
    switch (lv)
    {
    case level::trace:
        return spdlog::level::trace;
    case level::debug:
        return spdlog::level::debug;
    case level::info:
        return spdlog::level::info;
    case level::warn:
        return spdlog::level::warn;
    case level::error:
        return spdlog::level::err;
    case level::fatal:
        return spdlog::level::critical;
    case level::off:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

level parse_level(const std::string& text, level fallback)
{
    if (text == "trace")
        return level::trace;
    if (text == "debug")
        return level::debug;
    if (text == "info")
        return level::info;
    if (text == "warn")
        return level::warn;
    if (text == "error")
        return level::error;
    if (text == "fatal")
        return level::fatal;
    if (text == "off")
        return level::off;
    return fallback;
}

const char* to_string(level lv)
{
    switch (lv)
    {
    case level::trace:
        return "trace";
    case level::debug:
        return "debug";
    case level::info:
        return "info";
    case level::warn:
        return "warn";
    case level::error:
        return "error";
    case level::fatal:
        return "fatal";
    case level::off:
        return "off";
    }
    return "?";
}

logger::logger() = default;

logger::~logger()
{
    // Normal flow: main() already called stop() and both pointers are empty.
    // Fallback: release without flushing, thread pool first so its
    // worker drains and joins before the logger goes away.
    threshold_.store(static_cast<int>(level::off), std::memory_order_relaxed);
    thread_pool_.reset();
    async_logger_.reset();
}

bool logger::start(const configuration& conf)
{
    stop();

    std::vector<spdlog::sink_ptr> sinks;
    try
    {
        if (conf.console)
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        if (!conf.folder_name.empty())
        {
            std::filesystem::create_directories(conf.folder_name);
            const std::string file_name = conf.folder_name + "/" + conf.module_name + ".log";
            // rotate at 00:00, keep every file
            sinks.push_back(std::make_shared<spdlog::sinks::daily_file_sink_mt>(file_name, 0, 0, false, 0));
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[nslog] cannot start logger '%s': %s\n", conf.module_name.c_str(), e.what());
        return false;
    }

    // Own thread pool (queue 8192, one worker), not the global registry's: nothing here depends
    // on spdlog's registry statics at shutdown.
    thread_pool_ = std::make_shared<spdlog::details::thread_pool>(8192, 1);
    async_logger_ = std::make_shared<spdlog::async_logger>(conf.module_name, sinks.begin(), sinks.end(), thread_pool_,
                                                           spdlog::async_overflow_policy::block);

    async_logger_->set_level(to_spdlog(conf.log_level));
    // [date time.ms][module][level letter][thread id]message
    async_logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%n][%L][%t]%v");
    async_logger_->flush_on(spdlog::level::info);

    // spdlog's default error handler locks a function-local static mutex, which may already be
    // destroyed during static destruction. Use a handler without shared state.
    async_logger_->set_error_handler(
        [](const std::string& msg)
        {
            std::fprintf(stderr, "[spdlog err] %s\n", msg.c_str());
        });

    threshold_.store(static_cast<int>(conf.log_level), std::memory_order_relaxed);
    return true;
}

void logger::flush()
{
    if (async_logger_)
        async_logger_->flush();
}

void logger::stop()
{
    threshold_.store(static_cast<int>(level::off), std::memory_order_relaxed);
    if (async_logger_)
        async_logger_->flush();
    thread_pool_.reset(); // last reference: drains the queue and joins the worker
    async_logger_.reset();
}

void logger::write(level lv, const std::string& message)
{
    // start()/stop() must not run concurrently with logging: call them from main()
    // before worker threads start and after they have been joined.
    if (async_logger_)
        async_logger_->log(to_spdlog(lv), message);
}

} // namespace nslog
