#pragma once

// 64-bit server instance id: four 16-bit parts.
// Text form: "domain.idc.type.id". id == 0 means anycast: any instance of that server type.

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lpn
{

struct sid
{
    std::uint16_t domain = 0; // service / world
    std::uint16_t idc = 0;    // data centre (region); reserved for a gateway per region
    std::uint16_t type = 0;   // server type; equals the tunnel id
    std::uint16_t id = 0;     // instance number; 0 = anycast

    // Packing on the wire: domain in the top 16 bits, id in the bottom 16, big-endian as a whole.
    static constexpr sid from_value(std::uint64_t v)
    {
        return sid{static_cast<std::uint16_t>(v >> 48), static_cast<std::uint16_t>(v >> 32),
                   static_cast<std::uint16_t>(v >> 16), static_cast<std::uint16_t>(v)};
    }

    constexpr std::uint64_t value() const
    {
        return (static_cast<std::uint64_t>(domain) << 48) | (static_cast<std::uint64_t>(idc) << 32) |
               (static_cast<std::uint64_t>(type) << 16) | static_cast<std::uint64_t>(id);
    }

    constexpr bool is_anycast() const
    {
        return id == 0;
    }

    std::string to_string() const
    {
        return std::to_string(domain) + "." + std::to_string(idc) + "." + std::to_string(type) + "." +
               std::to_string(id);
    }

    // Parses "domain.idc.type.id". Returns nullopt on any syntax or range error.
    static std::optional<sid> parse(std::string_view text)
    {
        std::uint16_t parts[4] = {};
        const char* p = text.data();
        const char* end = text.data() + text.size();
        for (int i = 0; i < 4; ++i)
        {
            const auto [next, ec] = std::from_chars(p, end, parts[i]);
            if (ec != std::errc{} || next == p)
                return std::nullopt;
            p = next;
            if (i < 3)
            {
                if (p == end || *p != '.')
                    return std::nullopt;
                ++p;
            }
        }
        if (p != end)
            return std::nullopt;
        return sid{parts[0], parts[1], parts[2], parts[3]};
    }

    friend constexpr bool operator==(const sid&, const sid&) = default;
};

} // namespace lpn
