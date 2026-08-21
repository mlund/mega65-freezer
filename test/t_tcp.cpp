/* TCP, against RFC 793 written out as literals.
 *
 * Driven the way a caller drives it -- start, then step with each frame that
 * arrives and on each clock tick -- because the sequencing is the whole of
 * what a connection is: which segment is new, which is the server saying it
 * again, and which sequence number the answer carries.
 *
 * The server's segments are hand-written here, header byte by header byte,
 * because that is what TCP itself owns.  Only the ethernet and IPv4 wrapping
 * and the one's-complement sum come from the code under test, and both are
 * pinned independently in t_ip.cpp -- against RFC 1071's worked example and
 * against a receiver's own check that a header sums to zero.
 *
 * Three of the cases below are here because WeeIP, the 6502 stack this could
 * have been a port of, gets them wrong -- and the bugs are invisible until
 * they are not.  Checked against its source rather than taken on trust:
 *
 *   - a segment carrying options: nwk.c:669 is `data_size -= 40`, above a
 *     comment of its own admitting it assumes there are none;
 *   - the SYN's one sequence number, spent in two conditional places at
 *     nwk.c:436-437 rather than once;
 *   - an inbound checksum: nwk.c:573-577 verifies the IPv4 header, and
 *     nothing anywhere verifies TCP's.
 *
 * The wrap is tested here too, but not for that reason: nwk.c:660 subtracts
 * before comparing, which is right, and it is the case a test is most likely
 * to be missing rather than the case somebody got wrong.
 *
 * A wrong payload length copies option bytes into the file, and nothing
 * reports it. */

#include <array>
#include <cstring>
#include <vector>

#include "doctest.h"
#include "tcpwire.h"

extern "C" {
#include "tcp.h"
}

namespace {

using namespace wire;

constexpr uint16_t SEED = 0xBEEF;

/* What the request is here: its bytes do not matter to TCP, only its length,
 * which is the sequence space it occupies. */
const std::array<uint8_t, 12> REQUEST = {'G', 'E', 'T', ' ', '/', '\r', '\n', '\r',
                                         '\n', 'x', 'y', 'z'};

/* A client mid-connection, with the buffer a caller gives it.  The buffer
 * carries slack past the largest frame this can build, so a frame written too
 * long is caught here rather than by whatever sits after it in far memory. */
constexpr size_t SEND_ROOM = TCP_PAYLOAD_AT + REQUEST.size() + TCP_OPTION_BYTES;

struct Client {
    TcpClient c{};
    std::vector<uint8_t> out;

    explicit Client(uint16_t seed = SEED, uint16_t port = HTTP_PORT,
                    uint16_t request_length = static_cast<uint16_t>(REQUEST.size()))
        : out(SEND_ROOM + 8, 0xEE) {
        const NetEndpoint us = endpoint(OUR_MAC, OUR_IP, 0);
        const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP, port);
        tcp_start(&c, &us, &server, REQUEST.data(), request_length, seed);
    }

    /* The segment the client wants sent now, having been given `in`.  `flags`
     * of 0 back means it sent nothing. */
    Seg step(const uint8_t* in, uint16_t length) {
        const uint16_t n = tcp_step(&c, in, length, out.data());
        for (size_t i = SEND_ROOM; i < out.size(); i++) {
            CHECK(out[i] == 0xEE);  // nothing written past the buffer it was given
        }
        return sent(out.data(), n);
    }
    Seg step(const std::vector<uint8_t>& in) {
        return step(in.data(), static_cast<uint16_t>(in.size()));
    }
    Seg tick() { return step(nullptr, 0); }

    std::vector<uint8_t> server(const Seg& s, bool corrupt = false) {
        return from_server(s, c.us.port, SERVER_IP, HTTP_PORT, corrupt);
    }
};

/* A connection carried through the handshake, with the server's own sequence
 * numbering under the test's control so the wrap can be arranged. */
struct Open {
    Client client;
    uint32_t theirs;  // the next sequence number the server will send from
    uint32_t ours;    // the next one we expect an acknowledgement of

    explicit Open(uint32_t server_isn = 0x11223344, uint16_t seed = SEED) : client(seed) {
        const Seg syn = client.tick();
        REQUIRE(syn.flags == SYN);
        const uint32_t isn = syn.seq;
        Seg reply;
        reply.flags = SYN | ACK;
        reply.seq = server_isn;
        reply.ack = isn + 1;
        const Seg got = client.step(client.server(reply));
        REQUIRE((got.flags & ACK) != 0);
        REQUIRE(got.payload.size() == REQUEST.size());
        theirs = server_isn + 1;
        ours = isn + 1 + static_cast<uint32_t>(REQUEST.size());

        /* And acknowledge the request, so the connection is quiet. */
        Seg pure;
        pure.flags = ACK;
        pure.seq = theirs;
        pure.ack = ours;
        (void)client.step(client.server(pure));
    }

