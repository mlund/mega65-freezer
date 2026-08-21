/* DHCP, against RFC 2131 and RFC 2132 written out as literals.
 *
 * Driven through the client the way a caller drives it -- start, then step with
 * each frame that arrives and on each timeout -- because the sequencing is half
 * of what can go wrong and the only other place it runs is on hardware.
 *
 * Whole frames go in and out, so the replies below are wrapped by udp_build().
 * That is the code under test building the test's input, which is only
 * acceptable because the wrapping is checked independently in t_ip.cpp against
 * the RFCs; what is hand-written here is every byte DHCP itself owns.
 *
 * Two things earn most of the space.  A reply arrives as a broadcast, so every
 * other client's exchange on the LAN lands in our receive buffer too and has to
 * be turned away by the transaction id and the hardware address -- accepting
 * one would configure this machine with somebody else's address.  And an option
 * carries a length off the wire, which is the number that decides how far the
 * walk reads; the IPv4 layer already had one bug of exactly that shape, so the
 * malformed cases below are the point rather than the garnish. */

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "doctest.h"

extern "C" {
#include "dhcp.h"
#include "ip.h"
}

namespace {

const std::array<uint8_t, 6> OUR_MAC = {0x02, 0x47, 0x53, 0x11, 0x22, 0x33};
const std::array<uint8_t, 6> SERVER_MAC = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
const std::array<uint8_t, 4> SERVER_IP = {192, 168, 68, 1};
constexpr uint16_t SEED = 0xBEEF;
/* Where the DHCP payload starts inside a frame, so the assertions below can be
 * written at the offsets RFC 2131 states rather than at those plus a header. */
constexpr size_t PAYLOAD = UDP_PAYLOAD_AT;

/* A reply put on the wire the way a server puts it: from the server's address
 * and ports, broadcast, because a client with no address cannot be unicast to. */
std::vector<uint8_t> as_frame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame(DHCP_FRAME_BYTES, 0);
    NetEndpoint from{};
    NetEndpoint to{};
    std::memcpy(from.mac, SERVER_MAC.data(), 6);
    std::memcpy(from.ip, SERVER_IP.data(), 4);
    from.port = 67;
    std::memset(to.mac, 0xFF, 6);
    std::memset(to.ip, 0xFF, 4);
    to.port = 68;
    const uint16_t n = udp_build(frame.data(), &from, &to, payload.data(),
                                 static_cast<uint16_t>(payload.size()));
    frame.resize(n);
    return frame;
}

/* A server's reply, built here rather than by the code under test. */
struct Reply {
    std::vector<uint8_t> bytes;

