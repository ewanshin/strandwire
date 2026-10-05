#pragma once

// Console text I/O in UTF-8 for the executables' main(). Every string that goes into a protobuf
// `string` field must be UTF-8, but a Windows console hands out bytes in the active code page
// (CP949 on a Korean system), so typed Korean text reached protobuf as invalid UTF-8.
//
// This header is one of the few places with platform-specific code. It is included only by
// main.cpp files, never by library code.
//
// 실행 파일의 main()을 위한 UTF-8 콘솔 텍스트 입출력이다. protobuf `string` 필드에 들어가는 모든 문자열은
// UTF-8이어야 하지만, Windows 콘솔은 활성 코드 페이지(한국어 시스템에서는 CP949)의 바이트를 넘겨주므로
// 입력한 한글이 잘못된 UTF-8로 protobuf에 도달했다.
//
// 이 헤더는 플랫폼별 코드가 있는 몇 안 되는 곳 중 하나다. main.cpp 파일만 포함하며 라이브러리 코드는
// 포함하지 않는다.

#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <conio.h>
#include <cstdlib>
#include <windows.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace console
{

#ifdef _WIN32
namespace detail
{

inline UINT& saved_output_cp()
{
    static UINT cp = 0;
    return cp;
}

inline void restore_output_cp()
{
    if (saved_output_cp() != 0)
        SetConsoleOutputCP(saved_output_cp());
}

inline std::string to_utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace detail
#endif

// Makes UTF-8 bytes written to stdout/stderr display correctly. On Windows the console output
// code page is switched to UTF-8 and restored at exit, because the setting outlives the process.
// Call once at the start of main().
//
// stdout/stderr에 쓴 UTF-8 바이트가 올바르게 표시되게 한다. Windows에서는 콘솔 출력 코드 페이지를 UTF-8로
// 바꾸고 종료 시 되돌린다. 이 설정은 프로세스보다 오래 남기 때문이다.
// main() 시작 시 한 번 호출한다.
inline void init_utf8_output()
{
#ifdef _WIN32
    const UINT current = GetConsoleOutputCP();
    if (current != 0 && current != CP_UTF8)
    {
        detail::saved_output_cp() = current;
        if (SetConsoleOutputCP(CP_UTF8))
            std::atexit(detail::restore_output_cp);
        else
            detail::saved_output_cp() = 0;
    }
#endif
}

// Converts a command-line argument (argv[i]) to UTF-8. On Windows narrow argv is in the ANSI
// code page (CP949 on a Korean system); elsewhere it is already UTF-8.
//
// 명령줄 인자(argv[i])를 UTF-8로 변환한다. Windows에서 narrow argv는 ANSI 코드 페이지(한국어 시스템에서는
// CP949)이고, 다른 OS에서는 이미 UTF-8이다.
inline std::string arg_to_utf8(const char* arg)
{
#ifdef _WIN32
    const int len = static_cast<int>(std::strlen(arg));
    if (len == 0)
        return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, arg, len, nullptr, 0);
    if (n <= 0)
        return arg;
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, arg, len, w.data(), n);
    return detail::to_utf8(w);
#else
    return arg;
#endif
}

// Reads one line from stdin as UTF-8, without the line terminator. Returns false at end of input.
// - Windows, stdin is a real console: read UTF-16 with ReadConsoleW and convert. (Switching the
//   input code page to UTF-8 instead is unreliable: older conhost returns NUL for non-ASCII input.)
// - stdin is a pipe or a file, or any other OS: bytes are taken as they are and assumed to be UTF-8.
//
// stdin에서 한 줄을 UTF-8로 읽는다. 줄 끝 문자는 제외한다. 입력이 끝나면 false를 반환한다.
// - Windows이고 stdin이 실제 콘솔인 경우: ReadConsoleW로 UTF-16을 읽어 변환한다. (대신 입력 코드 페이지를
//   UTF-8로 바꾸는 방법은 믿을 수 없다. 오래된 conhost는 비ASCII 입력에 NUL을 돌려준다.)
// - stdin이 파이프나 파일인 경우, 또는 다른 OS: 바이트를 그대로 받아 UTF-8로 간주한다.
inline bool read_line(std::string& out)
{
    out.clear();
#ifdef _WIN32
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
    {
        std::wstring line;
        wchar_t buf[512];
        for (;;)
        {
            DWORD read = 0;
            if (!ReadConsoleW(h, buf, static_cast<DWORD>(std::size(buf)), &read, nullptr) || read == 0)
            {
                if (line.empty())
                    return false;
                break;
            }
            line.append(buf, read);
            if (line.back() == L'\n')
                break;
        }
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r'))
            line.pop_back();
        if (!line.empty() && line.front() == 0x1A) // Ctrl+Z / Ctrl+Z (입력 종료)
            return false;
        out = detail::to_utf8(line);
        return true;
    }
#endif
    if (!std::getline(std::cin, out))
        return false;
    if (!out.empty() && out.back() == '\r')
        out.pop_back();
    return true;
}