    /* A data segment from the server, in order unless `seq` says otherwise. */
    std::vector<uint8_t> data(const std::vector<uint8_t>& payload, uint8_t flags = ACK | PSH,
                              const std::vector<uint8_t>& options = {}) {
        Seg s;
        s.flags = flags;
        s.seq = theirs;
        s.ack = ours;
        s.options = options;
        s.payload = payload;
        return client.server(s);
    }
};

std::vector<uint8_t> bytes(size_t n, uint8_t fill = 0xA5) {
    return std::vector<uint8_t>(n, fill);
}

/* The maximum-segment-size option, RFC 793 §3.1: kind 2, length 4. */
const std::vector<uint8_t> MSS_OPTION = {2, 4, static_cast<uint8_t>(TCP_MSS >> 8),
                                         static_cast<uint8_t>(TCP_MSS & 0xFF)};
/* Twelve bytes of RFC 7323 timestamps with its NOP padding, which is what a
 * Linux server puts on every segment -- the case that makes a payload length
 * computed by subtracting a constant read option bytes as file data. */
const std::vector<uint8_t> TIMESTAMPS = {1, 1, 8, 10, 0x11, 0x22, 0x33, 0x44,
                                         0x55, 0x66, 0x77, 0x88};

}  // namespace

TEST_SUITE("tcp") {

/* The client speaks first, and what it says is a SYN and nothing else: no
 * acknowledgement, because there is nothing yet to acknowledge. */
TEST_CASE("a connection opens with a SYN and no acknowledgement") {
    Client c;
    const Seg syn = c.tick();
    CHECK(syn.flags == SYN);
    CHECK(syn.ack == 0);
    CHECK(syn.payload.empty());
    CHECK(syn.window == TCP_WINDOW_BYTES);
    /* The segment size is told rather than left to the server's guess: this
     * receiver has exactly one frame's room, so a server that assumed the path
     * would take more would be told so by a dropped frame instead. */
    CHECK(syn.options == MSS_OPTION);
    CHECK_FALSE(tcp_done(&c.c));
    CHECK_FALSE(tcp_failed(&c.c));
}

/* The SYN occupies one sequence number, always -- not only once something else
 * has been sent.  If it did not, every byte of the reply would be one out. */
TEST_CASE("the SYN consumes exactly one sequence number") {
    Client c;
    const Seg syn = c.tick();
    Seg reply;
    reply.flags = SYN | ACK;
    reply.seq = 5000;
    reply.ack = syn.seq + 1;
    const Seg got = c.step(c.server(reply));
    CHECK((got.flags & ACK) != 0);
    CHECK(got.seq == syn.seq + 1);   // our SYN took the one before it
    CHECK(got.ack == 5001);          // and theirs took one of theirs
}

/* A SYN acknowledging some other number is not this connection's. */
TEST_CASE("a handshake acknowledging the wrong number is refused") {
    Client c;
    const Seg syn = c.tick();
    Seg reply;
    reply.flags = SYN | ACK;
    reply.seq = 5000;
    reply.ack = syn.seq;  // one short: this does not acknowledge our SYN
    const Seg got = c.step(c.server(reply));
    CHECK(got.flags == 0);
    CHECK_FALSE(tcp_done(&c.c));
}

/* The request goes with the third leg of the handshake.  A caller never asked
 * for it to be sent, and never sees that there was a handshake at all. */
TEST_CASE("the request goes as soon as the connection opens") {
    Client c;
    const Seg syn = c.tick();
    Seg reply;
    reply.flags = SYN | ACK;
    reply.seq = 900;
    reply.ack = syn.seq + 1;
    const Seg got = c.step(c.server(reply));
    CHECK((got.flags & ACK) != 0);
    CHECK((got.flags & PSH) != 0);
    CHECK(got.payload ==
          std::vector<uint8_t>(REQUEST.begin(), REQUEST.end()));
    CHECK(got.seq == syn.seq + 1);
    CHECK(got.options.empty());  // options belong on the SYN alone
}

/* Nothing is sent to a port that was never named, and nothing is sent from a
 * machine with no address: a segment from 0.0.0.0 has nowhere to be answered
 * to. */
TEST_CASE("a connection with nowhere to go fails with nothing sent") {
    Client no_port(SEED, 0);
    CHECK(tcp_failed(&no_port.c));
    CHECK(no_port.tick().flags == 0);

    TcpClient c{};
    const NetEndpoint us = endpoint(OUR_MAC, {0, 0, 0, 0}, 0);
    const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP, HTTP_PORT);
    tcp_start(&c, &us, &server, REQUEST.data(), REQUEST.size(), SEED);
    CHECK(tcp_failed(&c));
}