    Reply(uint8_t type, const uint8_t* xid, const std::array<uint8_t, 6>& mac = OUR_MAC)
        : bytes(DHCP_FRAME_BYTES - PAYLOAD, 0) {
        bytes[0] = 2;  // BOOTREPLY
        bytes[1] = 1;  // ethernet
        bytes[2] = 6;  // six-byte hardware address
        std::memcpy(&bytes[4], xid, 4);
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
    /* The reply as it reaches a client, optionally cut short. */
    std::vector<uint8_t> frame(size_t payload_bytes = 0) {
        auto p = bytes;
        if (payload_bytes) {
            p.resize(payload_bytes);
        }
        return as_frame(p);
    }
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

/* A client mid-exchange, with the frame buffer it builds into.  The buffer
 * carries slack past DHCP_FRAME_BYTES so a frame written too long is caught
 * here rather than by whatever sits after it in far memory. */
struct Client {
    DhcpClient c{};
    std::vector<uint8_t> out;

    Client() : out(DHCP_FRAME_BYTES + 8, 0xEE) { dhcp_start(&c, OUR_MAC.data(), SEED); }

    /* The DHCP payload the client wants sent now, having been given `in`.  An
     * empty `in` is a timeout: say the current step again. */
    std::vector<uint8_t> step(const uint8_t* in, uint16_t length) {
        const uint16_t n = dhcp_step(&c, in, length, out.data());
        for (size_t i = DHCP_FRAME_BYTES; i < out.size(); i++) {
            CHECK(out[i] == 0xEE);  // nothing written past the buffer it was given
        }
        if (n == 0) {
            return {};
        }
        CHECK(n == DHCP_FRAME_BYTES);
        return std::vector<uint8_t>(out.begin() + PAYLOAD, out.begin() + n);
    }
    std::vector<uint8_t> step(const std::vector<uint8_t>& in) {
        return step(in.data(), static_cast<uint16_t>(in.size()));
    }
    std::vector<uint8_t> timeout() { return step(nullptr, 0); }

    /* The offer that carries an exchange to the point where a request is the
     * next thing to send, and the request it produced. */
    std::vector<uint8_t> offered(uint8_t server_last = 1) {
        Reply r(2, c.xid);
        r.option(54, {192, 168, 68, server_last});
        r.end();
        return step(r.frame());
    }
};

}  // namespace

TEST_SUITE("dhcp") {

/* Every field RFC 2131 fixes, at the offset it fixes it. */
TEST_CASE("the exchange opens with a discover saying who is asking") {
    Client client;
    const auto p = client.timeout();
    REQUIRE_FALSE(p.empty());

    CHECK(p[0] == 1);   // BOOTREQUEST
    CHECK(p[1] == 1);   // htype ethernet
    CHECK(p[2] == 6);   // hlen
    /* The identifier the client chose for itself: read back rather than
     * assumed, because choosing it is the client's business and not a test's. */
    for (int i = 0; i < 4; i++) {
        CHECK(p[4 + i] == client.c.xid[i]);
    }
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
    CHECK_FALSE(dhcp_leased(&client.c));

    /* The parameter list has to ask for a router, or a machine that can reach
     * its own subnet and nothing beyond it looks exactly like a working one --
     * and the proxy is never on the subnet.
     *
     * Nothing asks for 150 any more: it names a TFTP server, and nothing
     * fetches over TFTP. */
    const auto wanted = option_of(p, 55);
    REQUIRE_FALSE(wanted.empty());
    CHECK(std::find(wanted.begin(), wanted.end(), 3) != wanted.end());
    CHECK(std::find(wanted.begin(), wanted.end(), 1) != wanted.end());
    CHECK(std::find(wanted.begin(), wanted.end(), 150) == wanted.end());
}

/* The transport is the module's now, so it is the module that has to get it
 * right: the reserved ports both ways, and a broadcast at both layers because
 * a client with no address of its own cannot be reached any other way. */
TEST_CASE("a message travels as RFC 2131 says it must") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());
    const auto& f = client.out;

    for (int i = 0; i < 6; i++) {
        CHECK(f[i] == 0xFF);              // ethernet destination: everyone
        CHECK(f[6 + i] == OUR_MAC[i]);    // source: us
    }
    CHECK(((f[12] << 8) | f[13]) == 0x0800);  // ethertype IPv4
    CHECK(f[23] == 17);                       // protocol UDP
    for (int i = 0; i < 4; i++) {
        CHECK(f[26 + i] == 0);            // source address: none yet
        CHECK(f[30 + i] == 0xFF);         // destination: the broadcast address
    }
    CHECK(((f[34] << 8) | f[35]) == 68);  // from the client port
    CHECK(((f[36] << 8) | f[37]) == 67);  // to the server port
}

/* Two machines starting at the same moment must not read each other's replies,
 * and neither must two runs of the same machine. */
TEST_CASE("the identifier separates machines and runs") {
    const std::array<uint8_t, 6> other = {0x02, 0x47, 0x53, 0x11, 0x99, 0x88};
    DhcpClient a{};
    DhcpClient b{};
    DhcpClient again{};
    dhcp_start(&a, OUR_MAC.data(), SEED);
    dhcp_start(&b, other.data(), SEED);
    dhcp_start(&again, OUR_MAC.data(), SEED + 1);
    CHECK(std::memcmp(a.xid, b.xid, 4) != 0);      // different machines
    CHECK(std::memcmp(a.xid, again.xid, 4) != 0);  // different runs
}

