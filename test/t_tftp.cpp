/* TFTP, against RFC 1350 written out as literals, and against what ether65's
 * docs/FILEHOST.md §4 says a client may rely on.
 *
 * Driven the way a caller drives it -- start, then step with each frame that
 * arrives and on each timeout -- because the sequencing is the whole of what a
 * transfer is: which block is new, which is the server saying it again, and
 * which port the answer goes to after the server moves the transfer to one of
 * its own.
 *
 * Whole frames go in and out, so the server's messages below are wrapped by
 * udp_build().  That is the code under test building the test's input, which is
 * only acceptable because the wrapping is checked independently in t_ip.cpp;
 * what is hand-written here is every byte TFTP itself owns.
 *
 * The block number is the number that decides what reaches the caller, so a
 * repeat that is delivered twice would write the same sector twice into a file
 * being laid down -- the cases below are the point rather than the garnish. */

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

extern "C" {
#include "catalog.h"
#include "ip.h"
#include "tftp.h"
}

/* The two numbers are the same number: a client asks only for `catalog` and for
 * the paths the catalogue named, so a path the catalogue can hold and the
 * client cannot ask for would be refused here with nothing sent -- which reads
 * on screen exactly like a server that never answered.  Pinned in the test
 * rather than in tftp.h, which has no business knowing the catalogue's format. */
static_assert(TFTP_NAME_MAX == CATALOG_PATH_BYTES);

namespace {

const std::array<uint8_t, 6> OUR_MAC = {0x02, 0x47, 0x53, 0x11, 0x22, 0x33};
const std::array<uint8_t, 6> SERVER_MAC = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
const std::array<uint8_t, 4> OUR_IP = {192, 168, 68, 120};
const std::array<uint8_t, 4> SERVER_IP = {192, 168, 68, 57};
const std::array<uint8_t, 4> ELSEWHERE = {192, 168, 68, 99};
constexpr uint16_t SEED = 0xBEEF;
constexpr uint16_t TID = 40000;  // the port a server moves a transfer to
constexpr size_t PAYLOAD = UDP_PAYLOAD_AT;

NetEndpoint endpoint(const std::array<uint8_t, 6>& mac, const std::array<uint8_t, 4>& ip,
                     uint16_t port = 0) {
    NetEndpoint e{};
    std::memcpy(e.mac, mac.data(), 6);
    std::memcpy(e.ip, ip.data(), 4);
    e.port = port;
    return e;
}

/* A NUL-terminated string as a request carries it. */
void put_string(std::vector<uint8_t>& into, const std::string& text) {
    into.insert(into.end(), text.begin(), text.end());
    into.push_back(0);
}

/* The string starting at `at`, as the client wrote it. */
std::string string_at(const std::vector<uint8_t>& p, size_t at) {
    std::string out;
    while (at < p.size() && p[at]) {
        out.push_back(static_cast<char>(p[at++]));
    }
    return out;
}

/* A client mid-transfer, with the two buffers a caller gives it.  Both carry
 * slack past the size the header states, so a frame written too long is caught
 * here rather than by whatever sits after it in far memory. */
struct Client {
    TftpClient c{};
    std::vector<uint8_t> out;

    explicit Client(const char* name = "catalog") : out(TFTP_SEND_BYTES + 8, 0xEE) {
        const NetEndpoint us = endpoint(OUR_MAC, OUR_IP);
        const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP);
        tftp_start(&c, &us, &server, name, SEED);
    }

    /* The TFTP payload the client wants sent now, having been given `in`. */
    std::vector<uint8_t> step(const uint8_t* in, uint16_t length) {
        const uint16_t n = tftp_step(&c, in, length, out.data());
        for (size_t i = TFTP_SEND_BYTES; i < out.size(); i++) {
            CHECK(out[i] == 0xEE);  // nothing written past the buffer it was given
        }
        if (n == 0) {
            return {};
        }
        return std::vector<uint8_t>(out.begin() + PAYLOAD, out.begin() + n);
    }
    std::vector<uint8_t> step(const std::vector<uint8_t>& in) {
        return step(in.data(), static_cast<uint16_t>(in.size()));
    }
    std::vector<uint8_t> timeout() { return step(nullptr, 0); }