/* --- What arrives, and how much of it is payload ----------------------- */

/* The rule the whole transfer rests on: a segment's payload is what the IPv4
 * header says it carries less what the data offset says the header is.  Twelve
 * bytes of timestamps in front of it must not become twelve bytes of file. */
TEST_CASE("a segment carrying options delivers only its payload") {
    Open o;
    const auto body = bytes(100, 0x5A);
    const Seg reply = o.client.step(o.data(body, ACK | PSH, TIMESTAMPS));

    CHECK(o.client.c.data_length == 100);
    CHECK(o.client.c.data_at == TCP_PAYLOAD_AT + TIMESTAMPS.size());
    CHECK(reply.ack == o.theirs + 100);
    CHECK(o.client.c.dropped == 0);
}

/* The same segment without options, so the two answers can be compared: the
 * payload is the same bytes at a different offset, and never at a fixed one. */
TEST_CASE("the payload offset follows the data offset field") {
    for (size_t options : {size_t{0}, size_t{4}, size_t{8}, size_t{12}}) {
        CAPTURE(options);
        Open o;
        const auto body = bytes(64, 0xC3);
        (void)o.client.step(o.data(body, ACK | PSH, std::vector<uint8_t>(options, 1)));
        CHECK(o.client.c.data_length == 64);
        CHECK(o.client.c.data_at == TCP_PAYLOAD_AT + options);
    }
}

/* A data offset that claims more header than the segment holds would make the
 * payload length underflow into 64KB of somebody else's memory. */
TEST_CASE("a header longer than the segment is refused") {
    Open o;
    auto f = o.data(bytes(4));
    f[TCP_AT + 12] = 0xF0;  // fifteen words of header, in a segment that has six
    const Seg reply = o.client.step(f);
    CHECK(reply.flags == 0);
    CHECK(o.client.c.data_length == 0);
}

/* And one claiming less than a header is not a segment at all. */
TEST_CASE("a header shorter than TCP's own is refused") {
    Open o;
    auto f = o.data(bytes(4));
    f[TCP_AT + 12] = 0x40;  // four words: shorter than the twenty bytes RFC 793 fixes
    CHECK(o.client.step(f).flags == 0);
    CHECK(o.client.c.data_length == 0);
}

/* Inbound checksums are verified.  TCP's is not optional the way UDP's is, and
 * a corrupted segment accepted is a corrupted file written down. */
TEST_CASE("a segment with a bad checksum is thrown away") {
    Open o;
    Seg s;
    s.flags = ACK | PSH;
    s.seq = o.theirs;
    s.ack = o.ours;
    s.payload = bytes(40);
    const Seg reply = o.client.step(o.client.server(s, /*corrupt=*/true));
    CHECK(reply.flags == 0);
    CHECK(o.client.c.data_length == 0);
    CHECK_FALSE(tcp_heard(&o.client.c));
}

/* --- Whose segment is it ----------------------------------------------- */

TEST_CASE("another machine's segment is not this connection's") {
    Open o;
    Seg s;
    s.flags = ACK | PSH;
    s.seq = o.theirs;
    s.ack = o.ours;
    s.payload = bytes(8);
    CHECK(from_server(s, o.client.c.us.port, ELSEWHERE).size() > 0);
    CHECK(o.client.step(from_server(s, o.client.c.us.port, ELSEWHERE)).flags == 0);
    CHECK(o.client.c.data_length == 0);
    /* The right machine, the wrong port. */
    CHECK(o.client.step(from_server(s, o.client.c.us.port, SERVER_IP, 8080)).flags == 0);
    CHECK(o.client.c.data_length == 0);
    /* And our own port is what a reply comes back to. */
    CHECK(o.client.step(from_server(s, static_cast<uint16_t>(o.client.c.us.port + 1))).flags == 0);
    CHECK(o.client.c.data_length == 0);
}

