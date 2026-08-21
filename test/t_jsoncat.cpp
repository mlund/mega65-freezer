/* The JSON catalogue turned into the fixed-width one, checked by reading the
 * result back with catalog.c -- the code that actually consumes it, and which
 * is tested independently in t_catalog.cpp.
 *
 * That is the oracle worth having: it does not repeat this file's arithmetic
 * about where a field starts, it asks the browser's own reader what it sees.
 * A field written at the wrong offset is not a crash, it is a browser full of
 * plausible wrong entries, and only the real reader catches that.
 *
 * The bulk of the input is verbatim from the live proxy, not written by hand.
 * A hand-written fixture states what the format was believed to be, which is
 * the thing under test. */

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "jsoncat_capture.h"

extern "C" {
#include "catalog.h"
#include "jsoncat.h"
}

/* One writer, one reader, one stride.  The reader takes the stride from the
 * header, so these are only equal because this writer chose to make them
 * so -- and a header of a different size would put every record at the wrong
 * place. */
static_assert(JSONCAT_RECORD_BYTES == CATALOG_HEADER_BYTES);

namespace {

/* A whole reply put through the transcoder in pieces, as it arrives. */
struct Run {
    JsonCatalog c{};
    std::vector<std::vector<uint8_t>> raw;
    std::array<uint8_t, CATALOG_HEADER_BYTES> header{};
    bool clean = false;

    Run() { jsoncat_begin(&c); }

    /* Feeds `json` in pieces of at most `chunk` bytes, the way segments
     * arrive.  The caller's loop is the one in jsoncat.h's contract. */
    void feed(const std::string& json, size_t chunk = 4096) {
        size_t at = 0;
        while (at < json.size()) {
            const size_t piece = std::min(chunk, json.size() - at);
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(json.data()) + at;
            uint16_t left = static_cast<uint16_t>(piece);
            while (left) {
                uint16_t taken = 0;
                std::vector<uint8_t> record(JSONCAT_RECORD_BYTES, 0xEE);
                const bool got = jsoncat_take(&c, bytes, left, &taken, record.data());
                /* Or the caller's loop never ends. */
                REQUIRE(taken > 0);
                REQUIRE(taken <= left);
                if (got) {
                    raw.push_back(record);
                }
                bytes += taken;
                left = static_cast<uint16_t>(left - taken);
            }
            at += piece;
        }
        clean = jsoncat_end(&c, header.data());
    }

    CatalogRecord at(size_t i) const {
        CatalogRecord r{};
        catalog_record(raw.at(i).data(), &r);
        return r;
    }
};

/* What each kept record of the capture must come out as.  Literals, read off
 * the JSON by eye rather than computed the way the code computes them. */
struct Expected {
    const char* title;
    const char* author;
    const char* path;
    uint8_t kind;
    uint32_t size;
    uint8_t category;
    uint16_t year;
};

const std::vector<Expected> KEPT = {
    {"Turrican Engine Demo", "Shallan50k", "/files/t/turricandemo_TTCgrt.D81", CatalogD81, 819200,
     CatalogGame, 2021},
    {"GeekSlide", "deft", "/files/g/geekslide_GEsZLs.d81", CatalogD81, 819200, CatalogDemo, 2021},
    {"MEGAPLOT", "grim_fandango", "/files/m/megaplot_3IrPXs.d81", CatalogD81, 819200,
     CatalogApplication, 2022},
    {"Yaped32 - Native PETSCII editor", "ubik", "/files/y/ydist_pgAyPV.d81", CatalogD81, 819200,
     CatalogTool, 2025},
    {"B10SEX", "ubik", "/files/b/b10sex_PogUCV.prg", CatalogPrg, 1902, CatalogManual, 2021},
    {"Two Easter Eggs in BASIC 10", "Snoopy", "/files/e/Easter_eggs_sB7rhi.d81", CatalogD81,
     819200, CatalogOther, 2021},
    {"BASIC65.D81", "maurice", "/files/b/BASIC65_CyUtHo.D81", CatalogD81, 819200, CatalogFirmware,
     2024},
    {"screenful-compo", "zeha", "/files/s/screenful-compo_MGk0Bg.prg", CatalogPrg, 1051,
     CatalogOther, 2025},
};

/* A record in the shape the proxy sends one, for the cases the capture does
 * not happen to contain.  Keys in upstream's own order, values quoted the way
 * upstream quotes them. */
std::string record(const std::string& title, const std::string& filename,
                   const std::string& location, const std::string& category = "Game",
                   const std::string& size = "819200", const std::string& published = "2021/06/02",
                   const std::string& author = "someone") {
    return "{\"fileid\":\"x\",\"title\":\"" + title + "\",\"category\":\"" + category +
           "\",\"type\":\"Other\",\"os\":\"MEGA65\",\"rating\":\"-\",\"published\":\"" + published +
           "\",\"filename\":\"" + filename + "\",\"size\":\"" + size + "\",\"location\":\"" +
           location + "\",\"author\":\"" + author + "\"}";
}

std::string array(const std::vector<std::string>& records) {
    std::string out = "[";
    for (size_t i = 0; i < records.size(); i++) {
        out += (i ? "," : "") + records[i];
    }
    return out + "]";
}

const std::string PROXY = "http://m65filehost.twistedpair.se";

std::string fill(size_t n, char c = 'x') { return std::string(n, c); }

}  // namespace

