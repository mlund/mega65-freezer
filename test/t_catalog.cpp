/* The FileHost catalogue decoder, against ether65's docs/FILEHOST.md section 2.
 *
 * The oracle is the specification rather than another implementation of it:
 * offsets and widths are written out here as literals, so a decoder that
 * drifted would have to drift past a number quoted from the document.  The
 * cases that earn their place are the ones a client gets wrong silently -- a
 * record size other than 128, and padding read as content. */

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

extern "C" {
#include "catalog.h"
}

namespace {

/* A catalogue built the way the document says to build one, so the test states
 * the format independently of the code reading it. */
struct Entry {
    std::string title;
    std::string author;
    std::string path;
    uint8_t kind;
    uint32_t size;
};

void put_le16(std::vector<uint8_t>& out, size_t at, uint16_t v) {
    out[at] = static_cast<uint8_t>(v);
    out[at + 1] = static_cast<uint8_t>(v >> 8);
}

void put_le32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        out[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    }
}

void put_text(std::vector<uint8_t>& out, size_t at, const std::string& text, size_t width,
              uint8_t pad) {
    for (size_t i = 0; i < width; i++) {
        out[at + i] = i < text.size() ? static_cast<uint8_t>(text[i]) : pad;
    }
}

std::vector<uint8_t> catalogue(const std::vector<Entry>& entries, uint8_t version = 1,
                               uint16_t record_bytes = 128,
                               const char* magic = "M65FHCAT") {
    /* Room for the fields even when the stride under test is too small to hold
     * them, so a deliberately malformed header still produces a buffer this
     * builder can fill. */
    const size_t stride = std::max<size_t>(record_bytes, 128);
    std::vector<uint8_t> out(128 + stride * entries.size(), 0);
    std::memcpy(out.data(), magic, 8);
    out[8] = version;
    put_le16(out, 9, record_bytes);
    put_le16(out, 11, static_cast<uint16_t>(entries.size()));
    put_le32(out, 13, 1700000000);

    for (size_t i = 0; i < entries.size(); i++) {
        const size_t at = 128 + i * stride;
        put_text(out, at + 0, entries[i].title, 40, ' ');
        put_text(out, at + 40, entries[i].author, 16, ' ');
        put_text(out, at + 56, entries[i].path, 48, 0);
        out[at + 104] = entries[i].kind;
        put_le32(out, at + 105, entries[i].size);
    }
    return out;
}

CatalogRecord decoded(const std::vector<uint8_t>& raw, uint16_t index, uint16_t record_bytes = 128) {
    CatalogRecord out{};
    catalog_record(raw.data() + catalog_record_offset(record_bytes, index), &out);
    return out;
}

const Entry ROBOTS = {"ATTACK OF THE ROBOTS", "SOMEBODY", "files/a/robots_x1.d81", CatalogD81,
                      819200};
const Entry HELLO = {"HELLO WORLD", "NOBODY", "files/h/hello.prg", CatalogPrg, 42};

}  // namespace