    /* A server's message, put on the wire the way a server puts it: from its
     * own address and from the port it chose for this transfer, to the address
     * and port the client asked from. */
    std::vector<uint8_t> from_server(const std::vector<uint8_t>& payload, uint16_t port = TID,
                                     const std::array<uint8_t, 4>& ip = SERVER_IP) {
        std::vector<uint8_t> frame(TFTP_RECEIVE_BYTES, 0);
        const NetEndpoint from = endpoint(SERVER_MAC, ip, port);
        const NetEndpoint to = endpoint(OUR_MAC, OUR_IP, c.us.port);
        const uint16_t n = udp_build(frame.data(), &from, &to, payload.data(),
                                     static_cast<uint16_t>(payload.size()));
        frame.resize(n);
        return frame;
    }

    /* One data block, `bytes` long, filled so its contents are recognisable. */
    std::vector<uint8_t> block(uint16_t number, uint16_t bytes, uint8_t fill = 0xA5,
                               uint16_t port = TID) {
        std::vector<uint8_t> p = {0, 3, static_cast<uint8_t>(number >> 8),
                                  static_cast<uint8_t>(number)};
        p.insert(p.end(), bytes, fill);
        return from_server(p, port);
    }

    /* What the last step delivered, read where the client says it is. */
    std::vector<uint8_t> delivered(const std::vector<uint8_t>& in) const {
        return std::vector<uint8_t>(in.begin() + TFTP_DATA_AT,
                                    in.begin() + TFTP_DATA_AT + c.data_length);
    }

    /* The transfer up to its first block: the request, then a full block 1. */
    std::vector<uint8_t> transferring() {
        REQUIRE_FALSE(timeout().empty());
        return step(block(1, TFTP_BLOCK_BYTES));
    }
};

/* An acknowledgement of `number`, which is the only thing a client sends once
 * the request is out. */
std::vector<uint8_t> ack(uint16_t number) {
    return {0, 4, static_cast<uint8_t>(number >> 8), static_cast<uint8_t>(number)};
}

}  // namespace

TEST_SUITE("tftp") {

/* Every field RFC 1350 fixes, in the order it fixes them, plus the one option
 * FILEHOST.md §4 has the client ask for. */
TEST_CASE("the transfer opens with a read request naming the file") {
    Client client;
    const auto p = client.timeout();
    REQUIRE_FALSE(p.empty());

    CHECK(p[0] == 0);
    CHECK(p[1] == 1);  // read request
    CHECK(string_at(p, 2) == "catalog");
    CHECK(string_at(p, 10) == "octet");  // never netascii: the bytes are a file's, not text
    /* tsize, asked for as zero and answered with the length: how much room to
     * find before the first block is written down. */
    CHECK(string_at(p, 16) == "tsize");
    CHECK(string_at(p, 22) == "0");
    CHECK(p.size() == 24);
    CHECK_FALSE(tftp_done(&client.c));
}

/* The transport is the module's, so it is the module that has to get it right:
 * the reserved port to open with, a port of our own that separates this
 * transfer from the last, and a unicast to the address the caller resolved. */
TEST_CASE("a request travels as RFC 1350 says it must") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());
    const auto& f = client.out;

    for (int i = 0; i < 6; i++) {
        CHECK(f[i] == SERVER_MAC[i]);      // ethernet destination: the server
        CHECK(f[6 + i] == OUR_MAC[i]);     // source: us
    }
    CHECK(((f[12] << 8) | f[13]) == 0x0800);  // ethertype IPv4
    CHECK(f[23] == 17);                       // protocol UDP
    for (int i = 0; i < 4; i++) {
        CHECK(f[26 + i] == OUR_IP[i]);
        CHECK(f[30 + i] == SERVER_IP[i]);
    }
    CHECK(((f[34] << 8) | f[35]) == client.c.us.port);
    CHECK(((f[36] << 8) | f[37]) == 69);  // to the reserved port, this once

    /* And a port of our own that is ours, not something already reserved. */
    CHECK(client.c.us.port >= 49152);
    TftpClient other{};
    const NetEndpoint us = endpoint(OUR_MAC, OUR_IP);
    const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP);
    tftp_start(&other, &us, &server, "catalog", SEED + 1);
    CHECK(other.us.port != client.c.us.port);  // a later run is not this one
}

/* A gateway on a port of its own is a gateway that needs no root, and RFC 1350
 * fixes 69 only as where the request goes. */
TEST_CASE("a server may be asked at a port of its own") {
    TftpClient client{};
    std::vector<uint8_t> out(TFTP_SEND_BYTES + 8, 0xEE);
    const NetEndpoint us = endpoint(OUR_MAC, OUR_IP);
    const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP, 6969);
    tftp_start(&client, &us, &server, "catalog", SEED);

    const uint16_t n = tftp_step(&client, nullptr, 0, out.data());
    REQUIRE(n);
    CHECK(((out[36] << 8) | out[37]) == 6969);
    CHECK(client.server.port == 6969);
}

