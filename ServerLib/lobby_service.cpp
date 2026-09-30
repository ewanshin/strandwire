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

// Handlers run on the session's strand.

void on_login_req(session& s, const chat::login_req& req)
{
    chat::login_res res;
    if (s.logged_in()) {
        res.set_error_code_(nserror::ALREADY_LOGGED_IN);
    } else if (req.name().empty() || req.name().size() > MAX_NAME_BYTES || !utf8::is_valid(req.name())) {
        res.set_error_code_(nserror::INVALID_NAME);
    } else {
        const std::int32_t pid = s.manager().next_player_id();
        s.set_player(pid, req.name());
        res.set_error_code_(nserror::SUCCESS);
        res.set_player_id(pid);
        server_log.info("[session ", s.id(), "] login ok: id=", pid, " name=", s.name());
    }
    s.send_message(lpn::tunnel::lobby, res);
}

void on_chat_req(session& s, const chat::chat_req& req)
{
    chat::chat_res res;
    if (!s.logged_in()) {
        res.set_error_code_(nserror::NOT_LOGGED_IN);
        s.send_message(lpn::tunnel::lobby, res);
        return;
    }
    // Never broadcast bytes that are not UTF-8: every receiver's protobuf would complain, and
    // non-C++ clients could fail to decode the string at all.
    if (!utf8::is_valid(req.text())) {
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
    const lpn::frame f =
        lpn::make_tunnel(LOBBY, lpn::packet_type::data, s.tunnel_sid(LOBBY), lpn::encode_message(noti));
    s.manager().broadcast(LOBBY, lpn::make_shared_buffer(f));
}

} // namespace

void register_lobby_handlers(lpn::message_dispatcher<session>& dispatcher)
{
    dispatcher.regist(&on_login_req);
    dispatcher.regist(&on_chat_req);
}