TEST_CASE("a reset ends the connection") {
    Open o;
    Seg s;
    s.flags = RST;
    s.seq = o.theirs;
    s.ack = o.ours;
    CHECK(o.client.step(o.client.server(s)).flags == 0);
    CHECK(tcp_failed(&o.client.c));
    CHECK_FALSE(tcp_done(&o.client.c));
    /* And nothing is said again after it. */
    CHECK(o.client.tick().flags == 0);
}

/* --- Order, and what falls outside the window --------------------------- */

/* Every in-order segment is acknowledged, and acknowledged at once: with two
 * segments of window a delayed acknowledgement delays the window update, and
 * the server stops to wait on us. */
TEST_CASE("data arriving in order is delivered and acknowledged") {
    Open o;
    uint32_t expected = o.theirs;
    for (uint16_t n : {uint16_t{1}, uint16_t{100}, uint16_t{TCP_MSS}, uint16_t{7}}) {
        CAPTURE(n);
        const Seg reply = o.client.step(o.data(bytes(n)));
        CHECK(o.client.c.data_length == n);
        expected += n;
        CHECK(reply.ack == expected);
        CHECK((reply.flags & ACK) != 0);
        CHECK(reply.payload.empty());  // the request has been acknowledged
        o.theirs = expected;
    }
    CHECK(o.client.c.dropped == 0);
}

/* A segment from further along the stream is thrown away and counted, because
 * nothing here reassembles.  The count is what decides whether it should:
 * near zero on a real connection means the hundred lines of one-run
 * reassembly never need writing. */
TEST_CASE("a segment out of the window is dropped and counted") {
    Open o;
    Seg ahead;
    ahead.flags = ACK | PSH;
    ahead.seq = o.theirs + 1000;  // a hole in front of it
    ahead.ack = o.ours;
    ahead.payload = bytes(50);
    const Seg reply = o.client.step(o.client.server(ahead));

    CHECK(o.client.c.data_length == 0);
    CHECK(o.client.c.dropped == 1);
    /* Answered all the same, and with the number still expected: that is what
     * asks the server to send the missing bytes again. */
    CHECK((reply.flags & ACK) != 0);
    CHECK(reply.ack == o.theirs);

    /* And when the gap is filled, the stream carries on from where it was. */
    const Seg then = o.client.step(o.data(bytes(30)));
    CHECK(o.client.c.data_length == 30);
    CHECK(then.ack == o.theirs + 30);
}

/* A segment already taken, sent again because our acknowledgement was lost.
 * Delivering it twice would write the same bytes twice into a file. */
TEST_CASE("a segment already taken is not delivered twice") {
    Open o;
    const auto again = o.data(bytes(64));
    const Seg first = o.client.step(again);
    CHECK(o.client.c.data_length == 64);
    CHECK(first.ack == o.theirs + 64);

    const Seg repeat = o.client.step(again);
    CHECK(o.client.c.data_length == 0);
    CHECK(o.client.c.dropped == 1);
    CHECK(repeat.ack == o.theirs + 64);  // answered again, so the server moves on
}

/* Sequence numbers are 32 bits and they wrap.  Compared as values rather than
 * as differences, everything after the wrap looks like an old duplicate and
 * the transfer stops dead at the boundary. */
TEST_CASE("the stream carries on across the sequence wrap") {
    Open o(0xFFFFFFF0u);
    CHECK(o.theirs == 0xFFFFFFF1u);

    const Seg first = o.client.step(o.data(bytes(8)));
    CHECK(o.client.c.data_length == 8);
    CHECK(first.ack == 0xFFFFFFF9u);
    o.theirs = 0xFFFFFFF9u;

    /* This one straddles the wrap: it starts at 0xFFFFFFF9 and ends at 9. */
    const Seg over = o.client.step(o.data(bytes(16)));
    CHECK(o.client.c.data_length == 16);
    CHECK(over.ack == 9u);
    o.theirs = 9u;

    /* And on past it, with numbers that are now smaller than the ones before. */
    const Seg after = o.client.step(o.data(bytes(4)));
    CHECK(o.client.c.data_length == 4);
    CHECK(after.ack == 13u);
    CHECK(o.client.c.dropped == 0);
}

/* The other half of the wrap: a segment from before it, arriving after, is an
 * old duplicate however much larger its number looks. */
