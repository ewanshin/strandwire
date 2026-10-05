// Byte-level tests of the LPN wire format.
// The expected byte strings here are the wire contract: if one of them has to change, every
// client in every language has to change with it.
// See docs/protocol.md.
//
// LPN 와이어 포맷의 바이트 단위 테스트.
// 여기의 기대 바이트 문자열이 와이어 계약이다. 하나라도 바꿔야 한다면 모든 언어의 모든
// 클라이언트가 함께 바뀌어야 한다.
// docs/protocol.md를 보라.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <asio.hpp>

#include "chat.pb.h"
#include "common/lpn/dispatcher.h"
#include "common/lpn/frame.h"
#include "common/lpn/frame_io.h"
#include "common/lpn/msgid.h"
#include "common/lpn/sid.h"
#include "common/lpn/wire.h"
#include "common/utf8.h"
#include "tests/test_support.h"

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n";                                  \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

namespace
{

std::vector<char> bytes(std::initializer_list<int> list)
{
    std::vector<char> out;
    for (const int v : list)
        out.push_back(static_cast<char>(v));
    return out;
}

// Strips the 4-byte frame_len so the rest can be fed to decode_body.
// 4바이트 frame_len을 떼어내 나머지를 decode_body에 넘길 수 있게 한다.
std::span<const char> body_of(const std::vector<char>& packet)
{
    return std::span<const char>(packet).subspan(lpn::FRAME_LEN_SIZE);
}

template <class F>
bool throws_protocol_error(F&& f)
{
    try
    {
        f();
    }
    catch (const lpn::protocol_error&)
    {
        return true;
    }
    return false;
}

void test_big_endian_helpers()
{
    char buf[8];
    lpn::put_u16(buf, 0x1234);
    CHECK(static_cast<unsigned char>(buf[0]) == 0x12 && static_cast<unsigned char>(buf[1]) == 0x34);
    CHECK(lpn::get_u16(buf) == 0x1234);
    lpn::put_u32(buf, 0xA1B2C3D4u);
    CHECK(static_cast<unsigned char>(buf[0]) == 0xA1 && static_cast<unsigned char>(buf[3]) == 0xD4);
    CHECK(lpn::get_u32(buf) == 0xA1B2C3D4u);
    lpn::put_u64(buf, 0x0102030405060708ull);
    CHECK(buf[0] == 1 && buf[7] == 8);
    CHECK(lpn::get_u64(buf) == 0x0102030405060708ull);
}

void test_sid()
{
    const lpn::sid s{1, 2, 11, 3};
    CHECK(s.value() == 0x00010002000B0003ull);
    CHECK(lpn::sid::from_value(s.value()) == s);
    CHECK(s.to_string() == "1.2.11.3");
    CHECK(!s.is_anycast());
    CHECK((lpn::sid{0, 0, 11, 0}).is_anycast());

    const auto parsed = lpn::sid::parse("1.2.11.3");
    CHECK(parsed && *parsed == s);
    CHECK(!lpn::sid::parse("1.2.11"));
    CHECK(!lpn::sid::parse("1.2.11.3.4"));
    CHECK(!lpn::sid::parse("1.2.x.3"));
    CHECK(!lpn::sid::parse("1.2.11.70000"));
    CHECK(!lpn::sid::parse(""));
}

void test_msgid_reference_values()
{
    // Published FNV-1a 32-bit test vectors (offset basis 2166136261, prime 16777619).
    // 공개된 FNV-1a 32비트 테스트 벡터 (offset basis 2166136261, prime 16777619).
    CHECK(lpn::fnv1a32("") == 0x811C9DC5u);
    CHECK(lpn::fnv1a32("a") == 0xE40C292Cu);
    CHECK(lpn::fnv1a32("foobar") == 0xBF9CF968u);

    CHECK(lpn::msgid_of<chat::login_req>() == lpn::fnv1a32("chat.login_req"));
    chat::chat_noti noti;
    CHECK(lpn::msgid_of(noti) == lpn::fnv1a32("chat.chat_noti"));
}

void test_golden_heartbeat()
{
    const auto expected = bytes({0x00, 0x00, 0x00, 0x02, 0x06, 0x03});
    const auto packet = lpn::encode(lpn::make_heartbeat(lpn::heartbeat_command::noop_req));
    CHECK(packet == expected);

    const lpn::frame f = lpn::decode_body(body_of(packet));
    CHECK(f.type == lpn::packet_type::heartbeat);
    CHECK(f.param == 0x03);
    CHECK(f.payload.empty());
}

void test_golden_connect_anycast()
{
    // type_ = CONNECT (1), param_ = LOBBY tunnel (11 = 0x0B). sid "0.0.11.0".
    // type_ = CONNECT (1), param_ = LOBBY 터널 (11 = 0x0B). sid "0.0.11.0".
    const auto expected = bytes({0x00, 0x00, 0x00, 0x0A, 0x01, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00});
    const lpn::sid anycast{0, 0, 11, 0};
    const auto packet = lpn::encode(lpn::make_tunnel(lpn::tunnel::lobby, lpn::packet_type::connect, anycast.value()));
    CHECK(packet == expected);

    const lpn::frame f = lpn::decode_body(body_of(packet));
    CHECK(f.is_tunnel());
    CHECK(f.tunnel_id() == 11);
    CHECK(f.type == lpn::packet_type::connect);
    CHECK(lpn::sid::from_value(f.server_sid) == anycast);
    CHECK(f.payload.empty());
}

void test_golden_data()
{
    // DATA on LOBBY to server "1.2.11.3" carrying chat.login_req{name="ab"}.
    // protobuf body: field 1, wire type 2 -> 0x0A, length 2, 'a', 'b'.
    //
    // 서버 "1.2.11.3"으로 보내는 LOBBY DATA. chat.login_req{name="ab"}를 담는다.
    // protobuf 본문: 필드 1, 와이어 타입 2 -> 0x0A, 길이 2, 'a', 'b'.
    chat::login_req req;
    req.set_name("ab");
    const std::vector<char> payload = lpn::encode_message(req);

    const std::uint32_t id = lpn::fnv1a32("chat.login_req");
    std::vector<char> expected = bytes({0x00, 0x00, 0x00, 0x12, // frame_len = 2 + 8 + 8 = 18
                                        0x03, 0x0B,             // data, tunnel 11 / DATA, 터널 11
                                        0x00, 0x01, 0x00, 0x02, 0x00, 0x0B, 0x00, 0x03}); // sid 1.2.11.3
    expected.push_back(static_cast<char>(id >> 24));
    expected.push_back(static_cast<char>(id >> 16));
    expected.push_back(static_cast<char>(id >> 8));
    expected.push_back(static_cast<char>(id));
    for (const int v : std::initializer_list<int>{0x0A, 0x02, 'a', 'b'})
        expected.push_back(static_cast<char>(v));

    const lpn::sid target{1, 2, 11, 3};
    const auto packet =
        lpn::encode(lpn::make_tunnel(lpn::tunnel::lobby, lpn::packet_type::data, target.value(), payload));
    CHECK(packet == expected);
    // 18 bytes of fixed overhead + 4 bytes of protobuf
    // 고정 오버헤드 18바이트 + protobuf 4바이트
    CHECK(packet.size() == 22);

    const lpn::frame f = lpn::decode_body(body_of(packet));
    CHECK(f.type == lpn::packet_type::data);
    CHECK(f.payload == payload);
}

// "00 1F 4D" -> bytes. Spaces and '|' are ignored so the strings can be pasted from docs/protocol.md.
// "00 1F 4D" -> 바이트. 공백과 '|'는 무시하므로 docs/protocol.md의 문자열을 그대로 붙여 넣을 수 있다.
std::vector<char> from_hex(std::string_view text)
{
    std::vector<char> out;
    int high = -1;
    for (const char c : text)
    {
        int v = -1;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        if (v < 0)
            continue;
        if (high < 0)
        {
            high = v;
        }
        else
        {
            out.push_back(static_cast<char>(high * 16 + v));
            high = -1;
        }
    }
    return out;
}

// Every byte example in docs/protocol.md section 11, produced by the real encoder.
// If this fails, either the wire format changed or the document is wrong: fix both together.
//
// docs/protocol.md 11절의 모든 바이트 예제를 실제 인코더로 만들어 본다.
// 실패하면 와이어 포맷이 바뀌었거나 문서가 틀린 것이다. 둘을 함께 고친다.
void test_documented_examples()
{
    const std::uint64_t anycast = lpn::sid{0, 0, 11, 0}.value();
    const std::uint64_t server = lpn::sid{0, 0, 11, 1}.value();
    const auto data = [&](const google::protobuf::Message& m)
    {
        return lpn::encode(
            lpn::make_tunnel(lpn::tunnel::lobby, lpn::packet_type::data, server, lpn::encode_message(m)));
    };

    CHECK(lpn::encode(lpn::make_tunnel(lpn::tunnel::lobby, lpn::packet_type::connect, anycast)) ==
          from_hex("00 00 00 0A | 01 | 0B | 00 00 00 00 00 0B 00 00"));
    CHECK(lpn::encode(lpn::make_tunnel(lpn::tunnel::lobby, lpn::packet_type::connect, server)) ==
          from_hex("00 00 00 0A | 01 | 0B | 00 00 00 00 00 0B 00 01"));

    chat::login_req login_req;
    login_req.set_name("alice");
    CHECK(data(login_req) == from_hex("00 00 00 15 | 03 | 0B | 00 00 00 00 00 0B 00 01 | "
                                      "87 29 27 DF | 0A 05 61 6C 69 63 65"));

    chat::login_res login_res;
    login_res.set_error_code_(0);
    login_res.set_player_id(1);
    CHECK(data(login_res) == from_hex("00 00 00 12 | 03 | 0B | 00 00 00 00 00 0B 00 01 | "
                                      "89 29 2B 05 | 08 00 10 01"));

    chat::chat_req chat_req;
    chat_req.set_text("hi");
    CHECK(data(chat_req) == from_hex("00 00 00 12 | 03 | 0B | 00 00 00 00 00 0B 00 01 | "
                                     "42 A3 E0 C2 | 0A 02 68 69"));

    chat::chat_res chat_res;
    chat_res.set_error_code_(0);
    CHECK(data(chat_res) == from_hex("00 00 00 10 | 03 | 0B | 00 00 00 00 00 0B 00 01 | "
                                     "40 A3 DD 9C | 08 00"));

    chat::chat_noti chat_noti;
    chat_noti.set_player_id(1);
    chat_noti.set_name("alice");
    chat_noti.set_text("hi");
    CHECK(data(chat_noti) == from_hex("00 00 00 1B | 03 | 0B | 00 00 00 00 00 0B 00 01 | "
                                      "99 8A 08 8A | 08 01 12 05 61 6C 69 63 65 1A 02 68 69"));

    CHECK(lpn::encode(lpn::make_heartbeat(lpn::heartbeat_command::noop_req)) == from_hex("00 00 00 02 | 06 | 03"));
    CHECK(lpn::encode(lpn::make_heartbeat(lpn::heartbeat_command::noop_res)) == from_hex("00 00 00 02 | 06 | 04"));

    const std::uint64_t region_anycast = lpn::sid{0, 0, 13, 0}.value();
    CHECK(lpn::encode(lpn::make_tunnel(lpn::tunnel::region, lpn::packet_type::failed, region_anycast)) ==
          from_hex("00 00 00 0A | 04 | 0D | 00 00 00 00 00 0D 00 00"));

    // msgid table in section 8
    // 8절의 msgid 표
    CHECK(lpn::msgid_of<chat::login_req>() == 2267621343u);
    CHECK(lpn::msgid_of<chat::login_res>() == 2301176581u);
    CHECK(lpn::msgid_of<chat::chat_req>() == 1118036162u);
    CHECK(lpn::msgid_of<chat::chat_res>() == 1084480924u);
    CHECK(lpn::msgid_of<chat::chat_noti>() == 2575960202u);
}

void test_decode_rejections()
{
    // shorter than the LPN header
    // LPN 헤더보다 짧다
    CHECK(throws_protocol_error(
        []
        {
            lpn::decode_body(bytes({0x06}));
        }));
    CHECK(throws_protocol_error(
        []
        {
            lpn::decode_body(std::vector<char>{});
        }));
    // tunnel packet without room for the 8-byte sid
    // 8바이트 sid가 들어갈 자리가 없는 터널 패킷
    CHECK(throws_protocol_error(
        []
        {
            lpn::decode_body(bytes({0x01, 0x0B, 0x00, 0x00, 0x00, 0x00}));
        }));
    // tunnel id beyond the tunnel table (the receiver indexes an array with it)
    // 터널 테이블 범위를 넘는 터널 id (수신 측이 이 값으로 배열을 인덱싱한다)
    CHECK(throws_protocol_error(
        []
        {
            lpn::decode_body(bytes({0x03, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01}));
        }));
    // a heartbeat is not a tunnel packet
    // 하트비트는 터널 패킷이 아니다
    CHECK(throws_protocol_error(
        []
        {
            lpn::make_tunnel(lpn::tunnel::lobby, lpn::packet_type::heartbeat, 0);
        }));
    // tunnel id out of range when building
    // 만들 때 터널 id가 범위를 벗어난다
    CHECK(throws_protocol_error(
        []
        {
            lpn::make_tunnel(std::uint8_t{32}, lpn::packet_type::data, 0);
        }));

    // unknown packet types decode fine; the receiver decides to ignore them
    // 알 수 없는 패킷 타입은 정상적으로 디코딩된다. 무시할지는 수신 측이 정한다
    const lpn::frame f = lpn::decode_body(bytes({0x7F, 0x01, 0x55}));
    CHECK(static_cast<std::uint8_t>(f.type) == 0x7F);
    CHECK(f.payload.size() == 1);
}

// Connected loopback socket pair on one io_context.
// 하나의 io_context 위에 연결된 루프백 소켓 쌍.
struct socket_pair
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    asio::ip::tcp::socket client{io};
    asio::ip::tcp::socket server{io};