TEST_CASE("a timeout says the same thing again") {
    Client client;
    const auto first = client.timeout();
    const auto again = client.timeout();
    CHECK(first == again);  // still discovering, so still a discover
    CHECK_FALSE(dhcp_leased(&client.c));
}

TEST_CASE("an offer is answered with a request naming it and its server") {
    Client client;
    const std::array<uint8_t, 4> ip = {192, 168, 68, 120};
    const std::array<uint8_t, 4> server = {192, 168, 68, 1};
    const auto p = client.offered();

    REQUIRE_FALSE(p.empty());
    CHECK(p[242] == 3);  // DHCPREQUEST
    /* Both options must be there: without the server identifier every server
     * on the LAN thinks the request is for it. */
    CHECK(option_of(p, 50) == std::vector<uint8_t>(ip.begin(), ip.end()));
    CHECK(option_of(p, 54) == std::vector<uint8_t>(server.begin(), server.end()));

    /* And a timeout now repeats the request, not the discover. */
    CHECK(client.timeout() == p);
}

TEST_CASE("an offer is read for everything it carries") {
    Client client;
    Reply r(2, client.c.xid);  // DHCPOFFER
    r.option(1, {255, 255, 255, 0});
    r.option(3, {192, 168, 68, 1});
    r.option(6, {192, 168, 68, 1});
    r.option(54, {192, 168, 68, 1});
    /* An option this client does not store, which must be walked past rather
     * than mistaken for one it does: 150 named a TFTP server, and nothing
     * fetches over TFTP any more. */
    r.option(150, {192, 168, 68, 57});
    r.end();

    REQUIRE_FALSE(client.step(r.frame()).empty());
    CHECK(client.c.lease.ip[3] == 120);
    CHECK(client.c.lease.netmask[3] == 0);
    CHECK(client.c.lease.router[3] == 1);
    CHECK(client.c.lease.dns[3] == 1);
    CHECK(client.c.lease.server[3] == 1);
}

TEST_CASE("an acknowledgement ends the exchange") {
    Client client;
    REQUIRE_FALSE(client.offered().empty());

    Reply ack(5, client.c.xid);
    ack.option(54, {192, 168, 68, 1});
    ack.end();
    /* Nothing left to send, and nothing a later timeout should send either. */
    CHECK(client.step(ack.frame()).empty());
    CHECK(dhcp_leased(&client.c));
    CHECK(client.timeout().empty());
    CHECK(client.c.lease.ip[3] == 120);
}

/* RFC 2131 has no acknowledgement that answers a discover, so one arriving
 * before any request was sent is a stray from an earlier exchange -- and would
 * install a lease this client never asked for. */
TEST_CASE("an acknowledgement nobody asked for is refused") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());

    Reply ack(5, client.c.xid);
    ack.option(54, {192, 168, 68, 1});
    ack.end();
    CHECK(client.step(ack.frame()).empty());
    CHECK_FALSE(dhcp_leased(&client.c));
    CHECK(client.c.lease.ip[3] == 0);
    /* And it is still discovering, so a timeout still asks. */
    const auto p = client.timeout();
    REQUIRE_FALSE(p.empty());
    CHECK(p[242] == 1);
}

/* An option carrying an address this client has no field for must be stepped
 * over by its stated length like any other.  Read as one it does store, the
 * lease comes back naming somebody else's machine as the router. */
TEST_CASE("an option this client does not store is walked past") {
    Client client;
    REQUIRE_FALSE(client.offered().empty());

    Reply ack(5, client.c.xid);
    ack.option(150, {10, 9, 8, 7});
    ack.option(3, {192, 168, 68, 1});
    ack.option(54, {192, 168, 68, 1});
    ack.end();
    CHECK(client.step(ack.frame()).empty());
    REQUIRE(dhcp_leased(&client.c));
    CHECK(client.c.lease.router[3] == 1);
    CHECK(client.c.lease.ip[3] == 120);
}

/* The address asked for is not ours to have.  Sitting in the requesting stage
 * repeating a request nobody will grant is the failure to avoid. */
