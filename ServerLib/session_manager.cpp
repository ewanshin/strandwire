#include "ServerLib/session_manager.h"

#include "ServerLib/session.h"

void session_manager::add(std::shared_ptr<session> s)
{
    std::lock_guard lock(mutex_);
    sessions_.emplace(s->id(), std::move(s));
}

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

std::vector<std::shared_ptr<session>> session_manager::snapshot() const
{
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<session>> out;
    out.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_)
        out.push_back(s);
    return out;
}

void session_manager::broadcast(std::uint8_t tunnel_id, lpn::shared_buffer buf)
{
    for (auto& s : snapshot()) {
        asio::dispatch(s->strand(), [s, tunnel_id, buf] {
            if (s->tunnel_open(tunnel_id))
                s->send(buf);
        });
    }
}

void session_manager::close_all()
{
    for (auto& s : snapshot())
        asio::dispatch(s->strand(), [s] { s->close("server shutdown"); });
}
