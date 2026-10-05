#pragma once

// msgid = FNV-1a32 of the protobuf message's full name (package included), e.g. "chat.login_req".
// The name is taken from the protobuf runtime at registration time, so a stock protoc is enough
// and there is no hand-maintained id table.
//
// msgid = protobuf 메시지 전체 이름(패키지 포함)의 FNV-1a 32비트 해시다. 예: "chat.login_req".
// 이름은 등록 시점에 protobuf 런타임에서 가져오므로 기본 protoc로 충분하고 손으로 관리하는 id 표가 없다.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/message.h>

#include "common/lpn/frame.h"
#include "common/lpn/wire.h"

namespace lpn
{

inline constexpr std::size_t MSGID_SIZE = 4;

// FNV-1a, 32-bit: xor each byte into the hash, then multiply by the prime. constexpr so a msgid
// can be a compile-time constant where the name is a literal.
//
// FNV-1a 32비트: 각 바이트를 해시에 xor한 뒤 소수를 곱한다. constexpr이므로 이름이 리터럴이면
// msgid가 컴파일 시간 상수가 될 수 있다.
constexpr std::uint32_t fnv1a32(std::string_view s)
{
    std::uint32_t hash = 2166136261u; // FNV offset basis / FNV 오프셋 기저
    for (const char c : s)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u; // FNV prime / FNV 소수
    }
    return hash;
}

// msgid of a message instance (runtime type name) and of a message type (descriptor). Both give
// the same value for the same type; the instance form is what encode_message uses.
//
// 메시지 인스턴스(런타임 타입 이름)의 msgid와 메시지 타입(디스크립터)의 msgid다. 같은 타입이면
// 둘 다 같은 값을 준다. encode_message는 인스턴스 형태를 쓴다.
inline std::uint32_t msgid_of(const google::protobuf::Message& m)
{
    return fnv1a32(m.GetTypeName());
}

template <class M>
std::uint32_t msgid_of()
{
    return fnv1a32(M::descriptor()->full_name());
}

// Payload of a DATA packet: [uint32 msgid, big-endian][protobuf body].
// Uses the partial serializer so a message with missing required fields can still be built in tests;
// callers that care check IsInitialized() first.
//
// DATA 패킷의 페이로드: [uint32 msgid, big-endian][protobuf 본문].
// 부분 직렬화를 쓰므로 테스트에서 required 필드가 빠진 메시지도 만들 수 있다.
// 신경 쓰는 호출자는 먼저 IsInitialized()를 확인한다.
inline std::vector<char> encode_message(const google::protobuf::Message& m)
{
    const std::size_t body = m.ByteSizeLong();
    std::vector<char> out(MSGID_SIZE + body);
    put_u32(out.data(), msgid_of(m));
    if (body > 0 && !m.SerializePartialToArray(out.data() + MSGID_SIZE, static_cast<int>(body)))
        throw protocol_error("protobuf serialization failed: " + m.GetTypeName());
    return out;
}

} // namespace lpn