TEST_CASE("an old segment across the wrap is still an old one") {
    Open o(0xFFFFFFF0u);
    (void)o.client.step(o.data(bytes(32)));  // takes 0xFFFFFFF1 .. 0x11
    o.theirs = 0x11u;
    CHECK(o.client.c.data_length == 32);

    Seg old;
    old.flags = ACK | PSH;
    old.seq = 0xFFFFFFF1u;  // numerically vast, and eight bytes ago
    old.ack = o.ours;
    old.payload = bytes(32);
    const Seg reply = o.client.step(o.client.server(old));
    CHECK(o.client.c.data_length == 0);
    CHECK(o.client.c.dropped == 1);
    CHECK(reply.ack == 0x11u);
}

/* Our own numbering wraps too, and the case that reaches it is an ordinary
 * one: an acknowledgement of the SYN, sent again by a server that did not hear
 * our reply, arriving after the wrap.  Compared as values it is vastly larger
 * than anything we have sent, so it reads as an acknowledgement of something
 * that never went out and the connection is abandoned on its first retry. */
TEST_CASE("our own sequence numbers carry across the wrap") {
    Client c(0xFFFF);
    const Seg syn = c.tick();
    CHECK(syn.seq == 0xFFFFFFFFu);  // the top of the space, so the next byte wraps

    Seg reply;
    reply.flags = SYN | ACK;
    reply.seq = 600;
    reply.ack = 0;  // our SYN's number, plus one, wrapped
    const Seg sent = c.step(c.server(reply));
    CHECK(sent.payload.size() == REQUEST.size());
    CHECK(sent.seq == 0u);

    /* The same acknowledgement again, from before the wrap in value and
     * before it in the stream. */
    Seg late;
    late.flags = ACK;
    late.seq = 601;
    late.ack = 0xFFFFFFFFu;
    const Seg answer = c.step(c.server(late));
    CHECK(answer.flags != 0);            // taken, not read as an impossible ack
    CHECK(answer.seq == 0u);             // and it did not wind the stream back
    CHECK(answer.payload.size() == REQUEST.size());
    CHECK_FALSE(tcp_failed(&c.c));

    /* And the real acknowledgement, past the wrap, still advances it. */
    Seg now;
    now.flags = ACK;
    now.seq = 601;
    now.ack = static_cast<uint32_t>(REQUEST.size());
    const Seg quiet = c.step(c.server(now));
    CHECK(quiet.payload.empty());
}

/* --- Closing ------------------------------------------------------------ */

/* A FIN takes one sequence number of its own, after whatever it carries. */
TEST_CASE("a FIN carrying data delivers the data and is acknowledged past it") {
    Open o;
    const Seg reply = o.client.step(o.data(bytes(20), ACK | PSH | FIN));
    CHECK(o.client.c.data_length == 20);
    CHECK(reply.ack == o.theirs + 21);  // twenty bytes, and the FIN
    CHECK((reply.flags & FIN) != 0);    // ours goes back with it
    CHECK_FALSE(tcp_done(&o.client.c));  // not until ours is acknowledged
}

/* The whole close, and the only thing that ends a reply with no stated
 * length. */
TEST_CASE("the connection is done once our own FIN is acknowledged") {
    Open o;
    const Seg reply = o.client.step(o.data(bytes(10), ACK | PSH | FIN));
    CHECK((reply.flags & FIN) != 0);
    const uint32_t our_fin = reply.seq;

    Seg last;
    last.flags = ACK;
    last.seq = o.theirs + 11;
    last.ack = our_fin + 1;  // our FIN took the one after everything we sent
    (void)o.client.step(o.client.server(last));
    CHECK(tcp_done(&o.client.c));
    CHECK_FALSE(tcp_failed(&o.client.c));
    /* And nothing more is said on a connection that is over. */
    CHECK(o.client.tick().flags == 0);
}

/* A FIN sent again because our acknowledgement was lost.  It must be answered
 * -- a server left waiting holds the connection open -- and it must not be
 * counted as a segment dropped, which would make the counter that decides the
 * reassembly question read high for a reason that has nothing to do with it. */
TEST_CASE("a FIN sent again is answered and not counted as a loss") {
    Open o;
    const auto fin = o.data({}, ACK | FIN);
    const Seg first = o.client.step(fin);
    CHECK((first.flags & FIN) != 0);
    CHECK(first.ack == o.theirs + 1);

    const Seg repeat = o.client.step(fin);
    CHECK((repeat.flags & FIN) != 0);
    CHECK(repeat.ack == o.theirs + 1);
    CHECK(repeat.seq == first.seq);  // and the same FIN, not a second one
    CHECK(o.client.c.dropped == 0);
    CHECK(o.client.c.data_length == 0);
}

