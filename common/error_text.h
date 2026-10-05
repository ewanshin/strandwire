#pragma once

// Locale-independent text for a std::error_code.
//
// std::error_code::message() for the system category is produced by the OS in the user's language
// and in the ANSI code page (on a Korean Windows: Korean text in CP949). That breaks two rules of
// this project: log text is English, and everything written to the console is UTF-8. It showed up
// as mojibake in the server log as soon as the console was switched to UTF-8.
//
//   netsys::describe(ec)  ->  "connection reset by peer (system:10054)"
//
// std::error_code를 로케일과 무관한 텍스트로 바꾼다.
//
// system 카테고리의 std::error_code::message()는 OS가 사용자의 언어와 ANSI 코드 페이지로 만든다
// (한국어 Windows에서는 CP949 한글). 이는 이 프로젝트의 두 규칙, 로그 텍스트는 영어이고 콘솔에 쓰는
// 모든 것은 UTF-8이라는 규칙을 깬다. 콘솔을 UTF-8로 바꾸자마자 서버 로그에 깨진 글자로 나타났다.
//
//   netsys::describe(ec)  ->  "connection reset by peer (system:10054)"

#include <string>
#include <system_error>

#include <asio.hpp>

namespace netsys
{

inline std::string describe(const std::error_code& ec)
{
    if (!ec)
        return "success";

    const char* text = nullptr;
    if (ec == asio::error::eof)
        text = "eof";
    else if (ec == asio::error::connection_reset)
        text = "connection reset by peer";
    else if (ec == asio::error::connection_aborted)
        text = "connection aborted";
    else if (ec == asio::error::connection_refused)
        text = "connection refused";
    else if (ec == asio::error::operation_aborted)
        text = "operation aborted";
    else if (ec == asio::error::timed_out)
        text = "timed out";
    else if (ec == asio::error::broken_pipe)
        text = "broken pipe";
    else if (ec == asio::error::not_connected)
        text = "not connected";
    else if (ec == asio::error::host_unreachable)
        text = "host unreachable";
    else if (ec == asio::error::network_unreachable)
        text = "network unreachable";
    else if (ec == asio::error::network_down)
        text = "network down";
    else if (ec == asio::error::network_reset)
        text = "network reset";
    else if (ec == asio::error::address_in_use)
        text = "address in use";
    else if (ec == asio::error::access_denied)
        text = "access denied";
    else if (ec == asio::error::no_descriptors)
        text = "too many open files";
    else if (ec == asio::error::no_buffer_space)
        text = "no buffer space";
    else if (ec == asio::error::host_not_found)
        text = "host not found";
    else if (ec == asio::error::service_not_found)
        text = "service not found";

    const std::string code = std::string(ec.category().name()) + ":" + std::to_string(ec.value());
    return text ? std::string(text) + " (" + code + ")" : "error (" + code + ")";
}

} // namespace netsys
