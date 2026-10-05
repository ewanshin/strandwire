#pragma once

// msgid -> handler table for one tunnel:
// a handler is registered with a single function pointer, and both the message type and the
// msgid are deduced from its signature. One prototype instance per message type is kept and
// cloned with New() on dispatch.
//
// 터널 하나의 msgid -> 핸들러 표다.
// 핸들러는 함수 포인터 하나로 등록하고, 메시지 타입과 msgid는 그 시그니처에서 추론한다.
// 메시지 타입마다 프로토타입 인스턴스 하나를 보관하고 디스패치할 때 New()로 복제한다.

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
    too_short,     // payload smaller than a msgid / 페이로드가 msgid보다 작다
    unknown_msgid, // no handler registered / 등록된 핸들러가 없다
    // bytes are not a valid protobuf encoding of the message
    // 바이트가 그 메시지의 올바른 protobuf 인코딩이 아니다
    parse_error,
    not_initialized, // parsed, but a required field is missing / 파싱은 됐지만 required 필드가 빠졌다
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
    // 자유 함수 또는 캡처 없는 람다: void handler(Ctx&, const M&)
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
    // 멤버 함수: void Ctx::handler(const M&)
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
    //
    // payload = [uint32 msgid][protobuf 본문]. id를 읽을 수 있었다면 out_msgid에 넣는다.
    // 디스패치마다 새 메시지 객체를 만들므로(보관한 프로토타입에 New()) 핸들러가 메시지를 보관하거나
    // 이동해도 되고, dispatch를 여러 스레드에서 동시에 호출해도 안전하다.
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
        //
        // ParseFromArray 대신 ParsePartial + IsInitialized를 쓴다. 두 실패(잘못된 바이트와 빠진
        // required 필드)를 따로 보고하며, 서버는 둘을 다르게 다룬다.
        if (!msg->ParsePartialFromArray(body.data(), static_cast<int>(body.size())))
            return dispatch_result::parse_error;
        if (!msg->IsInitialized())
            return dispatch_result::not_initialized;

        it->second.handler(ctx, *msg);
        return dispatch_result::ok;
    }

    // Full name of the message registered under id, or empty. For logs.
    // id로 등록된 메시지의 전체 이름이다. 없으면 빈 문자열. 로그용이다.
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
    //
    // M의 프로토타입 하나를 그 msgid 아래 저장한다. 같은 id로 두 번 등록하는 것은 프로그래밍 오류이며
    // (중복이거나 해시가 같은 두 이름) 시작 시 예외를 던진다.
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
