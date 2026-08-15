/* IPv4 and UDP, against RFC 791, RFC 768 and RFC 1071 written out as literals.
 *
 * The oracle is the wire format and one property of it: a header summed
 * *including* its own checksum comes to zero.  That is how a receiver checks
 * one, so it is an account of the answer that does not repeat the arithmetic
 * that produced it -- which matters here more than anywhere else in this tree,
 * because a wrong checksum produces no error, just a datagram the far end
 * throws away without telling anybody.
 *
 * One datagram is written out by hand with its checksums computed elsewhere,
 * so the offsets are pinned against bytes this code did not produce and not
 * only against its own output. */

#include <array>
#include <cstring>
#include <vector>

#include "doctest.h"

extern "C" {
#include "ip.h"
}

namespace {

const std::array<uint8_t, 6> OUR_MAC = {0x02, 0x47, 0x53, 0x11, 0x22, 0x33};
const std::array<uint8_t, 6> THEIR_MAC = {0x56, 0x06, 0x94, 0xdf, 0x5f, 0xa5};
const std::array<uint8_t, 4> OUR_IP = {192, 168, 68, 234};
const std::array<uint8_t, 4> THEIR_IP = {192, 168, 68, 57};

NetEndpoint endpoint(const std::array<uint8_t, 6>& mac, const std::array<uint8_t, 4>& ip,
                     uint16_t port) {
    NetEndpoint e{};
    std::memcpy(e.mac, mac.data(), 6);
    std::memcpy(e.ip, ip.data(), 4);
    e.port = port;
    return e;
}

std::vector<uint8_t> build(const std::vector<uint8_t>& payload, uint16_t from = 4096,
                           uint16_t to = 69) {
    std::vector<uint8_t> frame(UDP_PAYLOAD_AT + payload.size() + 8, 0xEE);
    const NetEndpoint us = endpoint(OUR_MAC, OUR_IP, from);
    const NetEndpoint them = endpoint(THEIR_MAC, THEIR_IP, to);
    const uint16_t length = udp_build(frame.data(), &us, &them, payload.data(),
                                      static_cast<uint16_t>(payload.size()));
    CHECK(length == UDP_PAYLOAD_AT + payload.size());
    /* Nothing written past the length it reported. */
    for (size_t i = length; i < frame.size(); i++) {
        CHECK(frame[i] == 0xEE);
    }
    frame.resize(length);
    return frame;
}

/* An endpoint to be parsed against, kept alive for the call. */
NetEndpoint parse_target;
const NetEndpoint& at(const std::array<uint8_t, 4>& ip, uint16_t port) {
    parse_target = endpoint(OUR_MAC, ip, port);
    return parse_target;
}

uint16_t at16(const std::vector<uint8_t>& f, size_t i) {
    return static_cast<uint16_t>((f[i] << 8) | f[i + 1]);
}

/* A receiver's check, and the reason a header carries a checksum at all. */
bool sums_to_zero(const uint8_t* data, uint16_t length) {
    return ip_sum_final(ip_sum(0, data, length)) == 0;
}

}  // namespace

