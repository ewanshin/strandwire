#pragma once

// Module logger for DummyClient (same pattern as ServerLib/server_log.h).
// Started and shut down from main(). The final statistics line stays on std::cout because
// scripts parse it.
//
// DummyClient의 모듈 로거 (ServerLib/server_log.h와 같은 패턴).
// main()에서 시작하고 종료한다. 마지막 통계 줄은 스크립트가 파싱하므로 std::cout에 남는다.

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
