#pragma once

// Console text I/O in UTF-8 for the executables' main(). Every string that goes into a protobuf
// `string` field must be UTF-8, but a Windows console hands out bytes in the active code page
// (CP949 on a Korean system), so typed Korean text reached protobuf as invalid UTF-8.
//
// This header is one of the few places with platform-specific code. It is included only by
// main.cpp files, never by library code.

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
        if (!line.empty() && line.front() == 0x1A) // Ctrl+Z
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
    bool escape_pressed()
    {
        if (!_kbhit())
        {
            Sleep(50);
            return false;
        }
        const int c = _getch();
        if (c == 0 || c == 0xE0)
        { // function / arrow key: a second code follows
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
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO)); // no line buffering, no echo
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
    bool escape_pressed()
    {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        if (::poll(&pfd, 1, 50) <= 0)
            return false;
        char c = 0;
        if (::read(STDIN_FILENO, &c, 1) != 1 || c != ESC)
            return false;
        if (::poll(&pfd, 1, 20) > 0)
        { // escape sequence: swallow it
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
