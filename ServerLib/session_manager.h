#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/lpn/frame_io.h"

class session;

// Thread-safe registry of live sessions. Never calls into a session while holding the lock.
class session_manager
{
public:
    void add(std::shared_ptr<session> s);
    void remove(std::uint32_t id);
    std::size_t count() const;

    // Queues buf on every live session that has tunnel_id open. buf is a fully encoded packet.
    void broadcast(std::uint8_t tunnel_id, lpn::shared_buffer buf);
    // Closes every live session on its own strand.
    void close_all();

    // Id counters. Session ids number every connection; player ids are issued at login only.
    // Atomics because sessions call these from their strands, i.e. from any worker thread.
    std::uint32_t next_session_id() { return next_session_id_++; }
    std::int32_t next_player_id() { return next_player_id_++; }

private:
    std::vector<std::shared_ptr<session>> snapshot() const;

    mutable std::mutex mutex_; // guards sessions_ only
    std::unordered_map<std::uint32_t, std::shared_ptr<session>> sessions_; // one strong reference per live session
    std::atomic<std::uint32_t> next_session_id_{1};
    std::atomic<std::int32_t> next_player_id_{1};
};
