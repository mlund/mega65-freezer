/* HTTP, against RFC 1945 and against what the proxy was measured to do.
 *
 * Driven the way a caller drives it, and through whole frames, so what is
 * checked is the same path the machine takes: a reply is cut into segments
 * here exactly as a server cuts it, and the cases that matter are the ones
 * where the cut falls somewhere awkward.  A header split across two segments
 * is not a rare case -- it is what happens whenever a reply is larger than one
 * frame, which is every reply worth fetching.
 *
 * The body is never copied by the code under test, so every check on it reads
 * the bytes back out of the frame that was handed in, at the offset the client
 * reported.  An offset that is wrong by the length of a header line reads as a
 * file that is subtly corrupt and nothing reports it. */

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "tcpwire.h"

extern "C" {
#include "catalog.h"
#include "http.h"
}

/* The two numbers are the same number: a client asks for the paths the
 * catalogue named, so a path the catalogue can hold and this cannot ask for
 * would be refused with nothing sent -- which on screen reads exactly like a
 * server that never answered.  Pinned here rather than in http.h, which has no
 * business knowing the catalogue's format. */
static_assert(HTTP_PATH_MAX == CATALOG_PATH_BYTES);

namespace {

using namespace wire;

constexpr uint16_t SEED = 0x1234;
const char* const HOST = "192.168.68.57";
const char* const PATH = "/php/readfilespublic.php";

std::vector<uint8_t> text(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

/* A fetch driven through whole frames, with the server's side under the
 * test's control down to where each segment is cut. */
struct Fetch {
    HttpClient c{};
    std::vector<uint8_t> out;
    std::vector<uint8_t> in;  // the frame the last step was given, for reading the body back
    uint32_t theirs = 0;      // the next sequence number the server will send from
    uint32_t ours = 0;        // the next one we expect an acknowledgement of
    std::vector<uint8_t> body;  // every body byte handed over, in order

    explicit Fetch(const char* path = PATH, const char* host = HOST, uint16_t port = HTTP_PORT,
                   uint16_t seed = SEED)
        : out(HTTP_SEND_BYTES + 8, 0xEE) {
        const NetEndpoint us = endpoint(OUR_MAC, OUR_IP, 0);
        const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP, port);
        http_start(&c, &us, &server, path, host, seed);
    }

    Seg step(const std::vector<uint8_t>& frame) {
        in = frame;
        const uint16_t n = http_step(&c, in.data(), static_cast<uint16_t>(in.size()), out.data());
        for (size_t i = HTTP_SEND_BYTES; i < out.size(); i++) {
            CHECK(out[i] == 0xEE);  // nothing written past the buffer it was given
        }
        if (c.data_length) {
            /* Read back where it was said to be, not from the test's own copy
             * of the reply: the offset is the whole answer. */
            CHECK(c.data_at + c.data_length <= in.size());
            body.insert(body.end(), in.begin() + c.data_at,
                        in.begin() + c.data_at + c.data_length);
        }
        return sent(out.data(), n);
    }
    Seg tick() {
        const uint16_t n = http_step(&c, nullptr, 0, out.data());
        return sent(out.data(), n);
    }

    /* Carries the connection through the handshake and returns the request the
     * client sent, as text. */
    std::string open(uint32_t server_isn = 0x22334455) {
        const Seg syn = tick();
        REQUIRE(syn.flags == SYN);
        Seg reply;
        reply.flags = SYN | ACK;
        reply.seq = server_isn;
        reply.ack = syn.seq + 1;
        const Seg got = step(from_server(reply, c.tcp.us.port));
        theirs = server_isn + 1;
        ours = syn.seq + 1 + static_cast<uint32_t>(got.payload.size());
        return std::string(got.payload.begin(), got.payload.end());
    }

    /* One segment of the reply, taken in order, optionally with a FIN on it. */
    Seg reply(const std::vector<uint8_t>& payload, bool fin = false) {
        Seg s;
        s.flags = ACK | (payload.empty() ? 0 : PSH) | (fin ? FIN : 0);
        s.seq = theirs;
        s.ack = ours;
        s.payload = payload;
        theirs += payload.size() + (fin ? 1 : 0);
        return step(from_server(s, c.tcp.us.port));
    }
    Seg reply(const std::string& payload, bool fin = false) { return reply(text(payload), fin); }

    /* The server acknowledging our FIN, which is what ends a connection. */
    void close(const Seg& our_fin) {
        Seg last;
        last.flags = ACK;
        last.seq = theirs;
        last.ack = our_fin.seq + our_fin.payload.size() + 1;
        (void)step(from_server(last, c.tcp.us.port));
    }

    std::string body_text() const { return std::string(body.begin(), body.end()); }
};

std::string fill(size_t n, char c = 'x') { return std::string(n, c); }

}  // namespace

