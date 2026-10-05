#include "ServerLib/lobby_service.h"

#include <cstddef>
#include <string>

#include "ServerLib/server_log.h"
#include "ServerLib/session.h"
#include "ServerLib/session_manager.h"
#include "chat.pb.h"
#include "common/lpn/frame.h"
#include "common/lpn/frame_io.h"
#include "common/lpn/msgid.h"
#include "common/utf8.h"
#include "error_code.pb.h"

namespace
{

constexpr std::size_t MAX_NAME_BYTES = 64;
constexpr auto LOBBY = static_cast<std::uint8_t>(lpn::tunnel::lobby);

// Handlers run on the session's strand, so they may read and write the session freely. They must
// not touch other sessions directly; the only cross-session path is session_manager::broadcast.
// Each request always gets exactly one reply on the same tunnel, success or not.
//
// 핸들러는 세션의 strand에서 실행되므로 세션을 자유롭게 읽고 쓸 수 있다. 다른 세션을 직접 건드리면
// 안 된다. 세션 간 경로는 session_manager::broadcast뿐이다.
// 모든 요청은 성공이든 아니든 같은 터널로 정확히 하나의 응답을 받는다.

// login_req: claim a player id for this connection. The name is the only input and is checked
// here because protobuf lets non-UTF-8 strings through in proto2.
//
// login_req: 이 연결에 플레이어 id를 요구한다. 이름이 유일한 입력이고 여기서 검사한다.
// proto2에서 protobuf는 UTF-8이 아닌 문자열을 통과시키기 때문이다.
void on_login_req(session& s, const chat::login_req& req)
{
    chat::login_res res;
    if (s.logged_in())
    {
        res.set_error_code_(nserror::ALREADY_LOGGED_IN);
    }
    else if (req.name().empty() || req.name().size() > MAX_NAME_BYTES || !utf8::is_valid(req.name()))
    {
        res.set_error_code_(nserror::INVALID_NAME);
    }
    else
    {
        const std::int32_t pid = s.manager().next_player_id();
        s.set_player(pid, req.name());
        res.set_error_code_(nserror::SUCCESS);
        res.set_player_id(pid);
        server_log.info("[session ", s.id(), "] login ok: id=", pid, " name=", s.name());
    }
    s.send_message(lpn::tunnel::lobby, res);
}

// chat_req: reply chat_res to the sender, then broadcast a chat_noti (sender included) to every
// session that has the LOBBY tunnel open. The sender's name comes from the session, never from
// the request, so it cannot be spoofed.
//
// chat_req: 보낸 쪽에 chat_res를 답하고, LOBBY 터널이 열린 모든 세션에 (보낸 쪽 포함) chat_noti를
// 브로드캐스트한다. 보낸 쪽 이름은 요청이 아니라 세션에서 가져오므로 위조할 수 없다.
void on_chat_req(session& s, const chat::chat_req& req)
{
    chat::chat_res res;
    if (!s.logged_in())
    {
        res.set_error_code_(nserror::NOT_LOGGED_IN);
        s.send_message(lpn::tunnel::lobby, res);
        return;
    }
    // Never broadcast bytes that are not UTF-8: every receiver's protobuf would complain, and
    // non-C++ clients could fail to decode the string at all.
    //
    // UTF-8이 아닌 바이트는 절대 브로드캐스트하지 않는다. 모든 수신자의 protobuf가 불평하고,
    // C++가 아닌 클라이언트는 문자열을 아예 디코딩하지 못할 수 있다.
    if (!utf8::is_valid(req.text()))
    {
        res.set_error_code_(nserror::INVALID_TEXT);
        s.send_message(lpn::tunnel::lobby, res);
        return;
    }
    res.set_error_code_(nserror::SUCCESS);
    s.send_message(lpn::tunnel::lobby, res);

    chat::chat_noti noti;
    noti.set_player_id(s.player_id());
    noti.set_name(s.name());
    noti.set_text(req.text());
    // One encoded packet shared by every recipient. All sessions are bound to the same server sid.
    // 모든 수신자가 공유하는 인코딩된 패킷 하나. 모든 세션이 같은 서버 sid에 바인딩되어 있다.
    const lpn::frame f =
        lpn::make_tunnel(LOBBY, lpn::packet_type::data, s.tunnel_sid(LOBBY), lpn::encode_message(noti));
    s.manager().broadcast(LOBBY, lpn::make_shared_buffer(f));
}

} // namespace

// The msgid of each handler is derived from its parameter type (chat.login_req etc.), so this is
// the whole registration: no id table to keep in sync with chat.proto.
//
// 각 핸들러의 msgid는 매개변수 타입(chat.login_req 등)에서 유도되므로 이것이 등록의 전부다.
// chat.proto와 맞춰 둘 id 표가 없다.
void register_lobby_handlers(lpn::message_dispatcher<session>& dispatcher)
{
    dispatcher.regist(&on_login_req);
    dispatcher.regist(&on_chat_req);
}
