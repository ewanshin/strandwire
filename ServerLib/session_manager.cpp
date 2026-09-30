#include "ServerLib/session_manager.h"

#include "ServerLib/session.h"

// The registry is the one place in the server protected by a mutex. Every method keeps the
// critical section to the map operation itself; anything that calls into a session happens after
// the lock is released, on that session's strand. So a session is never blocked by the registry
// and the registry is never re-entered from a session.

void session_manager::add(std::shared_ptr<session> s)
{
    std::lock_guard lock(mutex_);
    sessions_.emplace(s->id(), std::move(s));
}

// Called by session::close() on the session's own strand. Dropping the map's shared_ptr may be
// what finally frees the session, once its running coroutines finish and release `self`.
void session_manager::remove(std::uint32_t id)
{
    std::lock_guard lock(mutex_);
    sessions_.erase(id);
}

std::size_t session_manager::count() const
{
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

// Copies the current set of sessions under the lock. Holding shared_ptrs keeps them alive while
// the caller iterates without the lock. A session that closes meanwhile ignores what it is handed:
// send() and close() are no-ops on a closed session.
std::vector<std::shared_ptr<session>> session_manager::snapshot() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<session>> out;
    out.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_)
        out.push_back(s);
    return out;
}

// Fan-out of one encoded packet. Every session gets the same shared_buffer (one copy of the bytes
// for all recipients) and touches its own queue on its own strand. The tunnel check also runs on
// the strand, because the tunnel table is session state.
void session_manager::broadcast(std::uint8_t tunnel_id, lpn::shared_buffer buf)
{
    for (auto& s : snapshot()) {
        asio::dispatch(s->strand(), [s, tunnel_id, buf] {
            if (s->tunnel_open(tunnel_id))
                s->send(buf);
        });
    }
}

// Server shutdown: ask every session to close itself. Each close runs on its strand and removes
// the session from the map; when the last one is gone the io_context runs out of work.
void session_manager::close_all()
{
    for (auto& s : snapshot())
        asio::dispatch(s->strand(), [s] { s->close("server shutdown"); });
}