TEST_CASE("a timeout asks again") {
    Client client;
    const auto first = client.timeout();
    const auto again = client.timeout();
    CHECK(first == again);
}

/* A server that grants the option says so before it sends anything, and that
 * acknowledgement is itself acknowledged -- with block zero, which is what
 * starts the transfer. */
TEST_CASE("an option acknowledgement is answered and read for the file size") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());

    std::vector<uint8_t> oack = {0, 6};
    put_string(oack, "tsize");
    put_string(oack, "33024");
    CHECK(client.step(client.from_server(oack)) == ack(0));
    CHECK(client.c.has_size);
    CHECK(client.c.size == 33024);
    CHECK(client.c.data_length == 0);
    CHECK(client.c.stage == TftpTransferring);

    /* And the block that follows is the first one, delivered and acknowledged. */
    const auto first = client.block(1, TFTP_BLOCK_BYTES);
    CHECK(client.step(first) == ack(1));
    CHECK(client.c.data_length == TFTP_BLOCK_BYTES);
}

/* RFC 2347 lets the server decline every option, in which case the first thing
 * it sends is the first block.  A client that waits for an acknowledgement it
 * will never get transfers nothing. */
TEST_CASE("a server that declines the option is not waited for") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());

    const auto first = client.block(1, TFTP_BLOCK_BYTES, 0x5A);
    CHECK(client.step(first) == ack(1));
    CHECK(client.c.stage == TftpTransferring);
    CHECK_FALSE(client.c.has_size);  // nothing was said, so nothing is claimed
    CHECK(client.c.size == 0);
    CHECK(client.c.data_length == TFTP_BLOCK_BYTES);
    CHECK(client.delivered(first) == std::vector<uint8_t>(TFTP_BLOCK_BYTES, 0x5A));
}

/* The server answers from a port it picked for this transfer, and everything
 * after the request goes there.  Sending to 69 instead reaches a listener that
 * knows nothing of the transfer. */
TEST_CASE("the port the server answered from becomes the transfer's") {
    Client client;
    REQUIRE_FALSE(client.transferring().empty());
    CHECK(client.c.server.port == TID);
    CHECK(((client.out[36] << 8) | client.out[37]) == TID);

    /* And a block from anywhere else is not this transfer's, however well
     * numbered: two transfers to the same machine are told apart by this. */
    CHECK(client.step(client.block(2, TFTP_BLOCK_BYTES, 0xA5, TID + 1)).empty());
    CHECK(client.c.block == 1);
    CHECK(client.c.data_length == 0);
}

TEST_CASE("another machine's server is not ours") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());
    CHECK(client.step(client.from_server({0, 3, 0, 1, 0xFF}, TID, ELSEWHERE)).empty());
    CHECK(client.c.stage == TftpRequesting);
}

/* A lost acknowledgement has the server send the block again.  Acknowledging it
 * again is the whole recovery; delivering it again would write the same bytes
 * into the file twice. */
TEST_CASE("a block the server sent again is acknowledged but not delivered again") {
    Client client;
    REQUIRE_FALSE(client.transferring().empty());

    CHECK(client.step(client.block(1, TFTP_BLOCK_BYTES)) == ack(1));
    CHECK(client.c.data_length == 0);
    CHECK(client.c.block == 1);

    /* And a timeout says the same acknowledgement rather than the request. */
    CHECK(client.timeout() == ack(1));
}

/* Neither the next block nor the last: a stray from further back, which must
 * not be acknowledged -- an acknowledgement of an old block asks the server to
 * send the file from there again. */
TEST_CASE("a block that is neither next nor last is dropped") {
    Client client;
    REQUIRE_FALSE(client.transferring().empty());
    REQUIRE_FALSE(client.step(client.block(2, TFTP_BLOCK_BYTES)).empty());

    CHECK(client.step(client.block(9, TFTP_BLOCK_BYTES)).empty());
    CHECK(client.c.block == 2);
    CHECK(client.c.data_length == 0);
}

/* A block shorter than a whole one is the last, and its acknowledgement is
 * still owed. */
TEST_CASE("a short block ends the transfer, acknowledged like any other") {
    Client client;
    REQUIRE_FALSE(client.transferring().empty());

    const auto last = client.block(2, 100);
    CHECK(client.step(last) == ack(2));
    CHECK(tftp_done(&client.c));
    CHECK(client.c.data_length == 100);
    CHECK(client.delivered(last).size() == 100);
    /* Nothing left to send, and nothing a later timeout should send either. */
    CHECK(client.timeout().empty());
}