TEST_SUITE("catalog") {

TEST_CASE("a catalogue says what it is before it says anything else") {
    const auto raw = catalogue({ROBOTS});
    CatalogHeader header{};
    REQUIRE(catalog_header(raw.data(), &header));
    CHECK(header.record_bytes == 128);
    CHECK(header.record_count == 1);
}

TEST_CASE("anything that is not a catalogue is refused") {
    CatalogHeader header{};
    CHECK_FALSE(catalog_header(catalogue({ROBOTS}, 1, 128, "M65FHCAU").data(), &header));
    CHECK_FALSE(catalog_header(catalogue({ROBOTS}, 2).data(), &header));

    /* A record smaller than the fields it must hold could not be indexed into,
     * and zero would put every record at the same place. */
    CHECK_FALSE(catalog_header(catalogue({ROBOTS}, 1, 0).data(), &header));
    CHECK_FALSE(catalog_header(catalogue({ROBOTS}, 1, 64).data(), &header));
}

/* The rule the document states and a client gets wrong by hard-coding 128.
 * A later version may make records wider; the fields it already defines stay
 * where they are. */
TEST_CASE("a wider record still indexes and still decodes") {
    const uint16_t wide = 256;
    const auto raw = catalogue({ROBOTS, HELLO}, 1, wide);
    CatalogHeader header{};
    REQUIRE(catalog_header(raw.data(), &header));
    CHECK(header.record_bytes == wide);
    CHECK(catalog_record_offset(wide, 0) == 128);
    CHECK(catalog_record_offset(wide, 1) == 128 + wide);
    CHECK(std::string(decoded(raw, 1, wide).title) == HELLO.title);
}

/* A fixed 128-byte header, then the header's own stride.  At 128 bytes a
 * record that is the (i + 1) << 7 the document writes. */
TEST_CASE("records follow a header of a fixed 128 bytes") {
    CHECK(catalog_record_offset(128, 0) == 128);
    CHECK(catalog_record_offset(128, 1) == 256);
    /* Past what 16 bits hold, which is why the offset is not a uint16_t. */
    CHECK(catalog_record_offset(128, 511) == 65536);
    CHECK(catalog_record_offset(128, 65535) == 8388608);
}

TEST_CASE("a record holds what the machine shows and what it asks for") {
    const auto raw = catalogue({ROBOTS, HELLO});
    const CatalogRecord first = decoded(raw, 0);
    CHECK(std::string(first.title) == ROBOTS.title);
    CHECK(std::string(first.author) == ROBOTS.author);
    CHECK(std::string(first.path) == ROBOTS.path);
    CHECK(first.kind == CatalogD81);
    CHECK(first.size == 819200);

    const CatalogRecord second = decoded(raw, 1);
    CHECK(std::string(second.path) == HELLO.path);
    CHECK(second.kind == CatalogPrg);
    CHECK(second.size == 42);
}

/* Title and author are space-padded and the path NUL-padded, so the two need
 * different trimming and a decoder that used one rule shows the difference
 * only on a full-width field. */
TEST_CASE("padding is not content") {
    const std::string title(40, 'X');
    const std::string author(16, 'Y');
    const std::string path(48, 'z');
    const auto raw = catalogue({{title, author, path, CatalogD81, 1}});
    const CatalogRecord full = decoded(raw, 0);
    CHECK(std::string(full.title) == title);
    CHECK(std::string(full.author) == author);
    CHECK(std::string(full.path) == path);

    const auto sparse = catalogue({{"A", "B", "c", CatalogD81, 1}});
    const CatalogRecord one = decoded(sparse, 0);
    CHECK(std::string(one.title) == "A");
    CHECK(std::string(one.author) == "B");
    CHECK(std::string(one.path) == "c");
}

/* A title that is only spaces has nothing to trim to, and trimming that walks
 * off the front is the way to find out. */
TEST_CASE("an empty field decodes to an empty string") {
    const auto raw = catalogue({{"", "", "", CatalogPrg, 0}});
    const CatalogRecord empty = decoded(raw, 0);
    CHECK(std::string(empty.title).empty());
    CHECK(std::string(empty.author).empty());
    CHECK(std::string(empty.path).empty());
    CHECK(empty.size == 0);
}

/* Interior spaces are content; only the run at the end is padding. */
TEST_CASE("spaces inside a title survive") {
    const auto raw = catalogue({{"THE  LAST  NINJA", "A B", "files/t/x.d81", CatalogD81, 1}});
    CHECK(std::string(decoded(raw, 0).title) == "THE  LAST  NINJA");
    CHECK(std::string(decoded(raw, 0).author) == "A B");
}

TEST_CASE("size is little endian and uses all four bytes") {
    const auto raw = catalogue({{"T", "A", "p", CatalogD81, 0x04030201}});
    CHECK(decoded(raw, 0).size == 0x04030201);
}

}  // TEST_SUITE
