#pragma once

// msgid = FNV-1a32 of the protobuf message's full name (package included), e.g. "chat.login_req".
// The name is taken from the protobuf runtime at registration time, so a stock protoc is enough
// and there is no hand-maintained id table.

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
constexpr std::uint32_t fnv1a32(std::string_view s)
{
    std::uint32_t hash = 2166136261u; // FNV offset basis
    for (const char c : s) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u; // FNV prime
    }
    return hash;
}

// msgid of a message instance (runtime type name) and of a message type (descriptor). Both give
// the same value for the same type; the instance form is what encode_message uses.
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
