#pragma once

/* Putting a server's TCP segment on the wire, for the tests that need one.
 *
 * Shared by t_tcp.cpp and t_http.cpp rather than written twice: it carries a
 * checksum, and a checksum written twice is one that can be wrong in one place
 * while every test still passes.
 *
 * Only the ethernet and IPv4 wrapping and the one's-complement sum come from
 * the code under test; every byte TCP itself owns is written out here.  Both
 * borrowed pieces are pinned independently in t_ip.cpp -- against RFC 1071's
 * worked example and against a receiver's own check that a header sums to
 * zero. */

#include <array>
#include <cstring>
#include <vector>

#include "doctest.h"

extern "C" {
#include "tcp.h"
}

namespace wire {

inline constexpr std::array<uint8_t, 6> OUR_MAC = {0x02, 0x47, 0x53, 0x11, 0x22, 0x33};
inline constexpr std::array<uint8_t, 6> SERVER_MAC = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
inline constexpr std::array<uint8_t, 4> OUR_IP = {192, 168, 68, 120};
inline constexpr std::array<uint8_t, 4> SERVER_IP = {93, 184, 216, 34};
inline constexpr std::array<uint8_t, 4> ELSEWHERE = {10, 1, 2, 3};

inline constexpr uint16_t HTTP_PORT = 80;
/* Where the TCP header starts, which the two headers before it fix. */
inline constexpr size_t TCP_AT = IPV4_PAYLOAD_AT;

/* RFC 793 section 3.1's control bits. */
inline constexpr uint8_t FIN = 0x01;
inline constexpr uint8_t SYN = 0x02;
inline constexpr uint8_t RST = 0x04;
inline constexpr uint8_t PSH = 0x08;
inline constexpr uint8_t ACK = 0x10;

inline NetEndpoint endpoint(const std::array<uint8_t, 6>& mac, const std::array<uint8_t, 4>& ip,
                            uint16_t port) {
    NetEndpoint e{};
    std::memcpy(e.mac, mac.data(), 6);
    std::memcpy(e.ip, ip.data(), 4);
    e.port = port;
    return e;
}

inline uint16_t get16(const uint8_t* at) { return static_cast<uint16_t>((at[0] << 8) | at[1]); }
inline uint32_t get32(const uint8_t* at) {
    return (static_cast<uint32_t>(get16(at)) << 16) | get16(at + 2);
}
inline void put16(uint8_t* at, uint16_t v) {
    at[0] = static_cast<uint8_t>(v >> 8);
    at[1] = static_cast<uint8_t>(v);
}
inline void put32(uint8_t* at, uint32_t v) {
    put16(at, static_cast<uint16_t>(v >> 16));
    put16(at + 2, static_cast<uint16_t>(v));
}

/* One segment, in the shape both directions are read and written in. */
struct Seg {
    uint8_t flags = 0;
    uint32_t seq = 0;
    uint32_t ack = 0;
    uint16_t window = 0;
    std::vector<uint8_t> options;
    std::vector<uint8_t> payload;
};

/* A whole frame as a server puts one on the wire.  `corrupt` flips one bit of
 * the payload after the checksum is computed, which is the only way to make a
 * segment that is well formed and wrong. */
inline std::vector<uint8_t> from_server(const Seg& s, uint16_t our_port,
                                        const std::array<uint8_t, 4>& ip = SERVER_IP,
                                        uint16_t their_port = HTTP_PORT, bool corrupt = false) {
    REQUIRE(s.options.size() % 4 == 0);
    const size_t header = TCP_HEADER_BYTES + s.options.size();
    const size_t segment = header + s.payload.size();
    std::vector<uint8_t> f(TCP_AT + segment, 0);

    const NetEndpoint from = endpoint(SERVER_MAC, ip, 0);
    const NetEndpoint to = endpoint(OUR_MAC, OUR_IP, 0);
    ipv4_build_header(f.data(), &from, &to, IPV4_PROTOCOL_TCP, static_cast<uint16_t>(segment));

    uint8_t* t = f.data() + TCP_AT;
    put16(t + 0, their_port);
    put16(t + 2, our_port);
    put32(t + 4, s.seq);
    put32(t + 8, s.ack);
    t[12] = static_cast<uint8_t>((header / 4) << 4);
    t[13] = s.flags;
    put16(t + 14, s.window ? s.window : 8192);
    put16(t + 16, 0);  // the checksum, zero while it is computed over
    put16(t + 18, 0);  // urgent pointer
    std::memcpy(t + TCP_HEADER_BYTES, s.options.data(), s.options.size());
    std::memcpy(t + header, s.payload.data(), s.payload.size());

    uint32_t sum = ipv4_pseudo_sum(ip.data(), OUR_IP.data(), IPV4_PROTOCOL_TCP,
                                   static_cast<uint16_t>(segment));
    sum = ip_sum(sum, t, static_cast<uint16_t>(segment));
    put16(t + 16, ip_sum_final(sum));
    if (corrupt) {
        f.back() ^= 0x01;
    }
    return f;
}

/* Everything a receiver checks about a frame this stack sent: both headers sum
 * to zero, and the length IPv4 states is the length that arrived.  A wrong
 * checksum is a segment the far end discards without telling anybody, so it is
 * checked on every frame either suite sends rather than in a case of its
 * own. */
inline void check_sent(const uint8_t* frame, uint16_t length) {
    CHECK(ip_sum_final(ip_sum(0, frame + ETH_HEADER_BYTES, IPV4_HEADER_BYTES)) == 0);
    const uint16_t segment = static_cast<uint16_t>(length - TCP_AT);
    uint32_t sum = ipv4_pseudo_sum(OUR_IP.data(), SERVER_IP.data(), IPV4_PROTOCOL_TCP, segment);
    sum = ip_sum(sum, frame + TCP_AT, segment);
    CHECK(ip_sum_final(sum) == 0);
    CHECK(get16(frame + ETH_HEADER_BYTES + 2) == IPV4_HEADER_BYTES + segment);
}

/* The segment in `frame`, read back the way the server would read it. */
inline Seg sent(const uint8_t* frame, uint16_t length) {
    Seg s;
    if (length == 0) {
        return s;
    }
    REQUIRE(length >= TCP_PAYLOAD_AT);
    const uint8_t* t = frame + TCP_AT;
    const size_t header = static_cast<size_t>(t[12] >> 4) * 4;
    s.flags = t[13];
    s.seq = get32(t + 4);
    s.ack = get32(t + 8);
    s.window = get16(t + 14);
    s.options.assign(t + TCP_HEADER_BYTES, t + header);
    s.payload.assign(t + header, frame + length);
    check_sent(frame, length);
    return s;
}

}  // namespace wire
