#pragma once

// Module logger for the server: one logger singleton per module, reached through a macro.
//
//     server_log.info("[session ", id, "] closed: ", reason);
//
// The executable (NetworkServer, smoke_test) starts it from main() with
// server_log.start({level, "NetworkServer", folder}) and calls server_log.stop() before
// main() returns. Until it is started, everything logged is dropped.

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