/* A FIN that arrives before the bytes in front of it cannot be taken: doing so
 * would end the body at a hole. */
TEST_CASE("a FIN beyond a gap is not taken") {
    Open o;
    Seg ahead;
    ahead.flags = ACK | FIN;
    ahead.seq = o.theirs + 500;
    ahead.ack = o.ours;
    const Seg reply = o.client.step(o.client.server(ahead));
    CHECK((reply.flags & FIN) == 0);
    CHECK(reply.ack == o.theirs);
    CHECK_FALSE(tcp_done(&o.client.c));
}

/* --- Saying it again ---------------------------------------------------- */

/* The clock is the only thing that answers when a segment of ours was lost,
 * and it must say the same thing rather than the next thing. */
TEST_CASE("a tick says the current step again") {
    Client c;
    const Seg first = c.tick();
    const Seg again = c.tick();
    CHECK(again.flags == SYN);
    CHECK(again.seq == first.seq);
    CHECK(again.options == MSS_OPTION);
    CHECK(c.c.retransmits >= 1);
}

/* An unacknowledged request is sent again from where it started, not from
 * where the last one ended. */
TEST_CASE("an unacknowledged request is sent again unchanged") {
    Client c;
    const Seg syn = c.tick();
    Seg reply;
    reply.flags = SYN | ACK;
    reply.seq = 4000;
    reply.ack = syn.seq + 1;
    const Seg sent = c.step(c.server(reply));
    const Seg again = c.tick();
    CHECK(again.seq == sent.seq);
    CHECK(again.payload == sent.payload);
    CHECK(again.ack == sent.ack);
}

/* Once it has been acknowledged there is nothing left to repeat, so a tick
 * says only that we are still here and still have room. */
TEST_CASE("an acknowledged request is not sent a second time") {
    Open o;  // its constructor acknowledges the request
    const Seg again = o.client.tick();
    CHECK(again.payload.empty());
    CHECK((again.flags & ACK) != 0);
    CHECK(again.ack == o.theirs);
}

/* Half an acknowledgement: a server that took only some of the request is
 * sent the rest, and not the whole of it again. */
TEST_CASE("a partly acknowledged request resumes where it stopped") {
    Client c;
    const Seg syn = c.tick();
    Seg reply;
    reply.flags = SYN | ACK;
    reply.seq = 7000;
    reply.ack = syn.seq + 1;
    const Seg sent = c.step(c.server(reply));

    Seg part;
    part.flags = ACK;
    part.seq = 7001;
    part.ack = sent.seq + 5;  // five bytes of twelve
    (void)c.step(c.server(part));
    const Seg rest = c.tick();
    CHECK(rest.seq == sent.seq + 5);
    CHECK(rest.payload ==
          std::vector<uint8_t>(REQUEST.begin() + 5, REQUEST.end()));
}

/* An acknowledgement of something never sent is a segment from a connection
 * that is not this one. */
TEST_CASE("an acknowledgement past what was sent is refused") {
    Open o;
    Seg s;
    s.flags = ACK | PSH;
    s.seq = o.theirs;
    s.ack = o.ours + 5000;
    s.payload = bytes(8);
    CHECK(o.client.step(o.client.server(s)).flags == 0);
    CHECK(o.client.c.data_length == 0);
}

/* A request of no bytes at all: the handshake still completes and the
 * connection is still writable, which is what a caller with nothing to say
 * would find. */
TEST_CASE("a connection with an empty request still opens") {
    Client c(SEED, HTTP_PORT, 0);
    const Seg syn = c.tick();
    Seg reply;
    reply.flags = SYN | ACK;
    reply.seq = 300;
    reply.ack = syn.seq + 1;
    const Seg got = c.step(c.server(reply));
    CHECK((got.flags & ACK) != 0);
    CHECK(got.payload.empty());
    CHECK(got.ack == 301);
}

/* Two runs a moment apart must not look like one connection to a server that
 * still has the last one's segments in flight. */
TEST_CASE("the seed moves the port and the sequence number") {
    Client a(0x1234);
    Client b(0x9876);
    CHECK(a.c.us.port != b.c.us.port);
    CHECK(a.tick().seq != b.tick().seq);
    /* And the port is one RFC 6335 leaves to whoever picks it. */
    CHECK(a.c.us.port >= 0xC000);
    CHECK(b.c.us.port >= 0xC000);
}

}  // TEST_SUITE
