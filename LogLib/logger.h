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
//
// Logger 클래스: 색상 콘솔 싱크와 선택적 일별 파일을 갖춘 비동기 spdlog 로거의 얇은 래퍼이다.
//
// 인자는 하나씩 차례로 스트리밍된다. 포맷 문자열은 없다.
//
//     server_log.info("[session ", id, "] closed: ", reason);
//
// 설계 요점:
//   - 파일 싱크는 선택 사항이며 기본값은 꺼짐이다 (configuration::folder_name이 비어 있음);
//   - 콘솔 싱크는 끌 수 있다;
//   - 이 헤더의 사용자에게 spdlog는 보이지 않는다 (logger.cpp만 포함한다). 그래서 로그를 남기는
//     프로젝트는 spdlog를 include 경로에 둘 필요가 없고 컴파일 시간도 치르지 않는다;
//   - 로거는 전역 레지스트리 대신 자체 spdlog 스레드 풀을 소유한다.
//
// 수명: main()에서 start()를 한 번 호출하고 main()이 반환하기 전에 stop()을 호출한다.
// start() 전이나 stop() 후의 로그는 조용히 버려진다.

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
// "trace", "debug", "info", "warn", "error", "fatal", "off"를 파싱한다. 그 외에는 fallback을 반환한다.
level parse_level(const std::string& text, level fallback);

// The inverse of parse_level: "trace" .. "off".
// parse_level의 역함수: "trace" .. "off".
const char* to_string(level lv);

struct configuration
{
    level log_level = level::info;
    // shown in every line; also the file name stem
    // 모든 줄에 표시된다. 파일 이름의 어간이기도 하다
    std::string module_name = "app";
    bool console = true; // coloured stdout sink / 색상 있는 stdout 싱크
    // daily file sink in this folder; empty = no file log (default)
    // 이 폴더에 일별 파일 싱크를 만든다. 비어 있으면 파일 로그 없음 (기본값)
    std::string folder_name;
};

class logger
{
public:
    logger();
    ~logger();

    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    // Returns false (and logs nothing) if the file sink cannot be created. Restarting is allowed.
    // 파일 싱크를 만들 수 없으면 false를 반환한다 (아무것도 기록하지 않는다). 재시작은 허용된다.
    bool start(const configuration& conf);
    void flush();
    // Call from main() before it returns: flushes, then releases the async logger and its worker
    // thread while spdlog's internal statics are still alive. Doing it from a static destructor
    // can touch spdlog statics that were already destroyed.
    //
    // main()이 반환하기 전에 호출한다: 플러시한 뒤, spdlog의 내부 정적 객체가 아직 살아 있는 동안
    // 비동기 로거와 그 워커 스레드를 해제한다. 정적 소멸자에서 하면 이미 파괴된 spdlog 정적 객체를
    // 건드릴 수 있다.
    void stop();

    bool enabled(level lv) const noexcept
    {
        return static_cast<int>(lv) >= threshold_.load(std::memory_order_relaxed);
    }
    // True between start() and stop(). Lets callers fall back to the console outside that window.
    // start()와 stop() 사이에서 true이다. 호출자가 그 밖의 구간에서 콘솔로 대체할 수 있게 한다.
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
            return; // also true before start(): threshold_ is `off` / start() 전에도 참이다: threshold_가 `off`이므로
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
