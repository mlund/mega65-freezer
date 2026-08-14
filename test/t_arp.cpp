/* ARP frame building and reading, against RFC 826 written out as literals.
 *
 * The oracle is the wire format itself: every offset and constant below is
 * quoted from the RFC rather than derived from arp.c, because the failure this
 * guards against is a frame that is self-consistent and that nothing on the
 * network answers.  There is no emulator route to the wire on macOS, so this
 * is the only check the frame gets before hardware sees it. */

#include <array>
#include <vector>

#include "doctest.h"

extern "C" {
#include "arp.h"
}

namespace {

const std::array<uint8_t, 6> OUR_MAC = {0x02, 0x47, 0x53, 0x11, 0x22, 0x33};
const std::array<uint8_t, 4> NO_IP = {0, 0, 0, 0};
const std::array<uint8_t, 4> GATEWAY = {192, 168, 68, 1};

std::vector<uint8_t> request(const std::array<uint8_t, 4>& sender,
                             const std::array<uint8_t, 4>& target) {
    std::vector<uint8_t> frame(ARP_FRAME_BYTES + 8, 0xEE);
    const uint16_t length = arp_request(frame.data(), OUR_MAC.data(), sender.data(), target.data());
    CHECK(length == ARP_FRAME_BYTES);
    /* Nothing written past the length it reported. */
    for (size_t i = length; i < frame.size(); i++) {
        CHECK(frame[i] == 0xEE);
    }
    frame.resize(length);
    return frame;
}

/* An ARP frame built here rather than by the code under test, so the readers
 * are checked against the format and not against the writer. */
std::vector<uint8_t> arp_frame(uint16_t oper, const std::array<uint8_t, 6>& from_mac,
                               const std::array<uint8_t, 4>& from_ip,
                               const std::array<uint8_t, 4>& to_ip = {0, 0, 0, 0},
                               uint16_t ethertype = 0x0806) {
    const bool asking = oper == 1;
    std::vector<uint8_t> f(ARP_FRAME_BYTES, 0);
    for (int i = 0; i < 6; i++) {
        f[0 + i] = asking ? 0xFF : OUR_MAC[i];  // a question goes to everyone
        f[6 + i] = from_mac[i];
        f[22 + i] = from_mac[i];                       // sender hardware address
        f[32 + i] = asking ? 0 : OUR_MAC[i];           // target: the question itself
    }
    f[12] = static_cast<uint8_t>(ethertype >> 8);
    f[13] = static_cast<uint8_t>(ethertype);
    f[14] = 0x00, f[15] = 0x01;  // htype: ethernet
    f[16] = 0x08, f[17] = 0x00;  // ptype: IPv4
    f[18] = 6, f[19] = 4;        // hlen, plen
    f[20] = static_cast<uint8_t>(oper >> 8);
    f[21] = static_cast<uint8_t>(oper);
    for (int i = 0; i < 4; i++) {
        f[28 + i] = from_ip[i];  // sender protocol address
        f[38 + i] = to_ip[i];    // target protocol address
    }
    return f;
}

std::vector<uint8_t> reply(const std::array<uint8_t, 6>& from_mac,
                           const std::array<uint8_t, 4>& from_ip, uint16_t oper = 2,
                           uint16_t ethertype = 0x0806) {
    return arp_frame(oper, from_mac, from_ip, {0, 0, 0, 0}, ethertype);
}

}  // namespace

