#pragma once

// Shared by the test executables. Call test_support::init() first thing in main().
//
// - stdout/stderr are unbuffered, so the last log line before a crash is visible.
// - On Windows no dialog ever pops up: CRT assertions, abort() and unhandled exceptions are
//   written to stderr and the process exits with a non-zero code. A test must never block a
//   build or an unattended run waiting for someone to click a button.

#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#endif

namespace test_support
{

#ifdef _WIN32
inline LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info)
{
    const auto* rec = info->ExceptionRecord;
    std::fprintf(stderr, "\nFATAL: unhandled SEH exception 0x%08lX at address %p\n",
                 static_cast<unsigned long>(rec->ExceptionCode), rec->ExceptionAddress);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; // terminate without the WER dialog
}
#endif

inline void init()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(on_unhandled_exception);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT})
    {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

} // namespace test_support
