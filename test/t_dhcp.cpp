/* DHCP, against RFC 2131 and RFC 2132 written out as literals.
 *
 * Two things earn most of the space here.  A reply arrives as a broadcast, so
 * every other client's exchange on the LAN lands in our receive buffer too and
 * has to be turned away by the transaction id and the hardware address --
 * accepting one would configure this machine with somebody else's address.
 * And an option carries a length off the wire, which is the number that
 * decides how far the walk reads; the IPv4 layer already had one bug of
 * exactly that shape, so the malformed cases below are the point rather than
 * the garnish. */

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "doctest.h"

extern "C" {
#include "dhcp.h"
}

namespace {

const std::array<uint8_t, 6> OUR_MAC = {0x02, 0x47, 0x53, 0x11, 0x22, 0x33};
constexpr uint32_t XID = 0xDEADBEEF;

std::vector<uint8_t> discover() {
    std::vector<uint8_t> p(DHCP_PAYLOAD_BYTES + 8, 0xEE);
    const uint16_t n = dhcp_discover(p.data(), OUR_MAC.data(), XID);
    CHECK(n == DHCP_PAYLOAD_BYTES);
    for (size_t i = n; i < p.size(); i++) {
        CHECK(p[i] == 0xEE);  // nothing written past the length it reported
    }
    p.resize(n);
    return p;
}

/* A server's reply, built here rather than by the code under test. */
struct Reply {
    std::vector<uint8_t> bytes;

    Reply(uint8_t type, uint32_t xid = XID,
          const std::array<uint8_t, 6>& mac = OUR_MAC) : bytes(DHCP_PAYLOAD_BYTES, 0) {
        bytes[0] = 2;  // BOOTREPLY
        bytes[1] = 1;  // ethernet
        bytes[2] = 6;  // six-byte hardware address
        bytes[4] = static_cast<uint8_t>(xid >> 24);
        bytes[5] = static_cast<uint8_t>(xid >> 16);
        bytes[6] = static_cast<uint8_t>(xid >> 8);
        bytes[7] = static_cast<uint8_t>(xid);
        bytes[16] = 192; bytes[17] = 168; bytes[18] = 68; bytes[19] = 120;  // yiaddr
        std::memcpy(&bytes[28], mac.data(), 6);
        bytes[236] = 99; bytes[237] = 130; bytes[238] = 83; bytes[239] = 99;  // magic cookie
        at = 240;
        option(53, {type});
    }

    void option(uint8_t number, const std::vector<uint8_t>& value) {
        bytes[at++] = number;
        bytes[at++] = static_cast<uint8_t>(value.size());
        for (uint8_t v : value) {
            bytes[at++] = v;
        }
    }
    void end() { bytes[at++] = 255; }
    size_t at = 0;
};

/* The option a message carries, or nothing.  Written once: a test that
 * hand-rolls this walk twice can have the very bug it is checking for. */
std::vector<uint8_t> option_of(const std::vector<uint8_t>& p, uint8_t number) {
    for (size_t i = 240; i < p.size() && p[i] != 255;) {
        if (p[i] == 0) {
            i++;
            continue;
        }
        const uint8_t found = p[i], length = p[i + 1];
        if (found == number) {
            return std::vector<uint8_t>(p.begin() + i + 2, p.begin() + i + 2 + length);
        }
        i += 2 + length;
    }
    return {};
}

}  // namespace