TEST_SUITE("arp") {

/* Every field of a request, at the offset RFC 826 puts it. */
TEST_CASE("a request is a broadcast asking who has an address") {
    const auto f = request(NO_IP, GATEWAY);

    for (int i = 0; i < 6; i++) {
        CHECK(f[0 + i] == 0xFF);          // destination: broadcast
        CHECK(f[6 + i] == OUR_MAC[i]);    // source
        CHECK(f[22 + i] == OUR_MAC[i]);   // ARP sender hardware address
        CHECK(f[32 + i] == 0);            // target hardware address: the question
    }
    CHECK(f[12] == 0x08);
    CHECK(f[13] == 0x06);  // ethertype ARP
    CHECK(f[14] == 0x00);
    CHECK(f[15] == 0x01);  // htype ethernet
    CHECK(f[16] == 0x08);
    CHECK(f[17] == 0x00);  // ptype IPv4
    CHECK(f[18] == 6);
    CHECK(f[19] == 4);
    CHECK(f[20] == 0x00);
    CHECK(f[21] == 0x01);  // oper: request
    for (int i = 0; i < 4; i++) {
        CHECK(f[28 + i] == 0);            // sender protocol address
        CHECK(f[38 + i] == GATEWAY[i]);   // target protocol address
    }
}

/* Multi-byte fields are big-endian on the wire and little-endian on this CPU,
 * so a field written through a cast would come out reversed and only hardware
 * would say so. */
TEST_CASE("the two-byte fields are in network order") {
    const auto f = request(NO_IP, GATEWAY);
    CHECK(((f[12] << 8) | f[13]) == 0x0806);
    CHECK(((f[16] << 8) | f[17]) == 0x0800);
}

TEST_CASE("a sender address is carried when there is one") {
    const std::array<uint8_t, 4> ours = {192, 168, 68, 200};
    const auto f = request(ours, GATEWAY);
    for (int i = 0; i < 4; i++) {
        CHECK(f[28 + i] == ours[i]);
    }
}

TEST_CASE("a reply from the address asked about is recognised") {
    const std::array<uint8_t, 6> theirs = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const auto f = reply(theirs, GATEWAY);
    uint8_t got[6] = {};
    REQUIRE(arp_reply_from(f.data(), static_cast<uint16_t>(f.size()), GATEWAY.data(), got));
    for (int i = 0; i < 6; i++) {
        CHECK(got[i] == theirs[i]);
    }
}

/* A LAN is noisy: the probe sees every broadcast on it, so everything that is
 * not the answer has to be turned away. */
TEST_CASE("anything that is not that reply is turned away") {
    const std::array<uint8_t, 6> theirs = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const std::array<uint8_t, 4> someone_else = {192, 168, 68, 99};
    uint8_t got[6] = {};

    const auto other_host = reply(theirs, someone_else);
    CHECK_FALSE(arp_reply_from(other_host.data(), ARP_FRAME_BYTES, GATEWAY.data(), got));

    const auto a_request = reply(theirs, GATEWAY, 1);
    CHECK_FALSE(arp_reply_from(a_request.data(), ARP_FRAME_BYTES, GATEWAY.data(), got));

    const auto not_arp = reply(theirs, GATEWAY, 2, 0x0800);
    CHECK_FALSE(arp_reply_from(not_arp.data(), ARP_FRAME_BYTES, GATEWAY.data(), got));

    const auto full = reply(theirs, GATEWAY);
    CHECK_FALSE(arp_reply_from(full.data(), ARP_FRAME_BYTES - 1, GATEWAY.data(), got));
}

/* Answering matters as soon as this machine has an address: anything sending
 * to it asks this question first, and a machine that stays silent is one whose
 * replies are never delivered. */
TEST_CASE("the answer is the question turned round") {
    const std::array<uint8_t, 6> asker = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const std::array<uint8_t, 4> ours = {192, 168, 68, 60};
    const std::array<uint8_t, 4> theirs = {192, 168, 68, 1};

    /* Answered in the buffer it arrived in, which is all a caller holding one
     * received frame has. */
    auto frame = arp_frame(1, asker, theirs, ours);
    REQUIRE(arp_answer(frame.data(), ARP_FRAME_BYTES, OUR_MAC.data(), ours.data())
            == ARP_FRAME_BYTES);

    for (int i = 0; i < 6; i++) {
        CHECK(frame[0 + i] == asker[i]);     // straight back to whoever asked
        CHECK(frame[6 + i] == OUR_MAC[i]);
        CHECK(frame[22 + i] == OUR_MAC[i]);  // sender hardware address: us
        CHECK(frame[32 + i] == asker[i]);    // target hardware address: them
    }
    CHECK(((frame[20] << 8) | frame[21]) == 2);  // a reply
    for (int i = 0; i < 4; i++) {
        CHECK(frame[28 + i] == ours[i]);    // sender protocol address
        CHECK(frame[38 + i] == theirs[i]);  // target protocol address
    }

    /* And the asker can read it as the answer to what they asked. */
    uint8_t got[6] = {};
    REQUIRE(arp_reply_from(frame.data(), ARP_FRAME_BYTES, ours.data(), got));
    for (int i = 0; i < 6; i++) {
        CHECK(got[i] == OUR_MAC[i]);
    }
}

TEST_CASE("a question about somebody else goes unanswered") {
    const std::array<uint8_t, 6> asker = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const std::array<uint8_t, 4> ours = {192, 168, 68, 60};
    const std::array<uint8_t, 4> theirs = {192, 168, 68, 1};
    const std::array<uint8_t, 4> someone_else = {192, 168, 68, 99};

    auto frame = arp_frame(1, asker, theirs, someone_else);
    CHECK(arp_answer(frame.data(), ARP_FRAME_BYTES, OUR_MAC.data(), ours.data()) == 0);

    /* A reply is not a question, however much of it looks the same. */
    auto answered = arp_frame(2, asker, theirs, ours);
    CHECK(arp_answer(answered.data(), ARP_FRAME_BYTES, OUR_MAC.data(), ours.data()) == 0);

    auto runt = arp_frame(1, asker, theirs, ours);
    CHECK(arp_answer(runt.data(), ARP_FRAME_BYTES - 1, OUR_MAC.data(), ours.data()) == 0);
}

/* Before a lease there is no address to defend, and answering for 0.0.0.0
 * would claim one that is not ours. */
TEST_CASE("nothing is answered before there is an address") {
    const std::array<uint8_t, 6> asker = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const std::array<uint8_t, 4> theirs = {192, 168, 68, 1};

    auto frame = arp_frame(1, asker, theirs, NO_IP);
    CHECK(arp_answer(frame.data(), ARP_FRAME_BYTES, OUR_MAC.data(), NO_IP.data()) == 0);
}

}  // TEST_SUITE
