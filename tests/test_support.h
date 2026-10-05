#pragma once

// Shared by the test executables. Call test_support::init() first thing in main().
//
// - stdout/stderr are unbuffered, so the last log line before a crash is visible.
// - On Windows no dialog ever pops up: CRT assertions, abort() and unhandled exceptions are
//   written to stderr and the process exits with a non-zero code. A test must never block a
//   build or an unattended run waiting for someone to click a button.
//
// 테스트 실행 파일이 공유한다. main()의 첫 줄에서 test_support::init()을 부른다.
//
// - stdout/stderr는 버퍼링하지 않으므로 크래시 직전의 마지막 로그 줄이 보인다.
// - Windows에서는 어떤 대화 상자도 뜨지 않는다. CRT 단언, abort(), 처리되지 않은 예외는
//   stderr에 쓰고 프로세스는 0이 아닌 코드로 끝난다. 테스트가 누군가 버튼을 누르기를 기다리며
//   빌드나 무인 실행을 막아서는 안 된다.

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
    return EXCEPTION_EXECUTE_HANDLER; // terminate without the WER dialog / WER 대화 상자 없이 종료한다
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
