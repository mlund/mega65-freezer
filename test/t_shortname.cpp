/* The 8.3 name a catalogue entry gets on the card.
 *
 * The oracle is what FAT and hyppo will accept, so the checks are written as
 * the properties the result must have -- fits 8.3, holds only characters a
 * short directory entry can, and never comes out empty -- as well as the exact
 * text for the paths the catalogue actually publishes. */

#include <set>
#include <string>

#include "doctest.h"

extern "C" {
#include "catalog.h"
#include "shortname.h"
}

namespace {

std::string shortened(const std::string& path, uint8_t kind = CatalogD81) {
    char out[SHORT_NAME_BYTES] = {};
    catalog_short_name(path.c_str(), kind, out);
    /* A write past the last byte would leave no terminator to find. */
    REQUIRE(out[SHORT_NAME_BYTES - 1] == '\0');
    return out;
}

/* What fat_name_to_entry() can pack and hyppo can find again. */
void check_is_short_name(const std::string& name) {
    CAPTURE(name);
    const size_t dot = name.find('.');
    REQUIRE(dot != std::string::npos);
    CHECK(dot >= 1);
    CHECK(dot <= 8);
    CHECK(name.size() - dot - 1 == 3);
    CHECK(name.find('.', dot + 1) == std::string::npos);
    for (char c : name) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                             c == '-' || c == '.';
        CAPTURE(c);
        CHECK(allowed);
    }
}

/* The stem the name is recognisable by, without the hash that keeps it unique.
 * The hash characters are not written out anywhere below: what matters is that
 * they are there, that they are stable, and that they differ where the paths
 * do. */
std::string stem_of(const std::string& name) {
    return name.substr(0, name.find('.') - 3);
}

}  // namespace

TEST_SUITE("shortname") {

TEST_CASE("the name comes from the last part of the path") {
    CHECK(stem_of(shortened("files/e/EASTER.d81")) == "EASTE");
    CHECK(stem_of(shortened("EASTER.d81")) == "EASTE");
    check_is_short_name(shortened("files/e/EASTER.d81"));
}

/* The reason the name is not simply truncated.  FileHost's stems carry a
 * random suffix to tell near-identical titles apart, and the first characters
 * are exactly what that suffix is not in: truncation would land both of these
 * on one card entry, and attaching either would mount whichever was written
 * last while the screen named the other. */
TEST_CASE("two paths that share a stem get different names") {
    const std::string one = shortened("files/e/Easter_eggs_sB7rhi.d81");
    const std::string two = shortened("files/e/Easter_eggs_v2_q1Zk.d81");
    CHECK(stem_of(one) == stem_of(two));
    CHECK(one != two);
    check_is_short_name(one);
    check_is_short_name(two);
}

/* Same path, same name, every time -- otherwise a file already fetched would
 * not be recognised on the next run. */
TEST_CASE("the name is stable for a given path") {
    CHECK(shortened("files/e/Easter_eggs_sB7rhi.d81")
          == shortened("files/e/Easter_eggs_sB7rhi.d81"));
    /* The directories are part of what makes two entries different, so they
     * are part of what is hashed. */
    CHECK(shortened("files/a/x.d81") != shortened("files/b/x.d81"));
}

/* Two hundred and fifty-seven records is what FileHost publishes today; a
 * derivation that collided across a set that size would be no better than the
 * truncation it replaced. */
TEST_CASE("a catalogue's worth of paths gives a catalogue's worth of names") {
    std::set<std::string> names;
    for (int i = 0; i < 300; i++) {
        names.insert(shortened("files/g/Game_Collection_" + std::to_string(i) + ".d81"));
    }
    CHECK(names.size() == 300);
}

TEST_CASE("the extension comes from the kind, not from the path") {
    CHECK(shortened("files/x/thing.d81", CatalogPrg).substr(8) == ".PRG");
    CHECK(shortened("files/x/thing.prg", CatalogD81).substr(8) == ".D81");
    CHECK(stem_of(shortened("files/x/thing", CatalogD81)) == "THING");
}

/* The first dot would leave "MY", losing everything that told the entries
 * apart. */
TEST_CASE("only the last dot separates the extension") {
    CHECK(stem_of(shortened("files/x/my.old.game.d81")) == "MYOLD");
}

TEST_CASE("characters a short entry cannot hold are dropped") {
    CHECK(stem_of(shortened("files/x/a b(c)!.d81")) == "ABC");
    check_is_short_name(shortened("files/x/a b(c)!.d81"));
}

/* Nothing usable in the path still has to produce something attachable, or the
 * fetch fails at the last step with no name to report. */
TEST_CASE("a path with no usable stem still yields a name") {
    check_is_short_name(shortened("files/x/!!!.d81"));
    check_is_short_name(shortened(""));
    check_is_short_name(shortened("files/x/"));
    CHECK(stem_of(shortened("files/x/!!!.d81")) == "FILE");
}

TEST_CASE("every published path shape gives a usable name") {
    const char* paths[] = {
        "files/e/Easter_eggs_sB7rhi.d81", "files/a/A.prg",  "files/9/9lives.d81",
        "files/z/---.d81",                "deep/er/still/x.d81",
    };
    for (const char* path : paths) {
        check_is_short_name(shortened(path, CatalogD81));
    }
}

}  // TEST_SUITE