    socket_pair()
    {
        client.connect(acceptor.local_endpoint());
        server = acceptor.accept();
    }
};

void test_frame_io_round_trip()
{
    socket_pair sp;
    chat::chat_req req;
    req.set_text("hello");
    const lpn::frame sent = lpn::make_tunnel(lpn::tunnel::lobby, lpn::packet_type::data, 42, lpn::encode_message(req));
    lpn::write_frame(sp.client, sent);
    lpn::write_frame(sp.client, lpn::make_heartbeat(lpn::heartbeat_command::ping_req));

    bool done = false;
    asio::co_spawn(
        sp.io,
        [&]() -> asio::awaitable<void>
        {
            const lpn::frame a = co_await lpn::read_frame(sp.server);
            CHECK(a.is_tunnel());
            CHECK(a.param == sent.param && a.server_sid == 42 && a.payload == sent.payload);
            const lpn::frame b = co_await lpn::read_frame(sp.server);
            CHECK(b.type == lpn::packet_type::heartbeat && b.param == 0x01);
            done = true;
        },
        asio::detached);
    sp.io.run();
    CHECK(done);
}

void test_frame_io_rejects_oversize()
{
    socket_pair sp;
    // frame_len = MAX_FRAME_SIZE + 1. Nothing else is sent: the reader must refuse before allocating.
    // frame_len = MAX_FRAME_SIZE + 1. 그 밖에는 아무것도 보내지 않는다. 읽는 쪽은 할당하기 전에 거부해야 한다.
    char len_buf[4];
    lpn::put_u32(len_buf, lpn::MAX_FRAME_SIZE + 1);
    asio::write(sp.client, asio::buffer(len_buf));

    bool rejected = false;
    asio::co_spawn(
        sp.io,
        [&]() -> asio::awaitable<void>
        {
            try
            {
                co_await lpn::read_frame(sp.server);
            }
            catch (const lpn::protocol_error&)
            {
                rejected = true;
            }
        },
        asio::detached);
    sp.io.run();
    CHECK(rejected);
}