/* A file that is an exact multiple of the block size ends with an empty block,
 * which carries no bytes and still ends the transfer. */
TEST_CASE("an empty final block ends the transfer") {
    Client client;
    REQUIRE_FALSE(client.transferring().empty());

    CHECK(client.step(client.block(2, 0)) == ack(2));
    CHECK(tftp_done(&client.c));
    CHECK(client.c.data_length == 0);
}

/* The file is not there, or not one this server will part with.  Sitting in a
 * loop asking again is the failure to avoid. */
TEST_CASE("a refusal ends the transfer with the reason") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());

    std::vector<uint8_t> error = {0, 5, 0, 1};  // file not found
    put_string(error, "no such file");
    CHECK(client.step(client.from_server(error)).empty());
    CHECK(tftp_failed(&client.c));
    CHECK(client.c.error == 1);
    CHECK(client.timeout().empty());
}

/* Code 0 is a refusal like any other -- "not defined, see the message" -- so it
 * must not read as the one refusal the server had no part in. */
TEST_CASE("the server's own reasons are told apart from this client's") {
    Client client;
    REQUIRE_FALSE(client.timeout().empty());

    std::vector<uint8_t> error = {0, 5, 0, 0};
    put_string(error, "go away");
    CHECK(client.step(client.from_server(error)).empty());
    CHECK(tftp_failed(&client.c));
    CHECK(client.c.error == 0);

    const std::string too_long(TFTP_NAME_MAX + 1, 'x');
    Client ours(too_long.c_str());
    CHECK(tftp_failed(&ours.c));
    CHECK(ours.c.error == TFTP_REFUSED);
}

/* Nothing can be fetched before an address has been taken: udp_parse() reads an
 * all-zero address as "accept any destination", which is what a machine waiting
 * for a lease needs and the opposite of what a transfer needs -- every stray
 * datagram on the LAN from that server would be read as this transfer's.  The
 * request would go out from 0.0.0.0 and be unanswerable anyway. */
TEST_CASE("a machine with no address of its own cannot fetch") {
    TftpClient client{};
    const NetEndpoint us = endpoint(OUR_MAC, {0, 0, 0, 0});
    const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP);
    tftp_start(&client, &us, &server, "catalog", SEED);
    CHECK(tftp_failed(&client));
    CHECK(client.error == TFTP_REFUSED);

    std::vector<uint8_t> out(TFTP_SEND_BYTES, 0);
    CHECK(tftp_step(&client, nullptr, 0, out.data()) == 0);
}

/* The transfer is over, and nothing arriving after it may start it again: a
 * caller that writes what each step delivers would run the file past its end,
 * one that steps before it tests would resume a transfer the server refused,
 * and a stray refusal would fail one that arrived whole. */
TEST_CASE("a message after the end does not restart the transfer") {
    {
        Client client;
        REQUIRE_FALSE(client.transferring().empty());
        REQUIRE_FALSE(client.step(client.block(2, 100)).empty());
        REQUIRE(tftp_done(&client.c));

        /* Not an option acknowledgement, which would put it back to
         * transferring with no blocks taken... */
        std::vector<uint8_t> oack = {0, 6};
        put_string(oack, "tsize");
        put_string(oack, "1");
        CHECK(client.step(client.from_server(oack)).empty());
        CHECK(tftp_done(&client.c));
        CHECK_FALSE(client.c.has_size);

        /* ...and not a refusal, which would lose a file already in hand. */
        std::vector<uint8_t> error = {0, 5, 0, 1};
        put_string(error, "gone");
        CHECK(client.step(client.from_server(error)).empty());
        CHECK(tftp_done(&client.c));
        CHECK_FALSE(tftp_failed(&client.c));
    }

    {
        Client client;
        REQUIRE_FALSE(client.transferring().empty());
        REQUIRE_FALSE(client.step(client.block(2, 100)).empty());
        REQUIRE(tftp_done(&client.c));

        CHECK(client.step(client.block(3, TFTP_BLOCK_BYTES)).empty());
        CHECK(tftp_done(&client.c));
        CHECK(client.c.block == 2);
        CHECK(client.c.data_length == 0);
    }

    {
        Client client;
        REQUIRE_FALSE(client.transferring().empty());
        std::vector<uint8_t> error = {0, 5, 0, 2};  // access violation
        put_string(error, "no");
        REQUIRE(client.step(client.from_server(error)).empty());
        REQUIRE(tftp_failed(&client.c));

        CHECK(client.step(client.block(2, TFTP_BLOCK_BYTES)).empty());
        CHECK(tftp_failed(&client.c));
        CHECK(client.c.data_length == 0);
    }
}

