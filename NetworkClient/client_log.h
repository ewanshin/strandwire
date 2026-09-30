#pragma once

// Module logger for NetworkClient (same pattern as ServerLib/server_log.h).
// Started and shut down from main(). What the user is meant to read as chat output (the prompt,
// "CHAT from ...") stays on std::cout; status and errors go through this logger.

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