struct test_ctx
{
    int logins = 0;
    std::string last_name;
    std::string last_text;

    void on_chat(const chat::chat_req& m)
    {
        last_text = m.text();
    }
};

void on_login(test_ctx& ctx, const chat::login_req& m)
{
    ++ctx.logins;
    ctx.last_name = m.name();
}

void test_dispatcher()
{
    lpn::message_dispatcher<test_ctx> d;
    d.regist(&on_login);          // free function / 자유 함수
    d.regist(&test_ctx::on_chat); // member function / 멤버 함수
    test_ctx ctx;

    chat::login_req login;
    login.set_name("alice");
    std::uint32_t id = 0;
    CHECK(d.dispatch(ctx, lpn::encode_message(login), &id) == lpn::dispatch_result::ok);
    CHECK(id == lpn::fnv1a32("chat.login_req"));
    CHECK(ctx.logins == 1 && ctx.last_name == "alice");
    CHECK(d.name_of(id) == "chat.login_req");

    chat::chat_req chat;
    chat.set_text("hi");
    CHECK(d.dispatch(ctx, lpn::encode_message(chat)) == lpn::dispatch_result::ok);
    CHECK(ctx.last_text == "hi");

    // no handler for chat_noti
    // chat_noti에는 핸들러가 없다
    chat::chat_noti noti;
    noti.set_player_id(1);
    noti.set_name("n");
    noti.set_text("t");
    CHECK(d.dispatch(ctx, lpn::encode_message(noti)) == lpn::dispatch_result::unknown_msgid);

    // payload smaller than a msgid
    // msgid보다 작은 페이로드
    CHECK(d.dispatch(ctx, bytes({0x01, 0x02})) == lpn::dispatch_result::too_short);

    // required field missing: login_req without a name serializes to an empty body
    // 필수 필드 누락: 이름 없는 login_req는 빈 본문으로 직렬화된다
    chat::login_req empty;
    CHECK(d.dispatch(ctx, lpn::encode_message(empty)) == lpn::dispatch_result::not_initialized);

    // garbage body: field 1 declared as length-delimited with a length that runs past the end
    // 쓰레기 본문: 필드 1을 길이 구분 타입으로 선언했는데 길이가 끝을 넘어간다
    std::vector<char> garbage(lpn::MSGID_SIZE);
    lpn::put_u32(garbage.data(), lpn::fnv1a32("chat.login_req"));
    garbage.push_back(0x0A);
    garbage.push_back(0x7F);
    CHECK(d.dispatch(ctx, garbage) == lpn::dispatch_result::parse_error);

    // registering the same message twice is a programming error
    // 같은 메시지를 두 번 등록하는 것은 프로그래밍 오류다
    bool threw = false;
    try
    {
        d.regist(&on_login);
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

void test_utf8_validation()
{
    CHECK(utf8::is_valid(""));
    CHECK(utf8::is_valid("hello"));
    CHECK(utf8::is_valid("\xED\x95\x9C\xEA\xB8\x80")); // "한글" in UTF-8 / UTF-8의 "한글"
    CHECK(utf8::is_valid("\xF0\x9F\x98\x80"));         // U+1F600, 4-byte sequence / U+1F600, 4바이트 시퀀스
    // "한글" in CP949: what a Korean console sends
    // CP949의 "한글": 한국어 콘솔이 보내는 바이트
    CHECK(!utf8::is_valid("\xC7\xD1\xB1\xDB"));
    // CP949 jamo, as typed in the bug report
    // CP949 자모, 버그 보고서에 입력한 그대로
    CHECK(!utf8::is_valid("\xA4\xC3\xA4\xBF"));
    CHECK(!utf8::is_valid("\x80"));             // stray continuation byte / 홀로 남은 연속 바이트
    CHECK(!utf8::is_valid("\xE3\x81"));         // truncated 3-byte sequence / 잘린 3바이트 시퀀스
    CHECK(!utf8::is_valid("\xC0\xAF"));         // overlong encoding of '/' / '/'의 오버롱 인코딩
    CHECK(!utf8::is_valid("\xED\xA0\x80"));     // UTF-16 surrogate U+D800 / UTF-16 서로게이트 U+D800
    CHECK(!utf8::is_valid("\xF4\x90\x80\x80")); // above U+10FFFF / U+10FFFF 초과
    CHECK(!utf8::is_valid("\xFF"));
}

} // namespace

int main()
{
    test_support::init();
    test_utf8_validation();
    test_big_endian_helpers();
    test_sid();
    test_msgid_reference_values();
    test_golden_heartbeat();
    test_golden_connect_anycast();
    test_golden_data();
    test_documented_examples();
    test_decode_rejections();
    test_frame_io_round_trip();
    test_frame_io_rejects_oversize();
    test_dispatcher();
    std::cout << "wire_test: OK\n";
    return 0;
}
