#include "ServerLib/server_app.h"
#include "ServerLib/server_log.h"
#include "common/console.h"

// Everything that happens between process start and "server ready" lives in server_app
// (ServerLib/server_app.h). This file only wires the console to it.
int main(int argc, char* argv[])
{
    console::init_utf8_output(); // player names in the log are UTF-8

    server_app app;
    if (app.start(argc, argv) != 0)
        return 1;

    {
        // ESC on the console stops the server cleanly, exactly like Ctrl+C / SIGTERM.
        // Inactive when stdin is not an interactive console.
        console::key_watcher esc([&app] {
            server_log.info("ESC pressed: stopping");
            app.stop();
        });
        if (esc.active())
            server_log.info("press ESC or Ctrl+C to stop");
        app.wait();
    } // the watcher thread is joined here, before the app shuts down

    app.shutdown();
    return 0;
}