TEST_SUITE("jsoncat") {

/* --- The live capture ---------------------------------------------------- */

/* Every field of every record the proxy's own bytes produce, read back through
 * the browser's reader. */
TEST_CASE("the proxy's records come out as the browser's records") {
    Run run;
    run.feed(capture::CATALOGUE);
    CHECK(run.clean);
    REQUIRE(run.raw.size() == KEPT.size());
    CHECK(run.c.count == KEPT.size());

    for (size_t i = 0; i < KEPT.size(); i++) {
        CAPTURE(i);
        const CatalogRecord got = run.at(i);
        CHECK(std::string(got.title) == KEPT[i].title);
        CHECK(std::string(got.author) == KEPT[i].author);
        CHECK(std::string(got.path) == KEPT[i].path);
        CHECK(got.kind == KEPT[i].kind);
        CHECK(got.size == KEPT[i].size);
        CHECK(got.category == KEPT[i].category);
        CHECK(got.year == KEPT[i].year);
    }
}

/* All seven of them, and by the same names catalog.c already draws -- so the
 * two cannot drift into disagreeing about which number means which word. */
TEST_CASE("every category upstream uses maps onto one this browser knows") {
    Run run;
    run.feed(capture::CATALOGUE);
    std::vector<uint8_t> seen;
    for (size_t i = 0; i < run.raw.size(); i++) {
        seen.push_back(run.at(i).category);
    }
    for (uint8_t c = CatalogGame; c <= CatalogFirmware; c++) {
        CAPTURE(catalog_category_name(c));
        CHECK(std::count(seen.begin(), seen.end(), c) >= 1);
    }
    CHECK(std::count(seen.begin(), seen.end(), CatalogNoCategory) == 0);
}

/* Four of the twelve are a .zip, a .pdf and two others this browser cannot
 * open.  Dropping them is not new: 195 of upstream's 454 have no kind, and the
 * gateway drops those too. */
TEST_CASE("records with no kind this browser opens are dropped and counted") {
    Run run;
    run.feed(capture::CATALOGUE);
    CHECK(run.c.skipped == 4);
    CHECK(run.c.count == 8);
    for (size_t i = 0; i < run.raw.size(); i++) {
        const CatalogRecord got = run.at(i);
        CHECK((got.kind == CatalogPrg || got.kind == CatalogD81));
    }
}

/* Two of the capture's filenames end in .D81 rather than .d81. */
TEST_CASE("the extension is read whatever its case") {
    Run run;
    run.feed(capture::CATALOGUE);
    CHECK(run.at(0).kind == CatalogD81);  // turricandemo_TTCgrt.D81
    CHECK(run.at(6).kind == CatalogD81);  // BASIC65_CyUtHo.D81
}

/* The header cannot be written until the count is known, so it comes last and
 * the caller goes back for it.  Checked by the reader that has to accept it. */
TEST_CASE("the header is one the browser's reader accepts") {
    Run run;
    run.feed(capture::CATALOGUE);
    CatalogHeader header{};
    REQUIRE(catalog_header(run.header.data(), &header));
    CHECK(header.record_bytes == JSONCAT_RECORD_BYTES);
    CHECK(header.record_count == KEPT.size());
    /* And the offsets it computes from that are where the records were put. */
    CHECK(catalog_record_offset(header.record_bytes, 0) == CATALOG_HEADER_BYTES);
    CHECK(catalog_record_offset(header.record_bytes, 1) == CATALOG_HEADER_BYTES + 128);
}

/* --- Arriving in pieces --------------------------------------------------- */

/* The reply arrives in segments the server chose the size of, so no piece of
 * state may live only within one call.  A record cut in half across a call is
 * the ordinary case, not an edge. */
TEST_CASE("the capture reads the same however it is cut into pieces") {
    Run whole;
    whole.feed(capture::CATALOGUE);

    for (size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{64}, size_t{127},
                         size_t{128}, size_t{1460}}) {
        CAPTURE(chunk);
        Run run;
        run.feed(capture::CATALOGUE, chunk);
        CHECK(run.clean);
        REQUIRE(run.raw.size() == whole.raw.size());
        CHECK(run.c.skipped == whole.c.skipped);
        for (size_t i = 0; i < run.raw.size(); i++) {
            CHECK(run.raw[i] == whole.raw[i]);
        }
        CHECK(run.header == whole.header);
    }
}

