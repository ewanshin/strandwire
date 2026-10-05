#pragma once

// Strict UTF-8 validation. protobuf `string` fields must be UTF-8; protobuf itself only logs an
// error for proto2 messages and lets the bytes through, so the server checks text it is going to
// store or broadcast.
//
// 엄격한 UTF-8 검증이다. protobuf `string` 필드는 UTF-8이어야 한다. protobuf 자체는 proto2 메시지에서
// 오류를 로그로만 남기고 바이트를 그대로 통과시키므로, 서버가 저장하거나 브로드캐스트할 텍스트를
// 직접 검사한다.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace utf8
{

// Rejects truncated sequences, stray continuation bytes, overlong encodings, UTF-16 surrogates
// (U+D800..U+DFFF) and code points above U+10FFFF.
//
// 잘린 시퀀스, 홀로 있는 연속 바이트, overlong 인코딩, UTF-16 서로게이트(U+D800..U+DFFF),
// U+10FFFF를 넘는 코드 포인트를 거부한다.
constexpr bool is_valid(std::string_view s)
{
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n)
    {
        const auto b0 = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        std::uint32_t min = 0;
        if (b0 < 0x80)
        {
            ++i;
            continue;
        }
        else if ((b0 & 0xE0) == 0xC0)
        {
            len = 2;
            cp = b0 & 0x1Fu;
            min = 0x80;
        }
        else if ((b0 & 0xF0) == 0xE0)
        {
            len = 3;
            cp = b0 & 0x0Fu;
            min = 0x800;
        }
        else if ((b0 & 0xF8) == 0xF0)
        {
            len = 4;
            cp = b0 & 0x07u;
            min = 0x10000;
        }
        else
        {
            // continuation byte or 0xF8..0xFF as a lead byte
            // 선두 바이트가 연속 바이트거나 0xF8..0xFF다
            return false;
        }
        if (i + len > n)
            return false;
        for (std::size_t k = 1; k < len; ++k)
        {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

} // namespace utf8