TEST_CASE("a refusal sends the exchange back to the start") {
    Client client;
    REQUIRE_FALSE(client.offered().empty());

    Reply nak(6, client.c.xid);
    nak.end();
    const auto p = client.step(nak.frame());
    REQUIRE_FALSE(p.empty());
    CHECK(p[242] == 1);  // discovering again
    CHECK_FALSE(dhcp_leased(&client.c));
}

/* Two servers answering is ordinary, and the second's offer arrives after the
 * first has been accepted.  Letting it land would ask one server for the
 * other's address, which is refused by both. */
TEST_CASE("a second server's offer does not displace the one being requested") {
    Client client;
    const auto asked = client.offered(1);
    REQUIRE_FALSE(asked.empty());

    CHECK(client.offered(9).empty());
    CHECK(client.c.lease.server[3] == 1);
    CHECK(client.timeout() == asked);
}

TEST_CASE("another machine's exchange is not ours") {
    Client client;

    const uint8_t other_xid[4] = {0x12, 0x34, 0x56, 0x78};
    Reply not_ours(2, other_xid);
    not_ours.end();
    CHECK(client.step(not_ours.frame()).empty());

    const std::array<uint8_t, 6> other = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    Reply other_mac(2, client.c.xid, other);
    other_mac.end();
    CHECK(client.step(other_mac.frame()).empty());

    /* Still where it was, so a timeout still asks the same question. */
    CHECK_FALSE(client.timeout().empty());
    CHECK_FALSE(dhcp_leased(&client.c));
}

TEST_CASE("anything that is not a reply is turned away") {
    Client client;

    Reply r(2, client.c.xid);
    r.end();

    auto no_cookie = r.bytes;
    no_cookie[236] = 0;
    CHECK(client.step(as_frame(no_cookie)).empty());

    auto a_request = r.bytes;
    a_request[0] = 1;  // BOOTREQUEST: our own message, echoed back to us
    CHECK(client.step(as_frame(a_request)).empty());

    /* No message-type option at all is BOOTP, not DHCP. */
    auto bootp = r.bytes;
    bootp[240] = 255;
    CHECK(client.step(as_frame(bootp)).empty());

    CHECK(client.step(r.frame(100)).empty());
    CHECK_FALSE(dhcp_leased(&client.c));
}

/* An option length is read off the wire and decides how far the walk goes.
 * None of these may read past the payload, and none may loop.  Each starts a
 * fresh client, because what is being checked is what the walk stored. */
TEST_CASE("a malformed option list is walked safely") {
    /* A length reaching past the end of the payload.  The message type before
     * it is honest, so the reply still reads as an offer -- what must not
     * happen is the walk following the length off the end, and what must not
     * be believed is the option it introduces. */
    {
        Client client;
        Reply overrun(2, client.c.xid);
        overrun.bytes[overrun.at++] = 3;
        overrun.bytes[overrun.at++] = 255;  // claims 255 bytes with far fewer left
        CHECK_FALSE(client.step(overrun.frame(250)).empty());
        CHECK(client.c.lease.router[0] == 0);
    }

    /* An option header cut off by the end of the payload: a number with no
     * length byte after it. */
    {
        Client client;
        Reply truncated(2, client.c.xid);
        truncated.bytes[truncated.at++] = 3;
        CHECK_FALSE(client.step(truncated.frame(truncated.at)).empty());
        CHECK(client.c.lease.router[0] == 0);
    }

    /* Pad bytes are one byte each and must not be read as a length. */
    {
        Client client;
        Reply padded(2, client.c.xid);
        for (int i = 0; i < 10; i++) {
            padded.bytes[padded.at++] = 0;
        }
        padded.option(54, {192, 168, 68, 1});
        padded.end();
        CHECK_FALSE(client.step(padded.frame()).empty());
        CHECK(client.c.lease.server[3] == 1);
    }

    /* An option whose value is shorter than the field it carries. */
    {
        Client client;
        Reply short_ip(2, client.c.xid);
        short_ip.option(3, {192, 168});
        short_ip.option(54, {192, 168, 68, 1});
        short_ip.end();
        CHECK_FALSE(client.step(short_ip.frame()).empty());
        CHECK(client.c.lease.router[0] == 0);  // ignored rather than half-copied
    }
}

}  // TEST_SUITE