/* And cut at literally every offset, which is the only way to be sure the one
 * that breaks is not the one nobody chose: between a key's quote and its
 * colon, inside an escape, between the digits of a size. */
TEST_CASE("a reply split at every single offset reads the same") {
    const std::string json = array({
        record("First", "one.d81", PROXY + "/files/o/one_AAAAAA.d81", "Demo", "819200",
               "2019/12/31"),
        record("Second", "two.prg", PROXY + "/files/t/two_BBBBBB.prg", "Tool", "1024",
               "2024/01/01"),
    });
    Run whole;
    whole.feed(json);
    REQUIRE(whole.raw.size() == 2);

    for (size_t cut = 1; cut < json.size(); cut++) {
        CAPTURE(cut);
        /* Driven by hand rather than through feed(), which ends the catalogue
         * after every call: the whole point is that the boundary falls at
         * `cut` and the state carries across it. */
        Run manual;
        for (const std::string& piece : {json.substr(0, cut), json.substr(cut)}) {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(piece.data());
            uint16_t left = static_cast<uint16_t>(piece.size());
            while (left) {
                uint16_t taken = 0;
                std::vector<uint8_t> rec(JSONCAT_RECORD_BYTES, 0xEE);
                if (jsoncat_take(&manual.c, bytes, left, &taken, rec.data())) {
                    manual.raw.push_back(rec);
                }
                REQUIRE(taken > 0);
                bytes += taken;
                left = static_cast<uint16_t>(left - taken);
            }
        }
        CHECK(jsoncat_end(&manual.c, manual.header.data()));
        REQUIRE(manual.raw.size() == 2);
        CHECK(manual.raw[0] == whole.raw[0]);
        CHECK(manual.raw[1] == whole.raw[1]);
    }
}

/* --- Fields at their edges ------------------------------------------------ */

/* The longest title in the catalogue is 97 characters, on a record with no
 * kind -- so the capture cannot exercise the cut.  One that can: */
TEST_CASE("a title longer than the field is cut to it") {
    const std::string long_title = fill(97, 'T');
    Run run;
    run.feed(array({record(long_title, "a.d81", PROXY + "/files/a/a_AAAAAA.d81")}));
    REQUIRE(run.raw.size() == 1);
    CHECK(std::string(run.at(0).title) == fill(CATALOG_TITLE_BYTES, 'T'));
    CHECK(std::string(run.at(0).title).size() == CATALOG_TITLE_BYTES);
}

TEST_CASE("an author longer than the field is cut to it") {
    Run run;
    run.feed(array({record("t", "a.d81", PROXY + "/files/a/a_AAAAAA.d81", "Game", "1",
                           "2020/01/01", fill(40, 'A'))}));
    REQUIRE(run.raw.size() == 1);
    CHECK(std::string(run.at(0).author) == fill(CATALOG_AUTHOR_BYTES, 'A'));
}

/* A path is not cut, because a cut path names a different file.  The eight
 * over-long paths upstream has are all a .zip, a .7z or a .pdf, so none of
 * them is a record this keeps anyway -- but the rule has to hold for the day
 * one is. */
TEST_CASE("a path too long for the field loses the record") {
    const std::string long_path = "/files/a/" + fill(44, 'p') + ".d81";  // 57 characters
    REQUIRE(long_path.size() > CATALOG_PATH_BYTES);
    Run run;
    run.feed(array({record("t", "a.d81", PROXY + long_path)}));
    CHECK(run.raw.empty());
    CHECK(run.c.skipped == 1);
    CHECK(run.c.count == 0);
}

