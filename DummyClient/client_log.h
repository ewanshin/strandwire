#pragma once

// Module logger for DummyClient (same pattern as ServerLib/server_log.h).
// Started and shut down from main(). The final statistics line stays on std::cout because
// scripts parse it.

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