// Watches the console for the ESC key on a background thread and calls on_escape once.
// Used by the server for a clean shutdown (same path as Ctrl+C: server::stop()).
//
// Does nothing when stdin is not an interactive console (service, pipe, test run), so it is safe
// to create unconditionally. The destructor stops the thread and, on POSIX, restores the terminal.
// on_escape runs on the watcher thread: it must be thread-safe (server::stop() is).
//
// 백그라운드 스레드에서 콘솔의 ESC 키를 감시하고 on_escape를 한 번 호출한다.
// 서버가 깨끗하게 종료하는 데 쓴다 (Ctrl+C와 같은 경로: server::stop()).
//
// stdin이 대화형 콘솔이 아니면(서비스, 파이프, 테스트 실행) 아무것도 하지 않으므로 조건 없이 생성해도
// 안전하다. 소멸자는 스레드를 멈추고, POSIX에서는 터미널을 되돌린다.
// on_escape는 감시 스레드에서 실행되므로 스레드 안전해야 한다 (server::stop()은 그렇다).
class key_watcher
{
public:
    explicit key_watcher(std::function<void()> on_escape)
        : on_escape_(std::move(on_escape))
    {
        if (!prepare())
            return;
        active_ = true;
        thread_ = std::thread(
            [this]
            {
                poll_loop();
            });
    }

    ~key_watcher()
    {
        stop_.store(true);
        if (thread_.joinable())
            thread_.join();
        restore();
    }

    key_watcher(const key_watcher&) = delete;
    key_watcher& operator=(const key_watcher&) = delete;

    bool active() const noexcept
    {
        return active_;
    }

private:
    static constexpr int ESC = 0x1B;

    void poll_loop()
    {
        while (!stop_.load())
        {
            if (escape_pressed())
            {
                on_escape_();
                return;
            }
        }
    }

#ifdef _WIN32
    bool prepare()
    {
        const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
    }

    void restore()
    {}

    // Polls every 50 ms. _getch() does not echo and does not wait for Enter.
    // 50 ms마다 폴링한다. _getch()는 에코하지 않고 Enter를 기다리지 않는다.
    bool escape_pressed()
    {
        if (!_kbhit())
        {
            Sleep(50);
            return false;
        }
        const int c = _getch();
        if (c == 0 || c == 0xE0)
        { // function / arrow key: a second code follows / 기능 키나 화살표 키: 코드가 하나 더 따라온다
            (void)_getch();
            return false;
        }
        return c == ESC;
    }
#else
    bool prepare()
    {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved_) != 0)
            return false;
        termios raw = saved_;
        // no line buffering, no echo
        // 줄 버퍼링 없음, 에코 없음
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        restore_needed_ = (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0);
        return restore_needed_;
    }

    void restore()
    {
        if (restore_needed_)
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        restore_needed_ = false;
    }

    // Arrow and function keys arrive as ESC followed by more bytes. A lone ESC is the key itself.
    // 화살표 키와 기능 키는 ESC 뒤에 바이트가 더 붙어서 온다. ESC 하나만 오면 ESC 키 자체다.
    bool escape_pressed()
    {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        if (::poll(&pfd, 1, 50) <= 0)
            return false;
        char c = 0;
        if (::read(STDIN_FILENO, &c, 1) != 1 || c != ESC)
            return false;
        if (::poll(&pfd, 1, 20) > 0)
        { // escape sequence: swallow it / 이스케이프 시퀀스: 버린다
            char rest[8];
            (void)::read(STDIN_FILENO, rest, sizeof(rest));
            return false;
        }
        return true;
    }

    termios saved_{};
    bool restore_needed_ = false;
#endif

    std::function<void()> on_escape_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    bool active_ = false;
};

} // namespace console
