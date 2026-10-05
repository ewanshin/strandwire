#pragma once

// Module logger for NetworkClient (same pattern as ServerLib/server_log.h).
// Started and shut down from main(). What the user is meant to read as chat output (the prompt,
// "CHAT from ...") stays on std::cout; status and errors go through this logger.
//
// NetworkClient의 모듈 로거 (ServerLib/server_log.h와 같은 패턴).
// main()에서 시작하고 종료한다. 사용자가 채팅 출력으로 읽어야 하는 것(프롬프트, "CHAT from ...")은
// std::cout에 남고, 상태와 오류는 이 로거를 거친다.

#include "LogLib/logger.h"

class client_logger
{
public:
    static nslog::logger& instance()
    {
        static nslog::logger logger;
        return logger;
    }
};

#define client_log client_logger::instance()