TEST_SUITE("ip") {

/* RFC 1071's own worked example, so the sum is checked against the document
 * rather than against another run of itself. */
TEST_CASE("the one's-complement sum matches RFC 1071") {
    const uint8_t example[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(ip_sum_final(ip_sum(0, example, sizeof example)) == 0x220d);
}

TEST_CASE("an odd length pads with a zero low byte") {
    const uint8_t odd[] = {0x12};
    const uint8_t padded[] = {0x12, 0x00};
    CHECK(ip_sum(0, odd, 1) == ip_sum(0, padded, 2));
}

/* A sum accumulated over pieces is the sum over the whole, which is what lets
 * a UDP checksum cover a pseudo header that is not in the datagram. */
TEST_CASE("a sum can be carried across pieces") {
    const uint8_t whole[] = {0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46};
    const uint32_t split = ip_sum(ip_sum(0, whole, 2), whole + 2, 4);
    CHECK(ip_sum_final(split) == ip_sum_final(ip_sum(0, whole, sizeof whole)));
}

/* Every field of the header, at the offset RFC 791 puts it. */
TEST_CASE("the headers are where the RFCs say") {
    const auto f = build({0xDE, 0xAD, 0xBE, 0xEF});

    CHECK(at16(f, 12) == 0x0800);       // ethertype IPv4
    CHECK(f[14] == 0x45);               // version 4, 5-word header
    CHECK(at16(f, 16) == 20 + 8 + 4);   // total length
    CHECK(at16(f, 20) == 0x4000);       // don't fragment, no offset
    CHECK(f[22] == 64);                 // TTL
    CHECK(f[23] == 17);                 // protocol UDP
    for (int i = 0; i < 4; i++) {
        CHECK(f[26 + i] == OUR_IP[i]);
        CHECK(f[30 + i] == THEIR_IP[i]);
    }
    for (int i = 0; i < 6; i++) {
        CHECK(f[0 + i] == THEIR_MAC[i]);
        CHECK(f[6 + i] == OUR_MAC[i]);
    }
    CHECK(at16(f, 34) == 4096);         // source port
    CHECK(at16(f, 36) == 69);           // destination port
    CHECK(at16(f, 38) == 8 + 4);        // UDP length
    CHECK(f[42] == 0xDE);               // the payload, right after the headers
    CHECK(UDP_PAYLOAD_AT == 42);
}

TEST_CASE("the header checksum is one a receiver accepts") {
    for (size_t n : {0u, 1u, 4u, 5u, 512u}) {
        CAPTURE(n);
        const auto f = build(std::vector<uint8_t>(n, 0xA5));
        CHECK(sums_to_zero(f.data() + 14, 20));
    }
}

/* Odd payload lengths are where a checksum written without care goes wrong,
 * because the final word is half a byte of data and half padding. */
TEST_CASE("a datagram is accepted back whatever the payload length") {
    for (size_t n : {0u, 1u, 2u, 3u, 511u, 512u}) {
        CAPTURE(n);
        std::vector<uint8_t> payload(n);
        for (size_t i = 0; i < n; i++) {
            payload[i] = static_cast<uint8_t>(i * 7 + 1);
        }
        const auto f = build(payload);
        UdpDatagram got{};
        REQUIRE(udp_parse(f.data(), static_cast<uint16_t>(f.size()), &at(THEIR_IP, 69), &got));
        CHECK(got.payload_length == n);
        CHECK(got.from.port == 4096);
        CHECK(std::memcmp(got.from.ip, OUR_IP.data(), 4) == 0);
        CHECK(std::memcmp(got.from.mac, OUR_MAC.data(), 6) == 0);
        CHECK(std::memcmp(f.data() + UDP_PAYLOAD_AT, payload.data(), n) == 0);
    }
}

/* A checksum that comes to zero is sent as all ones, because zero on the wire
 * means "not computed" and the receiver would stop checking.
 *
 * Source port 5747 with this payload and these addresses is a case where the
 * sum really does come to zero -- found by search, because sweeping a range
 * and hoping to land on one covers the ordinary path many times and this
 * branch never. */
TEST_CASE("a computed checksum is never sent as zero") {
    const auto zero_case = build({0xDE, 0xAD}, 5747);
    CHECK(at16(zero_case, 40) == 0xFFFF);

    /* And it is still a checksum a receiver accepts, which 0x0000 would not
     * have been: 0xFFFF and 0x0000 are the same value in one's complement. */
    UdpDatagram got{};
    CHECK(udp_parse(zero_case.data(), static_cast<uint16_t>(zero_case.size()),
                    &at(THEIR_IP, 69), &got));

    const auto ordinary = build({1, 2, 3, 4});
    CHECK(at16(ordinary, 40) != 0);
}

TEST_CASE("a LAN's other traffic is turned away") {
    const auto f = build({1, 2, 3, 4});
    UdpDatagram got{};
    const uint16_t n = static_cast<uint16_t>(f.size());

    CHECK_FALSE(udp_parse(f.data(), n, &at(THEIR_IP, 70), &got));       // another port
    CHECK_FALSE(udp_parse(f.data(), n, &at(OUR_IP, 69), &got));         // another host
    CHECK_FALSE(udp_parse(f.data(), UDP_PAYLOAD_AT - 1, &at(THEIR_IP, 69), &got));  // truncated

    auto broken = f;
    broken[13] = 0x06;  // ARP, not IPv4
    CHECK_FALSE(udp_parse(broken.data(), n, &at(THEIR_IP, 69), &got));

    broken = f;
    broken[23] = 6;  // TCP
    CHECK_FALSE(udp_parse(broken.data(), n, &at(THEIR_IP, 69), &got));

    broken = f;
    broken[20] = 0x20;  // more-fragments
    CHECK_FALSE(udp_parse(broken.data(), n, &at(THEIR_IP, 69), &got));

    broken = f;
    broken[21] = 0x01;  // a non-zero fragment offset
    CHECK_FALSE(udp_parse(broken.data(), n, &at(THEIR_IP, 69), &got));

    broken = f;
    broken[14] = 0x46;  // options present, so the offsets would all move
    CHECK_FALSE(udp_parse(broken.data(), n, &at(THEIR_IP, 69), &got));
}

/* A length field is the one number a receiver must not take on trust: it comes
 * off the wire and decides how far the code reads.  These are built by hand
 * with the header checksum repaired, so what turns them away is the bounds
 * test and not some earlier check. */
TEST_CASE("a datagram claiming more than it carries is turned away") {
    auto hostile = [](uint16_t udp_length) {
        auto f = build({1, 2, 3, 4});
        f[38] = static_cast<uint8_t>(udp_length >> 8);
        f[39] = static_cast<uint8_t>(udp_length);
        f[16] = static_cast<uint8_t>((20 + udp_length) >> 8);
        f[17] = static_cast<uint8_t>(20 + udp_length);
        f[40] = 0;  // no UDP checksum, so that cannot be the thing rejecting it
        f[41] = 0;
        f[24] = 0;  // repair the IP header checksum over the edited fields
        f[25] = 0;
        const uint16_t fixed = ip_sum_final(ip_sum(0, f.data() + 14, 20));
        f[24] = static_cast<uint8_t>(fixed >> 8);
        f[25] = static_cast<uint8_t>(fixed);
        return f;
    };

    UdpDatagram got{};
    for (uint16_t claimed : {uint16_t(0xFFFF), uint16_t(0x8000), uint16_t(1400), uint16_t(13)}) {
        CAPTURE(claimed);
        const auto f = hostile(claimed);
        CHECK_FALSE(
            udp_parse(f.data(), static_cast<uint16_t>(f.size()), &at(THEIR_IP, 69), &got));
    }

    /* The honest length still passes, so the bound is not simply refusing
     * everything. */
    const auto ok = hostile(8 + 4);
    CHECK(udp_parse(ok.data(), static_cast<uint16_t>(ok.size()), &at(THEIR_IP, 69), &got));
    CHECK(got.payload_length == 4);
}

/* Ethernet pads anything under 60 bytes, so a short datagram arrives with
 * bytes after it that were never sent. */
TEST_CASE("ethernet padding is not payload") {
    auto f = build({1, 2, 3});
    f.resize(60, 0xAA);  // as the wire delivers it
    UdpDatagram got{};
    REQUIRE(udp_parse(f.data(), 60, &at(THEIR_IP, 69), &got));
    CHECK(got.payload_length == 3);
}

TEST_CASE("a corrupted datagram is rejected") {
    const auto f = build({1, 2, 3, 4});
    UdpDatagram got{};

    auto broken = f;
    broken[25] ^= 0x01;  // a bit of the header
    CHECK_FALSE(
        udp_parse(broken.data(), static_cast<uint16_t>(broken.size()), &at(THEIR_IP, 69), &got));

    broken = f;
    broken[43] ^= 0x01;  // a bit of the payload, which only the UDP checksum covers
    CHECK_FALSE(
        udp_parse(broken.data(), static_cast<uint16_t>(broken.size()), &at(THEIR_IP, 69), &got));
}

/* RFC 768 makes the UDP checksum optional over IPv4, and zero means it was not
 * computed -- so a sender that omits it must still be heard. */
TEST_CASE("a datagram with no UDP checksum is accepted") {
    auto f = build({1, 2, 3, 4});
    f[40] = 0;
    f[41] = 0;
    UdpDatagram got{};
    CHECK(udp_parse(f.data(), static_cast<uint16_t>(f.size()), &at(THEIR_IP, 69), &got));
}

/* Before a lease there is no address to match, and a DHCP offer arrives
 * addressed to somewhere this machine is not yet. */
TEST_CASE("with no address of our own, any destination is ours") {
    const auto f = build({1, 2, 3, 4});
    const std::array<uint8_t, 4> none = {0, 0, 0, 0};
    UdpDatagram got{};
    CHECK(udp_parse(f.data(), static_cast<uint16_t>(f.size()), &at(none, 69), &got));
}

/* A DNS query this code did not build: the bytes are written out by hand and
 * both checksums were computed by a separate implementation, so the offsets
 * and the arithmetic are pinned against something other than udp_build(). */
TEST_CASE("a datagram from elsewhere parses and verifies") {
    const uint8_t elsewhere[] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0x08, 0x00,
        0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46, 0x40, 0x00, 0x40, 0x11, 0x9c, 0xb1, 0xc0, 0xa8,
        0x00, 0x68, 0xc0, 0xa8, 0x00, 0x01, 0xc3, 0x50, 0x00, 0x35, 0x00, 0x28, 0x5e, 0xce,
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x77,
        0x77, 0x77, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d,
        0x00, 0x00, 0x01, 0x00, 0x01};
    const std::array<uint8_t, 4> to = {192, 168, 0, 1};

    /* The header sums to zero, which is the receiver's own test of it. */
    CHECK(sums_to_zero(elsewhere + 14, 20));

    UdpDatagram got{};
    REQUIRE(udp_parse(elsewhere, sizeof elsewhere, &at(to, 53), &got));
    CHECK(got.from.port == 0xc350);
    CHECK(got.payload_length == 0x28 - 8);
    CHECK(elsewhere[UDP_PAYLOAD_AT] == 0x12);  // the DNS transaction id
}

/* An address typed by a person and read off the card, so every way of getting
 * it wrong arrives here rather than on the wire.  A port may follow it, since
 * a gateway on a port of its own is a gateway needing no root to run. */
TEST_CASE("an address is read from text, or refused") {
    std::array<uint8_t, 4> got = {9, 9, 9, 9};
    uint16_t port = 69;

    REQUIRE(ip_parse("192.168.68.57", got.data(), &port));
    CHECK(got == std::array<uint8_t, 4>{192, 168, 68, 57});
    CHECK(port == 69);  // no port named, so the caller's own stands
    CHECK(ip_parse("0.0.0.0", got.data(), &port));
    CHECK(ip_parse("255.255.255.255", got.data(), &port));
    /* A file has a line ending, and an editor may leave a space. */
    CHECK(ip_parse("10.0.0.1\n", got.data(), &port));
    CHECK(ip_parse("10.0.0.1\r\n", got.data(), &port));
    CHECK(ip_parse("10.0.0.1 ", got.data(), &port));
    CHECK(got == std::array<uint8_t, 4>{10, 0, 0, 1});
    CHECK(port == 69);

    /* And with a port, which is the whole point of allowing one. */
    REQUIRE(ip_parse("10.0.0.2:6969", got.data(), &port));
    CHECK(got == std::array<uint8_t, 4>{10, 0, 0, 2});
    CHECK(port == 6969);
    CHECK(ip_parse("10.0.0.2:65535\n", got.data(), &port));
    CHECK(port == 65535);
    /* One named once stays named: a later text without one does not put the
     * reserved port back, it leaves what the caller is holding. */
    CHECK(ip_parse("10.0.0.3", got.data(), &port));
    CHECK(port == 65535);

    /* And everything that is not an address leaves both alone. */
    port = 6969;
    for (const char* bad : {"", "192.168.68", "192.168.68.57.1", "256.1.1.1", "1.2.3.4x",
                            "1..2.3", ".1.2.3", "1.2.3.", "a.b.c.d", "1 2 3 4",
                            "99999.1.1.1", "-1.2.3.4", "1.2.3.4:", "1.2.3.4:0",
                            "1.2.3.4:70000", "1.2.3.4:69x", "1.2.3.4::69"}) {
        CAPTURE(bad);
        CHECK_FALSE(ip_parse(bad, got.data(), &port));
        CHECK(got == std::array<uint8_t, 4>{10, 0, 0, 3});
        CHECK(port == 6969);
    }
}

/* Which address carries the frame, as against which address the datagram is
 * for.  Getting this backwards asks ARP for a machine nobody on the wire holds,
 * and the answer never comes -- so the mask is worth a test of its own rather
 * than only the two obvious cases. */
TEST_CASE("the next hop is the target on this network and the router off it") {
    const std::array<uint8_t, 4> us = {192, 168, 68, 120};
    const std::array<uint8_t, 4> router = {192, 168, 68, 1};
    const std::array<uint8_t, 4> mask24 = {255, 255, 255, 0};

    const std::array<uint8_t, 4> here = {192, 168, 68, 57};
    CHECK(ip_next_hop(here.data(), us.data(), mask24.data(), router.data()) == here.data());

    const std::array<uint8_t, 4> away = {192, 168, 69, 57};
    CHECK(ip_next_hop(away.data(), us.data(), mask24.data(), router.data()) == router.data());

    /* A mask that is not a whole number of bytes, which is where a hand-rolled
     * comparison goes wrong: 192.168.68.120/25 holds .126 and not .130. */
    const std::array<uint8_t, 4> mask25 = {255, 255, 255, 128};
    const std::array<uint8_t, 4> low = {192, 168, 68, 126};
    const std::array<uint8_t, 4> high = {192, 168, 68, 130};
    CHECK(ip_next_hop(low.data(), us.data(), mask25.data(), router.data()) == low.data());
    CHECK(ip_next_hop(high.data(), us.data(), mask25.data(), router.data()) == router.data());

    /* A machine given no mask at all -- a lease that carried none -- reaches
     * everything directly, which is what an all-zero mask means and is better
     * than routing every datagram to an address that may also be zero. */
    const std::array<uint8_t, 4> none = {0, 0, 0, 0};
    CHECK(ip_next_hop(away.data(), us.data(), none.data(), router.data()) == away.data());
}

}  // TEST_SUITE
