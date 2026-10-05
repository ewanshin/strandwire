#include "LogLib/logger.h"

#include <cstdio>
#include <filesystem>
#include <vector>

// spdlog is used header-only and only in this translation unit, so its options are set here
// rather than in two build systems.
//
// spdlog는 헤더 전용으로, 이 번역 단위에서만 사용한다. 그래서 옵션을 두 빌드 시스템이 아니라
// 여기서 설정한다.
#ifdef _WIN32
// Write to the Windows console with WriteConsoleW after converting from UTF-8. Log text (player
// names, chat) is UTF-8; this makes it display correctly whatever the console code page is.
//
// UTF-8에서 변환한 뒤 WriteConsoleW로 Windows 콘솔에 쓴다. 로그 텍스트(플레이어 이름, 채팅)는 UTF-8이므로
// 콘솔 코드 페이지가 무엇이든 올바르게 표시된다.
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
    //
    // 정상 흐름: main()이 이미 stop()을 호출했고 두 포인터는 비어 있다.
    // 대체 경로: 플러시 없이 해제한다. 로거가 사라지기 전에 워커가 큐를 비우고 join되도록
    // 스레드 풀을 먼저 해제한다.
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
            // 00:00에 회전하고 모든 파일을 보관한다
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
    //
    // 전역 레지스트리가 아니라 자체 스레드 풀(큐 8192, 워커 1개)을 쓴다: 종료 시점에 여기서
    // spdlog의 레지스트리 정적 객체에 의존하는 것이 없다.
    thread_pool_ = std::make_shared<spdlog::details::thread_pool>(8192, 1);
    async_logger_ = std::make_shared<spdlog::async_logger>(conf.module_name, sinks.begin(), sinks.end(), thread_pool_,
                                                           spdlog::async_overflow_policy::block);

    async_logger_->set_level(to_spdlog(conf.log_level));
    // [date time.ms][module][level letter][thread id]message
    // [날짜 시각.ms][모듈][레벨 글자][스레드 id]메시지
    async_logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%n][%L][%t]%v");
    async_logger_->flush_on(spdlog::level::info);

    // spdlog's default error handler locks a function-local static mutex, which may already be
    // destroyed during static destruction. Use a handler without shared state.
    //
    // spdlog의 기본 오류 핸들러는 함수 지역 정적 뮤텍스를 잠그는데, 정적 소멸 중에는 이미 파괴되었을 수
    // 있다. 공유 상태가 없는 핸들러를 쓴다.
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
    // last reference: drains the queue and joins the worker
    // 마지막 참조: 큐를 비우고 워커를 join한다
    thread_pool_.reset();
    async_logger_.reset();
}

void logger::write(level lv, const std::string& message)
{
    // start()/stop() must not run concurrently with logging: call them from main()
    // before worker threads start and after they have been joined.
    //
    // start()/stop()은 로깅과 동시에 실행되면 안 된다: 워커 스레드가 시작하기 전과 join된 후에
    // main()에서 호출한다.
    if (async_logger_)
        async_logger_->log(to_spdlog(lv), message);
}

} // namespace nslog
