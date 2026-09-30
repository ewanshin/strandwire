#pragma once

#include "common/lpn/dispatcher.h"

class session;

// Registers the handlers for messages on the LOBBY tunnel (package `chat`).
// To add a message: define it in ProtoLib/chat.proto, write `void on_xxx(session&, const chat::xxx&)`
// in lobby_service.cpp and add one regist() line there. The msgid is derived from the message name.
void register_lobby_handlers(lpn::message_dispatcher<session>& dispatcher);