/* The one repeat worth answering after the end: our last acknowledgement was
 * lost, so the server is still sending the last block.  RFC 1350 §6 has the
 * client answer it -- a server left waiting records a failed transfer for a
 * file that arrived whole.  A caller that keeps stepping for a moment after
 * tftp_done() is what makes that happen; one that stops loses only the
 * server's opinion of the transfer. */
TEST_CASE("the last block sent again is acknowledged after the transfer ends") {
    Client client;
    REQUIRE_FALSE(client.transferring().empty());
    const auto last = client.block(2, 100);
    REQUIRE_FALSE(client.step(last).empty());
    REQUIRE(tftp_done(&client.c));

    CHECK(client.step(last) == ack(2));
    CHECK(client.c.data_length == 0);  // not written into the file twice
    CHECK(tftp_done(&client.c));
    /* And a timeout after that still has nothing to say. */
    CHECK(client.timeout().empty());
}

/* A path that will not fit cannot be asked for, and asking for a truncated one
 * asks for a different file. */
TEST_CASE("a name too long is refused before anything is sent") {
    const std::string too_long(TFTP_NAME_MAX + 1, 'x');
    Client client(too_long.c_str());
    CHECK(tftp_failed(&client.c));
    CHECK(client.c.error == TFTP_REFUSED);  // ours, not the server's
    CHECK(client.timeout().empty());

    /* And the longest one that does fit is asked for whole. */
    const std::string longest(TFTP_NAME_MAX, 'x');
    Client fits(longest.c_str());
    const auto p = fits.timeout();
    REQUIRE_FALSE(p.empty());
    CHECK(string_at(p, 2) == longest);
}

/* Everything below comes off the wire and decides how far a walk reads or what
 * a caller is told arrived. */
TEST_CASE("a malformed message is turned away") {
    /* An option list whose last string never ends: the walk must stop at the
     * payload rather than read past it. */
    {
        Client client;
        REQUIRE_FALSE(client.timeout().empty());
        std::vector<uint8_t> oack = {0, 6};
        put_string(oack, "tsize");
        oack.insert(oack.end(), {'3', '3', '0'});  // no terminator
        CHECK(client.step(client.from_server(oack)) == ack(0));
        CHECK_FALSE(client.c.has_size);
    }

    /* An option name with no value after it at all. */
    {
        Client client;
        REQUIRE_FALSE(client.timeout().empty());
        std::vector<uint8_t> oack = {0, 6};
        put_string(oack, "tsize");
        CHECK(client.step(client.from_server(oack)) == ack(0));
        CHECK_FALSE(client.c.has_size);
    }

    /* RFC 2347 makes option names case-insensitive, and a server may echo one
     * back in any case it likes. */
    {
        Client client;
        REQUIRE_FALSE(client.timeout().empty());
        std::vector<uint8_t> oack = {0, 6};
        put_string(oack, "TSize");
        put_string(oack, "819200");
        REQUIRE_FALSE(client.step(client.from_server(oack)).empty());
        CHECK(client.c.has_size);
        CHECK(client.c.size == 819200);  // a .d81, and past sixteen bits
    }

    /* An option this client did not ask for is ignored rather than believed. */
    {
        Client client;
        REQUIRE_FALSE(client.timeout().empty());
        std::vector<uint8_t> oack = {0, 6};
        put_string(oack, "blksize");
        put_string(oack, "1468");
        REQUIRE_FALSE(client.step(client.from_server(oack)).empty());
        CHECK_FALSE(client.c.has_size);
    }

    /* A datagram too short to carry an opcode and a block number, and one
     * whose opcode is none of TFTP's. */
    {
        Client client;
        REQUIRE_FALSE(client.transferring().empty());
        CHECK(client.step(client.from_server({0, 3, 0})).empty());
        CHECK(client.step(client.from_server({0, 9, 0, 2, 0xFF})).empty());
        CHECK(client.c.block == 1);
    }

    /* A block longer than one was ever asked for: no blksize was negotiated,
     * so a server sending 1468 bytes is one whose blocks a caller sized its
     * sector buffer against would overrun. */
    {
        Client client;
        REQUIRE_FALSE(client.transferring().empty());
        CHECK(client.step(client.block(2, TFTP_BLOCK_BYTES + 1)).empty());
        CHECK(client.c.block == 1);
        CHECK(client.c.data_length == 0);
    }
}

}  // TEST_SUITE
