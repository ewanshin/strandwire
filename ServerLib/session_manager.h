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
// 살아 있는 세션의 스레드 안전한 레지스트리. 락을 쥔 채로 세션을 호출하지 않는다.
class session_manager
{
public:
    void add(std::shared_ptr<session> s);
    void remove(std::uint32_t id);
    std::size_t count() const;

    // Queues buf on every live session that has tunnel_id open. buf is a fully encoded packet.
    // tunnel_id가 열린 모든 살아 있는 세션의 큐에 buf를 넣는다. buf는 완전히 인코딩된 패킷이다.
    void broadcast(std::uint8_t tunnel_id, lpn::shared_buffer buf);
    // Closes every live session on its own strand.
    // 살아 있는 모든 세션을 각자의 strand에서 닫는다.
    void close_all();

    // Id counters. Session ids number every connection; player ids are issued at login only.
    // Atomics because sessions call these from their strands, i.e. from any worker thread.
    //
    // id 카운터. 세션 id는 모든 연결에 번호를 매기고, 플레이어 id는 로그인 때만 발급한다.
    // 세션이 자기 strand에서, 즉 어느 워커 스레드에서든 호출하므로 atomic이다.
    std::uint32_t next_session_id()
    {
        return next_session_id_++;
    }
    std::int32_t next_player_id()
    {
        return next_player_id_++;
    }

private:
    std::vector<std::shared_ptr<session>> snapshot() const;

    // guards sessions_ only
    // sessions_만 보호한다
    mutable std::mutex mutex_;
    // one strong reference per live session
    // 살아 있는 세션마다 강한 참조 하나
    std::unordered_map<std::uint32_t, std::shared_ptr<session>> sessions_;
    std::atomic<std::uint32_t> next_session_id_{1};
    std::atomic<std::int32_t> next_player_id_{1};
};
