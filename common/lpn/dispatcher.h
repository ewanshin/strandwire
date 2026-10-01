#pragma once

// msgid -> handler table for one tunnel:
// a handler is registered with a single function pointer, and both the message type and the
// msgid are deduced from its signature. One prototype instance per message type is kept and
// cloned with New() on dispatch.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include <google/protobuf/message.h>

#include "common/lpn/msgid.h"
#include "common/lpn/wire.h"

namespace lpn
{

enum class dispatch_result
{
    ok,
    too_short,       // payload smaller than a msgid
    unknown_msgid,   // no handler registered
    parse_error,     // bytes are not a valid protobuf encoding of the message
    not_initialized, // parsed, but a required field is missing
};

inline const char* to_string(dispatch_result r)
{
    switch (r)
    {
    case dispatch_result::ok:
        return "ok";
    case dispatch_result::too_short:
        return "too_short";
    case dispatch_result::unknown_msgid:
        return "unknown_msgid";
    case dispatch_result::parse_error:
        return "parse_error";
    case dispatch_result::not_initialized:
        return "not_initialized";
    }
    return "?";
}

template <class Ctx>
class message_dispatcher
{
public:
    // Free function or captureless lambda: void handler(Ctx&, const M&)
    template <class M>
    void regist(void (*fn)(Ctx&, const M&))
    {
        add<M>(
            [fn](Ctx& ctx, const google::protobuf::Message& m)
            {
                fn(ctx, static_cast<const M&>(m));
            });
    }

    // Member function: void Ctx::handler(const M&)
    template <class M>
    void regist(void (Ctx::*fn)(const M&))
    {
        add<M>(
            [fn](Ctx& ctx, const google::protobuf::Message& m)
            {
                (ctx.*fn)(static_cast<const M&>(m));
            });
    }

    // payload = [uint32 msgid][protobuf body]. out_msgid receives the id whenever it could be read.
    // A fresh message object is created per dispatch (New() on the stored prototype), so handlers
    // may keep or move the message and dispatch is safe to call from several threads at once.
    dispatch_result dispatch(Ctx& ctx, std::span<const char> payload, std::uint32_t* out_msgid = nullptr) const
    {
        if (payload.size() < MSGID_SIZE)
            return dispatch_result::too_short;

        const std::uint32_t id = get_u32(payload.data());
        if (out_msgid)
            *out_msgid = id;

        const auto it = entries_.find(id);
        if (it == entries_.end())
            return dispatch_result::unknown_msgid;

        std::unique_ptr<google::protobuf::Message> msg(it->second.prototype->New());
        const auto body = payload.subspan(MSGID_SIZE);
        // ParsePartial + IsInitialized instead of ParseFromArray: the two failures are reported
        // separately (bad bytes vs. a missing required field), which the server treats differently.
        if (!msg->ParsePartialFromArray(body.data(), static_cast<int>(body.size())))
            return dispatch_result::parse_error;
        if (!msg->IsInitialized())
            return dispatch_result::not_initialized;

        it->second.handler(ctx, *msg);
        return dispatch_result::ok;
    }

    // Full name of the message registered under id, or empty. For logs.
    std::string name_of(std::uint32_t id) const
    {
        const auto it = entries_.find(id);
        return it == entries_.end() ? std::string() : it->second.prototype->GetTypeName();
    }

private:
    using handler_fn = std::function<void(Ctx&, const google::protobuf::Message&)>;

    struct entry
    {
        std::unique_ptr<google::protobuf::Message> prototype;
        handler_fn handler;
    };

    // Stores one prototype of M under its msgid. Two registrations with the same id are a
    // programming error (a duplicate, or two names that hash alike) and throw at start-up.
    template <class M>
    void add(handler_fn handler)
    {
        const std::uint32_t id = msgid_of<M>();
        const auto [it, inserted] = entries_.try_emplace(id);
        if (!inserted)
        {
            throw std::logic_error("msgid collision or duplicate registration: " + M::descriptor()->full_name() +
                                   " vs " + it->second.prototype->GetTypeName());
        }
        it->second.prototype = std::make_unique<M>();
        it->second.handler = std::move(handler);
    }

    std::unordered_map<std::uint32_t, entry> entries_;
};

} // namespace lpn