TEST_SUITE("dhcp") {

/* Every field RFC 2131 fixes, at the offset it fixes it. */
TEST_CASE("a discover says who is asking and what it wants") {
    const auto p = discover();

    CHECK(p[0] == 1);   // BOOTREQUEST
    CHECK(p[1] == 1);   // htype ethernet
    CHECK(p[2] == 6);   // hlen
    CHECK(p[4] == 0xDE);
    CHECK(p[5] == 0xAD);
    CHECK(p[6] == 0xBE);
    CHECK(p[7] == 0xEF);  // xid, big-endian
    /* The broadcast flag: there is no address yet for a reply to be sent to. */
    CHECK(p[10] == 0x80);
    CHECK(p[11] == 0x00);
    for (int i = 0; i < 6; i++) {
        CHECK(p[28 + i] == OUR_MAC[i]);
    }
    CHECK(p[236] == 99);
    CHECK(p[237] == 130);
    CHECK(p[238] == 83);
    CHECK(p[239] == 99);  // the magic cookie that makes it DHCP and not BOOTP
    CHECK(p[240] == 53);  // first option: message type
    CHECK(p[241] == 1);
    CHECK(p[242] == 1);   // DHCPDISCOVER

    /* The parameter list has to ask for a TFTP server, or no server will
     * volunteer one and the whole reason for reading option 150 is gone. */
    const auto wanted = option_of(p, 55);
    REQUIRE_FALSE(wanted.empty());
    CHECK(std::find(wanted.begin(), wanted.end(), 150) != wanted.end());
    CHECK(std::find(wanted.begin(), wanted.end(), 3) != wanted.end());
}

TEST_CASE("a request names the address and the server that offered it") {
    DhcpLease offer{};
    const std::array<uint8_t, 4> ip = {192, 168, 68, 120};
    const std::array<uint8_t, 4> server = {192, 168, 68, 1};
    std::memcpy(offer.ip, ip.data(), 4);
    std::memcpy(offer.server, server.data(), 4);

    std::vector<uint8_t> p(DHCP_PAYLOAD_BYTES);
    CHECK(dhcp_request(p.data(), OUR_MAC.data(), XID, &offer) == DHCP_PAYLOAD_BYTES);
    CHECK(p[242] == 3);  // DHCPREQUEST

    /* Both options must be there: without the server identifier every server
     * on the LAN thinks the request is for it. */
    CHECK(option_of(p, 50) == std::vector<uint8_t>(ip.begin(), ip.end()));
    CHECK(option_of(p, 54) == std::vector<uint8_t>(server.begin(), server.end()));
}

TEST_CASE("an offer is read for everything it carries") {
    Reply r(2);  // DHCPOFFER
    r.option(1, {255, 255, 255, 0});
    r.option(3, {192, 168, 68, 1});
    r.option(6, {192, 168, 68, 1});
    r.option(54, {192, 168, 68, 1});
    r.option(150, {192, 168, 68, 57});
    r.end();

    DhcpLease lease{};
    CHECK(dhcp_parse(r.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpOffer);
    CHECK(lease.ip[3] == 120);
    CHECK(lease.netmask[3] == 0);
    CHECK(lease.router[3] == 1);
    CHECK(lease.dns[3] == 1);
    CHECK(lease.server[3] == 1);
    CHECK(lease.has_tftp);
    CHECK(lease.tftp[3] == 57);
}

/* Most servers say nothing about TFTP, and that must not read as a server at
 * address zero. */
TEST_CASE("a lease without a TFTP server says so") {
    Reply r(5);  // DHCPACK
    r.option(54, {192, 168, 68, 1});
    r.end();

    DhcpLease lease{};
    CHECK(dhcp_parse(r.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease) == DhcpAck);
    CHECK_FALSE(lease.has_tftp);
}

/* A refusal carries no lease, and must not take away one already held: the
 * client sits with an accepted offer while it waits for the acknowledgement,
 * and a second server on the LAN refusing the same exchange arrives in the
 * middle of that wait. */
TEST_CASE("a refusal is not mistaken for a lease, and does not erase one") {
    DhcpLease lease{};
    Reply offer(2);
    offer.option(54, {192, 168, 68, 1});
    offer.end();
    REQUIRE(dhcp_parse(offer.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
            == DhcpOffer);
    CHECK(lease.ip[3] == 120);

    Reply nak(6);
    nak.end();
    CHECK(dhcp_parse(nak.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease) == DhcpNak);
    CHECK(lease.ip[3] == 120);
    CHECK(lease.server[3] == 1);

    /* And the same for a reply carrying our exchange id but nothing usable. */
    Reply mute(2);
    mute.bytes[240] = 255;  // no message type at all
    CHECK(dhcp_parse(mute.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpNothing);
    CHECK(lease.ip[3] == 120);
}

TEST_CASE("another machine's exchange is not ours") {
    DhcpLease lease{};

    Reply other_xid(2, 0x12345678);
    other_xid.end();
    CHECK(dhcp_parse(other_xid.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpNothing);

    const std::array<uint8_t, 6> other = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    Reply other_mac(2, XID, other);
    other_mac.end();
    CHECK(dhcp_parse(other_mac.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpNothing);
}

TEST_CASE("anything that is not a reply is turned away") {
    DhcpLease lease{};

    Reply r(2);
    r.end();

    auto no_cookie = r.bytes;
    no_cookie[236] = 0;
    CHECK(dhcp_parse(no_cookie.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpNothing);

    auto a_request = r.bytes;
    a_request[0] = 1;  // BOOTREQUEST: our own message, echoed back to us
    CHECK(dhcp_parse(a_request.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpNothing);

    /* No message-type option at all is BOOTP, not DHCP. */
    auto bootp = r.bytes;
    bootp[240] = 255;
    CHECK(dhcp_parse(bootp.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpNothing);

    CHECK(dhcp_parse(r.bytes.data(), 100, XID, OUR_MAC.data(), &lease) == DhcpNothing);
}

/* An option length is read off the wire and decides how far the walk goes.
 * None of these may read past the payload, and none may loop. */
TEST_CASE("a malformed option list is walked safely") {
    DhcpLease lease{};

    /* A length reaching past the end of the payload.  The message type before
     * it is honest, so the reply still reads as an offer -- what must not
     * happen is the walk following the length off the end, and what must not
     * be believed is the option it introduces. */
    Reply overrun(2);
    overrun.bytes[overrun.at++] = 3;
    overrun.bytes[overrun.at++] = 255;  // claims 255 bytes with far fewer left
    lease = DhcpLease{};
    CHECK(dhcp_parse(overrun.bytes.data(), 250, XID, OUR_MAC.data(), &lease) == DhcpOffer);
    CHECK(lease.router[0] == 0);

    /* An option header cut off by the end of the payload: a number with no
     * length byte after it. */
    Reply truncated(2);
    truncated.bytes[truncated.at++] = 3;
    lease = DhcpLease{};
    CHECK(dhcp_parse(truncated.bytes.data(), static_cast<uint16_t>(truncated.at), XID,
                     OUR_MAC.data(), &lease) == DhcpOffer);
    CHECK(lease.router[0] == 0);

    /* Pad bytes are one byte each and must not be read as a length. */
    Reply padded(2);
    for (int i = 0; i < 10; i++) {
        padded.bytes[padded.at++] = 0;
    }
    padded.option(54, {192, 168, 68, 1});
    padded.end();
    CHECK(dhcp_parse(padded.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpOffer);
    CHECK(lease.server[3] == 1);

    /* An option whose value is shorter than the field it carries. */
    Reply short_ip(2);
    short_ip.option(3, {192, 168});
    short_ip.option(54, {192, 168, 68, 1});
    short_ip.end();
    CHECK(dhcp_parse(short_ip.bytes.data(), DHCP_PAYLOAD_BYTES, XID, OUR_MAC.data(), &lease)
          == DhcpOffer);
    CHECK(lease.router[0] == 0);  // ignored rather than half-copied
}

}  // TEST_SUITE
