#pragma once

#include "common/lpn/dispatcher.h"

class session;

// Registers the handlers for messages on the LOBBY tunnel (package `chat`).
// To add a message: define it in ProtoLib/chat.proto, write `void on_xxx(session&, const chat::xxx&)`
// in lobby_service.cpp and add one regist() line there. The msgid is derived from the message name.
//
// LOBBY 터널의 메시지(패키지 `chat`) 핸들러를 등록한다.
// 메시지를 추가하려면: ProtoLib/chat.proto에 정의하고, lobby_service.cpp에
// `void on_xxx(session&, const chat::xxx&)`를 쓰고, 거기에 regist() 한 줄을 더한다.
// msgid는 메시지 이름에서 유도된다.
void register_lobby_handlers(lpn::message_dispatcher<session>& dispatcher);