/* Exactly at the width is not too long. */
TEST_CASE("a path that exactly fills the field is kept") {
    const std::string path = "/files/a/" + fill(CATALOG_PATH_BYTES - 13, 'p') + ".d81";
    REQUIRE(path.size() == CATALOG_PATH_BYTES);
    Run run;
    run.feed(array({record("t", "a.d81", PROXY + path)}));
    REQUIRE(run.raw.size() == 1);
    CHECK(std::string(run.at(0).path) == path);
}

/* The value buffer must hold the whole URL, not just the path, because the
 * host in front of it is part of what arrives.  A URL that overflows it loses
 * the record rather than producing a path judged on a truncated copy. */
TEST_CASE("a URL too long to be seen whole loses the record") {
    Run run;
    run.feed(array({record("t", "a.d81", "http://" + fill(120, 'h') + "/files/a/a.d81")}));
    CHECK(run.raw.empty());
    CHECK(run.c.skipped == 1);
}

/* A location that is already a path, with no scheme and host in front of it,
 * is taken as it stands -- a proxy that rewrote them that way would otherwise
 * lose its whole catalogue. */
TEST_CASE("a location with no host is already the path") {
    Run run;
    run.feed(array({record("t", "a.d81", "/files/a/a_AAAAAA.d81")}));
    REQUIRE(run.raw.size() == 1);
    CHECK(std::string(run.at(0).path) == "/files/a/a_AAAAAA.d81");
}

TEST_CASE("a category this browser has no name for is left unstated") {
    Run run;
    run.feed(array({record("t", "a.d81", PROXY + "/files/a/a.d81", "Cryptocurrency")}));
    REQUIRE(run.raw.size() == 1);
    CHECK(run.at(0).category == CatalogNoCategory);
    CHECK(std::string(catalog_category_name(run.at(0).category)).empty());
}

/* Four digits off the front of YYYY/MM/DD, which is the only date format the
 * whole catalogue uses. */
TEST_CASE("the year is the first four digits of the publication date") {
    Run run;
    run.feed(array({record("a", "a.d81", PROXY + "/files/a/a.d81", "Game", "1", "1987/07/04"),
                    record("b", "b.d81", PROXY + "/files/b/b.d81", "Game", "1", "")}));
    REQUIRE(run.raw.size() == 2);
    CHECK(run.at(0).year == 1987);
    CHECK(run.at(1).year == 0);  // nothing said
}

/* The catalogue's largest file is 819200, but upstream carries one of 380MB --
 * so the field is written as four bytes and not two. */
TEST_CASE("a size past sixteen bits survives") {
    Run run;
    run.feed(array({record("a", "a.d81", PROXY + "/files/a/a.d81", "Game", "380992583")}));
    REQUIRE(run.raw.size() == 1);
    CHECK(run.at(0).size == 380992583u);
}

/* Upstream quotes its numbers today.  It need not always: a PHP change from
 * "819200" to 819200 is invisible in a browser that reads every size as
 * zero. */
TEST_CASE("a size is read whether or not it is quoted") {
    Run run;
    run.feed("[{\"title\":\"a\",\"filename\":\"a.d81\",\"size\":819200,"
             "\"location\":\"/files/a/a.d81\",\"category\":\"Game\",\"published\":\"2020/01/01\","
             "\"author\":\"me\"}]");
    REQUIRE(run.raw.size() == 1);
    CHECK(run.at(0).size == 819200u);
    CHECK(std::string(run.at(0).author) == "me");
}

/* --- What must be ignored ------------------------------------------------- */

/* The capture carries ten keys this does not read, one of which is HTML with
 * escaped quotes in it.  An escape read as a real quote ends the string early
 * and every key after it in that record is read as a value. */
TEST_CASE("escaped quotes inside a value do not end it") {
    Run run;
    run.feed("[{\"title\":\"say \\\"hello\\\" now\",\"hiddensize\":"
             "\"<span style=\\\"display: none;\\\">000000819200-</span>\","
             "\"filename\":\"a.d81\",\"location\":\"/files/a/a.d81\","
             "\"category\":\"Demo\",\"published\":\"2020/01/01\",\"author\":\"me\"}]");
    REQUIRE(run.raw.size() == 1);
    CHECK(std::string(run.at(0).title) == "say \"hello\" now");
    CHECK(run.at(0).category == CatalogDemo);
    CHECK(std::string(run.at(0).author) == "me");
}

/* A brace or a bracket inside a string is text, not structure.  Counted as
 * structure it ends the record early and every one after it is wrong. */
