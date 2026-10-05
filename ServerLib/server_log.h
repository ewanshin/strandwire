#pragma once

// Module logger for the server: one logger singleton per module, reached through a macro.
//
//     server_log.info("[session ", id, "] closed: ", reason);
//
// The executable (NetworkServer, smoke_test) starts it from main() with
// server_log.start({level, "NetworkServer", folder}) and calls server_log.stop() before
// main() returns. Until it is started, everything logged is dropped.
//
// 서버의 모듈 로거. 모듈마다 로거 싱글턴 하나, 매크로로 접근한다.
//
//     server_log.info("[session ", id, "] closed: ", reason);
//
// 실행 파일(NetworkServer, smoke_test)이 main()에서 server_log.start({level, "NetworkServer", folder})로
// 시작하고 main()이 돌아오기 전에 server_log.stop()을 호출한다. 시작 전에 로그한 것은 모두 버려진다.

#include "LogLib/logger.h"

class server_logger
{
public:
    static nslog::logger& instance()
    {
        static nslog::logger logger;
        return logger;
    }
};

#define server_log server_logger::instance()
