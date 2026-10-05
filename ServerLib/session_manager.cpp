#include "ServerLib/session_manager.h"

#include "ServerLib/session.h"

// The registry is the one place in the server protected by a mutex. Every method keeps the
// critical section to the map operation itself; anything that calls into a session happens after
// the lock is released, on that session's strand. So a session is never blocked by the registry
// and the registry is never re-entered from a session.
//
// 레지스트리는 서버에서 mutex로 보호되는 유일한 곳이다. 모든 메서드는 임계 구역을 맵 연산 자체로
// 한정한다. 세션을 호출하는 일은 락을 놓은 뒤 그 세션의 strand에서 일어난다. 그래서 세션은
// 레지스트리에 막히지 않고 레지스트리는 세션에서 재진입되지 않는다.

void session_manager::add(std::shared_ptr<session> s)
{
    std::lock_guard lock(mutex_);
    sessions_.emplace(s->id(), std::move(s));
}

// Called by session::close() on the session's own strand. Dropping the map's shared_ptr may be
// what finally frees the session, once its running coroutines finish and release `self`.
//
// 세션 자신의 strand에서 session::close()가 호출한다. 맵의 shared_ptr을 놓는 것이 세션을 마침내
// 해제하는 일일 수 있다. 실행 중인 코루틴이 끝나고 `self`를 놓은 뒤에.
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
//
// 락 아래에서 현재 세션 집합을 복사한다. shared_ptr을 쥐고 있어 호출자가 락 없이 순회하는 동안
// 세션이 살아 있다. 그 사이에 닫힌 세션은 받은 것을 무시한다. 닫힌 세션에서 send()와 close()는
// 아무 일도 하지 않는다.
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
//
// 인코딩된 패킷 하나의 팬아웃. 모든 세션이 같은 shared_buffer를 받고 (모든 수신자에 바이트 복사본
// 하나) 자기 strand에서 자기 큐를 만진다. 터널 표는 세션 상태이므로 터널 확인도 strand에서 한다.
void session_manager::broadcast(std::uint8_t tunnel_id, lpn::shared_buffer buf)
{
    for (auto& s : snapshot())
    {
        asio::dispatch(s->strand(),
                       [s, tunnel_id, buf]
                       {
                           if (s->tunnel_open(tunnel_id))
                               s->send(buf);
                       });
    }
}

// Server shutdown: ask every session to close itself. Each close runs on its strand and removes
// the session from the map; when the last one is gone the io_context runs out of work.
//
// 서버 종료: 모든 세션에 스스로 닫으라고 요청한다. 각 close는 자기 strand에서 실행되고 세션을
// 맵에서 지운다. 마지막 세션이 사라지면 io_context는 할 일이 없어진다.
void session_manager::close_all()
{
    for (auto& s : snapshot())
        asio::dispatch(s->strand(),
                       [s]
                       {
                           s->close("server shutdown");
                       });
}