TEST_CASE("braces inside a value are text") {
    Run run;
    run.feed(array({record("a }{ [] title", "a.d81", PROXY + "/files/a/a.d81")}));
    REQUIRE(run.raw.size() == 1);
    CHECK(std::string(run.at(0).title) == "a }{ [] title");
}

/* Whitespace between tokens is a server's own business: the same reply pretty
 * printed must give the same records. */
TEST_CASE("whitespace between tokens changes nothing") {
    const std::string tight = array({record("a", "a.d81", PROXY + "/files/a/a.d81")});
    std::string loose;
    bool in_string = false;
    for (size_t i = 0; i < tight.size(); i++) {
        if (tight[i] == '"' && (i == 0 || tight[i - 1] != '\\')) {
            in_string = !in_string;
        }
        loose += tight[i];
        if (!in_string && (tight[i] == ',' || tight[i] == ':' || tight[i] == '{')) {
            loose += "\n   ";
        }
    }
    Run a;
    a.feed(tight);
    Run b;
    b.feed(loose);
    REQUIRE(a.raw.size() == 1);
    CHECK(a.raw == b.raw);
}

/* A record missing the things it cannot do without is dropped rather than
 * written half filled. */
TEST_CASE("a record with no filename or no location is dropped") {
    Run no_name;
    no_name.feed("[{\"title\":\"a\",\"location\":\"/files/a/a.d81\",\"category\":\"Game\"}]");
    CHECK(no_name.raw.empty());
    CHECK(no_name.c.skipped == 1);

    Run no_place;
    no_place.feed("[{\"title\":\"a\",\"filename\":\"a.d81\",\"category\":\"Game\"}]");
    CHECK(no_place.raw.empty());
    CHECK(no_place.c.skipped == 1);
}

/* Upstream sends all seventeen keys on all 454 records, so nothing in the
 * capture exercises a record that leaves one out.  It need not stay that way,
 * and a field kept from the record before is the worst kind of wrong: every
 * entry looks filled in, and one of them is another file's. */
TEST_CASE("a record does not inherit the fields the one before it had") {
    Run run;
    run.feed("[" +
             record("First", "one.d81", PROXY + "/files/o/one_AAAAAA.d81", "Firmware", "819200",
                    "2019/12/31", "somebody") +
             ",{\"title\":\"Second\",\"filename\":\"two.prg\","
             "\"location\":\"/files/t/two_BBBBBB.prg\"}]");
    REQUIRE(run.raw.size() == 2);
    const CatalogRecord second = run.at(1);
    CHECK(std::string(second.title) == "Second");
    CHECK(std::string(second.path) == "/files/t/two_BBBBBB.prg");
    CHECK(second.kind == CatalogPrg);
    /* Everything the second record did not state, unstated. */
    CHECK(std::string(second.author).empty());
    CHECK(second.category == CatalogNoCategory);
    CHECK(second.size == 0);
    CHECK(second.year == 0);
}

TEST_CASE("an empty catalogue is an empty catalogue") {
    Run run;
    run.feed("[]");
    CHECK(run.clean);
    CHECK(run.raw.empty());
    CatalogHeader header{};
    REQUIRE(catalog_header(run.header.data(), &header));
    CHECK(header.record_count == 0);
}

/* --- A reply that stopped part way ---------------------------------------- */

/* The catalogue endpoint has no stated length, so its body ends at the close.
 * A connection cut early therefore looks exactly like a complete reply unless
 * the shape of the JSON says otherwise -- and storing that over a good
 * catalogue would lose entries with nothing to show for it. */
TEST_CASE("a reply that stopped part way is not a clean catalogue") {
    const std::string json = array({record("a", "a.d81", PROXY + "/files/a/a.d81"),
                                    record("b", "b.d81", PROXY + "/files/b/b.d81")});
    Run run;
    run.feed(json.substr(0, json.size() - 30));
    CHECK_FALSE(run.clean);
    /* What did arrive is still there, and the header still describes it. */
    CHECK(run.raw.size() == 1);
    CatalogHeader header{};
    REQUIRE(catalog_header(run.header.data(), &header));
    CHECK(header.record_count == 1);
}

TEST_CASE("a reply cut inside a string is not a clean catalogue") {
    Run run;
    run.feed("[{\"title\":\"unfinis");
    CHECK_FALSE(run.clean);
    CHECK(run.raw.empty());
}

TEST_CASE("a whole reply is a clean catalogue") {
    Run run;
    run.feed(array({record("a", "a.d81", PROXY + "/files/a/a.d81")}));
    CHECK(run.clean);
}

}  // TEST_SUITE