TEST_SUITE("http") {

/* --- The request ------------------------------------------------------- */

/* Every byte of it, because a request is not something a server negotiates:
 * it is either the shape HTTP fixes or it is met with 400. */
TEST_CASE("the request is the one HTTP/1.0 fixes") {
    Fetch f;
    CHECK(f.open() ==
          "GET /php/readfilespublic.php HTTP/1.0\r\n"
          "Host: 192.168.68.57\r\n"
          "Connection: close\r\n"
          "\r\n");
}

/* The close is asked for although HTTP/1.0 already implies it.  The catalogue
 * endpoint states no length, so its body ends at the close and nowhere else --
 * a proxy that kept the connection open would leave that fetch waiting. */
TEST_CASE("the request asks for the connection to be closed") {
    Fetch f;
    const std::string request = f.open();
    CHECK(request.find("Connection: close\r\n") != std::string::npos);
    CHECK(request.find("HTTP/1.1") == std::string::npos);
}

/* A path too long is refused rather than truncated: a truncated path names a
 * different file, and fetching the wrong file quietly is worse than fetching
 * none. */
TEST_CASE("a path or host that does not fit is refused with nothing sent") {
    const std::string long_path = "/" + fill(HTTP_PATH_MAX);
    Fetch too_long(long_path.c_str());
    CHECK(http_failed(&too_long.c));
    CHECK(too_long.tick().flags == 0);

    const std::string long_host = fill(HTTP_HOST_MAX + 1);
    Fetch bad_host(PATH, long_host.c_str());
    CHECK(http_failed(&bad_host.c));

    /* And exactly at the limit is not too long. */
    const std::string longest = "/" + fill(HTTP_PATH_MAX - 1);
    Fetch fits(longest.c_str());
    CHECK_FALSE(http_failed(&fits.c));
    CHECK(fits.open().substr(0, 4 + longest.size()) == "GET " + longest);
}

TEST_CASE("a fetch with nowhere to go is refused") {
    Fetch no_port(PATH, HOST, 0);
    CHECK(http_failed(&no_port.c));
    CHECK(no_port.tick().flags == 0);
}

/* Nothing of `path` and `host` is kept: TCP re-reads the request on every
 * retransmission, so what it re-reads must be the client's own copy. */
TEST_CASE("the request survives the caller reusing what it was built from") {
    char path[HTTP_PATH_MAX + 1];
    char host[HTTP_HOST_MAX + 1];
    std::strcpy(path, "/files/a/thing.d81");
    std::strcpy(host, "10.0.0.9");
    Fetch f;
    const NetEndpoint us = endpoint(OUR_MAC, OUR_IP, 0);
    const NetEndpoint server = endpoint(SERVER_MAC, SERVER_IP, HTTP_PORT);
    http_start(&f.c, &us, &server, path, host, SEED);
    std::memset(path, '!', sizeof path);
    std::memset(host, '!', sizeof host);

    const std::string request = f.open();
    CHECK(request.substr(0, 26) == "GET /files/a/thing.d81 HTT");
    CHECK(request.find("Host: 10.0.0.9\r\n") != std::string::npos);
    /* And again on a retransmission, from the same copy. */
    const Seg again = f.tick();
    CHECK(std::string(again.payload.begin(), again.payload.end()) == request);
}

/* --- The status line ---------------------------------------------------- */

TEST_CASE("a 200 is read and the body begins after the blank line") {
    Fetch f;
    (void)f.open();
    f.reply(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "HELLO");
    CHECK(f.c.status == 200);
    CHECK(f.c.has_length);
    CHECK(f.c.length == 5);
    CHECK(f.body_text() == "HELLO");
    CHECK(http_done(&f.c));
}

/* A refusal is a failure with its own number on it: 404 and 503 are different
 * things for a caller to say. */
TEST_CASE("a 404 fails and keeps its status") {
    Fetch f;
    (void)f.open();
    f.reply(
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "not here!");
    CHECK(http_failed(&f.c));
    CHECK_FALSE(http_done(&f.c));
    CHECK(f.c.status == 404);
    CHECK(f.body.empty());  // a body that is an error page is not the file
}

TEST_CASE("every status this cannot use is a failure, and says which") {
    for (int code : {301, 302, 400, 403, 500, 503}) {
        CAPTURE(code);
        Fetch f;
        (void)f.open();
        f.reply("HTTP/1.1 " + std::to_string(code) + " Nope\r\n\r\n");
        CHECK(http_failed(&f.c));
        CHECK(f.c.status == code);
    }
}

/* Partial content is what a resumed fetch is answered with, and the reply is
 * read the same way. */
TEST_CASE("a 206 is accepted like a 200") {
    Fetch f;
    (void)f.open();
    f.reply(
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-3/100\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "abcd");
    CHECK(f.c.status == 206);
    CHECK(f.body_text() == "abcd");
    CHECK(http_done(&f.c));
}

/* Something that is not an HTTP reply at all must fail on the first line
 * rather than be read as a header block with an odd first entry. */
TEST_CASE("a reply that is not HTTP fails at once") {
    for (const char* first : {"BANANA\r\n\r\n", "HTTP/1.1\r\n\r\n", "HTTP/1.1 XX OK\r\n\r\n",
                              "HTTP/1.1 20 OK\r\n\r\n", "\r\n"}) {
        CAPTURE(first);
        Fetch f;
        (void)f.open();
        f.reply(std::string(first) + "body");
        CHECK(http_failed(&f.c));
        CHECK(f.body.empty());
    }
}

/* --- Headers across segments -------------------------------------------- */

/* The case that is not rare: a reply larger than one frame has its header
 * block cut wherever the server's buffer ran out.  Cut at every offset,
 * because the one that breaks is the one nobody thought of -- inside the
 * digits of the length, between the CR and the LF, on the blank line. */
TEST_CASE("a header block split at any offset reads the same") {
    const std::string head =
        "HTTP/1.1 200 OK\r\n"
        "Server: nginx\r\n"
        "Content-Length: 12\r\n"
        "Accept-Ranges: bytes\r\n"
        "\r\n";
    const std::string content = "0123456789ab";
    const std::string whole = head + content;

    for (size_t cut = 1; cut < whole.size(); cut++) {
        CAPTURE(cut);
        Fetch f;
        (void)f.open();
        f.reply(whole.substr(0, cut));
        f.reply(whole.substr(cut));
        CHECK(f.c.status == 200);
        CHECK(f.c.has_length);
        CHECK(f.c.length == 12);
        CHECK(f.body_text() == content);
        CHECK(http_done(&f.c));
    }
}

/* One byte at a time is the same reply, and the case where every piece of
 * state has to survive between steps. */
TEST_CASE("a reply arriving one byte per segment reads the same") {
    const std::string whole =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "xyz";
    Fetch f;
    (void)f.open();
    for (char ch : whole) {
        f.reply(std::string(1, ch));
    }
    CHECK(f.c.status == 200);
    CHECK(f.body_text() == "xyz");
    CHECK(http_done(&f.c));
}

/* Header names are case-insensitive, RFC 1945 section 4.2, and a server is
 * free to spell one however it likes. */
TEST_CASE("Content-Length is read whatever its case and spacing") {
    for (const char* header : {"Content-Length: 4", "content-length: 4", "CONTENT-LENGTH:  4",
                               "Content-Length:4"}) {
        CAPTURE(header);
        Fetch f;
        (void)f.open();
        f.reply("HTTP/1.1 200 OK\r\n" + std::string(header) + "\r\n\r\nWXYZ");
        CHECK(f.c.has_length);
        CHECK(f.c.length == 4);
        CHECK(f.body_text() == "WXYZ");
    }
}

/* A header line longer than the buffer keeps its beginning and is otherwise
 * ignored, and must not swallow the lines after it. */
TEST_CASE("an over-long header line does not lose the ones after it") {
    Fetch f;
    (void)f.open();
    f.reply(
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: " + fill(400, 'z') + "\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "hi");
    CHECK(f.c.status == 200);
    CHECK(f.c.length == 2);
    CHECK(f.body_text() == "hi");
    CHECK(http_done(&f.c));
}

/* --- Where the body ends ------------------------------------------------ */

/* A file states its length, and the fetch is over on the last byte of it --
 * without waiting for a close that a caller would otherwise have to time out
 * against. */
TEST_CASE("a stated length ends the body on its last byte") {
    Fetch f;
    (void)f.open();
    f.reply(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "0123456789");
    CHECK(http_done(&f.c));
    CHECK(f.c.received == 10);
    CHECK(f.body_text() == "0123456789");
}

/* And a server that sends more than it promised does not get the extra into
 * the file: the length is what was asked for and what there is room for. */
TEST_CASE("a body longer than its stated length is cut to it") {
    Fetch f;
    (void)f.open();
    f.reply(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "abcdEXTRA");
    CHECK(http_done(&f.c));
    CHECK(f.c.received == 4);
    CHECK(f.body_text() == "abcd");
}

/* The catalogue endpoint states no length at all -- measured against the live
 * proxy -- so its body ends at the close and nowhere else.  This is the case
 * TFTP never had and the one a caller cannot work out for itself. */
TEST_CASE("a body with no stated length ends at the close") {
    Fetch f;
    (void)f.open();
    f.reply(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "\r\n"
        "[{\"a\":1},");
    CHECK_FALSE(f.c.has_length);
    CHECK_FALSE(http_done(&f.c));  // nothing yet says it is over
    const Seg our_fin = f.reply(text("{\"b\":2}]"), /*fin=*/true);
    CHECK((our_fin.flags & FIN) != 0);
    f.close(our_fin);
    CHECK(http_done(&f.c));
    CHECK(f.body_text() == "[{\"a\":1},{\"b\":2}]");
}

/* A connection that closes part way through a body whose length was already
 * stated is a truncated file, and must not be reported as a whole one. */
TEST_CASE("a stated length cut short by the close is a failure") {
    Fetch f;
    (void)f.open();
    const Seg our_fin = f.reply(text(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "only this much"), /*fin=*/true);
    f.close(our_fin);
    CHECK(http_failed(&f.c));
    CHECK_FALSE(http_done(&f.c));
    CHECK(f.c.received == 14);
}

/* And one that closes before the headers are even finished. */
TEST_CASE("a close during the headers is a failure") {
    Fetch f;
    (void)f.open();
    const Seg our_fin = f.reply(text("HTTP/1.1 200 OK\r\nContent-Len"), /*fin=*/true);
    f.close(our_fin);
    CHECK(http_failed(&f.c));
}

/* A reset part way through is the connection going away under the fetch. */
TEST_CASE("a reset fails the fetch") {
    Fetch f;
    (void)f.open();
    f.reply("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nsome");
    Seg rst;
    rst.flags = RST;
    rst.seq = f.theirs;
    rst.ack = f.ours;
    (void)f.step(from_server(rst, f.c.tcp.us.port));
    CHECK(http_failed(&f.c));
}

/* --- The body, and where it is ------------------------------------------ */

/* The body is never copied, so what a caller is handed is an offset into the
 * frame it gave in.  On the segment that carries the end of the headers that
 * offset is past them, and on every segment after it, it is the payload's own
 * start. */
TEST_CASE("the body offset is where the body is, not where a header ends") {
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 2000\r\n\r\n";
    Fetch f;
    (void)f.open();
    f.reply(head + fill(60, 'A'));
    /* Past the whole header block, in the frame that carried both. */
    CHECK(f.c.data_at == TCP_PAYLOAD_AT + head.size());
    CHECK(f.c.data_length == 60);

    f.reply(fill(100, 'B'));
    CHECK(f.c.data_at == TCP_PAYLOAD_AT);  // a segment that is body all through
    CHECK(f.c.data_length == 100);
    CHECK(f.body_text() == fill(60, 'A') + fill(100, 'B'));
}

/* A large body over many full segments, which is what a disk image is. */
TEST_CASE("a body over many segments arrives whole and in order") {
    constexpr size_t SEGMENTS = 9;
    std::string content;
    for (size_t i = 0; i < SEGMENTS; i++) {
        content += std::string(TCP_MSS, static_cast<char>('a' + i));
    }
    Fetch f;
    (void)f.open();
    f.reply("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(content.size()) + "\r\n\r\n");
    CHECK(f.c.length == content.size());
    for (size_t i = 0; i < SEGMENTS; i++) {
        f.reply(content.substr(i * TCP_MSS, TCP_MSS));
    }
    CHECK(f.body_text() == content);
    CHECK(http_done(&f.c));
    CHECK(f.c.tcp.dropped == 0);
}

/* A segment sent again because our acknowledgement was lost must not put its
 * bytes into the file twice. */
TEST_CASE("a segment sent again does not reach the body twice") {
    Fetch f;
    (void)f.open();
    f.reply("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n");

    Seg s;
    s.flags = ACK | PSH;
    s.seq = f.theirs;
    s.ack = f.ours;
    s.payload = text("12345678");
    const auto again = from_server(s, f.c.tcp.us.port);
    (void)f.step(again);
    CHECK(f.body_text() == "12345678");
    (void)f.step(again);
    CHECK(f.body_text() == "12345678");
    CHECK(f.c.data_length == 0);
}

/* An empty body, which a zero-length file is. */
TEST_CASE("a body of no bytes at all is still a whole one") {
    Fetch f;
    (void)f.open();
    f.reply("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    CHECK(http_done(&f.c));
    CHECK(f.c.received == 0);
    CHECK(f.body.empty());
}

/* --- Saying it again ----------------------------------------------------- */

/* Silence is timed against this rather than against bytes, so a segment the
 * client is right to throw away still says the server is there. */
TEST_CASE("a frame that is not this fetch's is not heard") {
    Fetch f;
    (void)f.open();
    Seg s;
    s.flags = ACK | PSH;
    s.seq = f.theirs;
    s.ack = f.ours;
    s.payload = text("HTTP/1.1 200 OK\r\n\r\n");
    (void)f.step(from_server(s, f.c.tcp.us.port, ELSEWHERE));
    CHECK_FALSE(http_heard(&f.c));
    CHECK(f.c.status == 0);

    (void)f.step(from_server(s, f.c.tcp.us.port));
    CHECK(http_heard(&f.c));
}

/* Nothing more is said once the fetch is over. */
TEST_CASE("a finished fetch says nothing further") {
    Fetch f;
    (void)f.open();
    f.reply("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nZ");
    CHECK(http_done(&f.c));
    CHECK(f.tick().flags == 0);
}

}  // TEST_SUITE
