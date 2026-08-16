/* Which records the browser shows, and in what order.
 *
 * The module owns an index of record numbers and nothing else, so the two
 * questions it cannot answer -- what a record contains, and what the user
 * typed -- arrive through the same link-time seams the fetch uses.  This file
 * supplies them from a table, which is why none of this needs a machine.
 *
 * Order is always ascending here.  Which way round that reads on screen is the
 * caller's business: it builds the key, so "newest first" and "unknown last"
 * are decisions it encodes rather than ones this has to know. */

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"

extern "C" {
#include "view.h"
}

namespace {

struct Fixture {
    uint8_t category;
    uint16_t year;
    bool matches;
};

std::vector<Fixture> FIXTURE;

/* The order rows come out in, as record numbers, which is the whole of what
 * this module decides. */
std::vector<uint16_t> rows() {
    std::vector<uint16_t> out;
    for (uint16_t i = 0; i < view_count(); i++) {
        out.push_back(view_row(i));
    }
    return out;
}

void given(std::vector<Fixture> records) {
    FIXTURE = std::move(records);
    view_reset(static_cast<uint16_t>(FIXTURE.size()));
}

}  // namespace

extern "C" bool view_matches(uint16_t record) {
    return FIXTURE[record].matches;
}

extern "C" uint8_t view_key(uint16_t record, enum ViewOrder order) {
    const Fixture& f = FIXTURE[record];
    if (order == ViewByYear) {
        /* Newest first, and a record that states no year last.  A byte, which
         * is why a year is ranked from 1900 rather than stored whole. */
        return f.year > 1900 ? static_cast<uint8_t>(255 - (f.year - 1900)) : VIEW_KEY_UNKNOWN;
    }
    return f.category ? f.category : VIEW_KEY_UNKNOWN;
}

TEST_SUITE("view") {

/* The file is already sorted by title -- the format says so -- so title order
 * is the order the records are in, and costs no sort at all. */
TEST_CASE("title order is the order the file is in") {
    given({{1, 2020, true}, {2, 2024, true}, {3, 2022, true}});
    view_build(ViewByTitle);
    CHECK(view_count() == 3);
    CHECK(rows() == std::vector<uint16_t>{0, 1, 2});
    CHECK(view_order() == ViewByTitle);
}

TEST_CASE("by year, newest first") {
    given({{1, 2020, true}, {2, 2024, true}, {3, 2022, true}});
    view_build(ViewByYear);
    CHECK(rows() == std::vector<uint16_t>{1, 2, 0});
    CHECK(view_order() == ViewByYear);
}

/* Equal keys keep the order they arrived in, which is the file's title order --
 * so "by category" reads as titles grouped under a category rather than as an
 * arbitrary shuffle within each one. */
TEST_CASE("the sort is stable, and title order is the tie-break") {
    given({{2, 2020, true}, {1, 2021, true}, {2, 2022, true}, {1, 2023, true}, {2, 2024, true}});
    view_build(ViewByCategory);
    CHECK(rows() == std::vector<uint16_t>{1, 3, 0, 2, 4});
}

TEST_CASE("a record stating no category or year sorts last, not first") {
    SUBCASE("no category") {
        given({{0, 2020, true}, {2, 2021, true}, {0, 2022, true}, {1, 2023, true}});
        view_build(ViewByCategory);
        CHECK(rows() == std::vector<uint16_t>{3, 1, 0, 2});
    }
    SUBCASE("no year") {
        given({{1, 0, true}, {1, 2021, true}, {1, 0, true}, {1, 2023, true}});
        view_build(ViewByYear);
        CHECK(rows() == std::vector<uint16_t>{3, 1, 0, 2});
    }
}

TEST_CASE("a search narrows the same index") {
    given({{1, 2020, true}, {2, 2024, false}, {3, 2022, true}, {1, 2021, false}});
    view_build(ViewByTitle);
    CHECK(view_count() == 2);
    CHECK(rows() == std::vector<uint16_t>{0, 2});

    SUBCASE("and the order still applies to what is left") {
        view_build(ViewByYear);
        CHECK(rows() == std::vector<uint16_t>{2, 0});
    }
}

TEST_CASE("a search that matches nothing leaves no rows") {
    given({{1, 2020, false}, {2, 2024, false}});
    view_build(ViewByTitle);
    CHECK(view_count() == 0);
    /* Sorting nothing is not a special case anywhere. */
    view_build(ViewByYear);
    CHECK(view_count() == 0);
}

TEST_CASE("a search that matches every row is the whole list") {
    given({{1, 2020, true}, {2, 2024, true}, {3, 2022, true}});
    view_build(ViewByTitle);
    CHECK(view_count() == 3);
}

TEST_CASE("an empty catalogue") {
    given({});
    view_build(ViewByTitle);
    CHECK(view_count() == 0);
    view_build(ViewByCategory);
    CHECK(view_count() == 0);
}

/* The browser indexes rows and the catalogue may hold more records than the
 * index can, so what it shows is bounded by what it can address. */
TEST_CASE("more records than the index holds are cut, not wrapped") {
    std::vector<Fixture> many(VIEW_MAX + 100, Fixture{1, 2020, true});
    given(std::move(many));
    view_build(ViewByTitle);
    CHECK(view_count() == VIEW_MAX);
    CHECK(view_row(0) == 0);
    CHECK(view_row(VIEW_MAX - 1) == VIEW_MAX - 1);
}

/* Rebuilding is what every key that changes the list does, so it must not
 * depend on what the last build left behind. */
TEST_CASE("building twice gives the same answer") {
    given({{2, 2020, true}, {1, 2024, true}, {3, 2022, true}});
    view_build(ViewByYear);
    const std::vector<uint16_t> once = rows();
    view_build(ViewByYear);
    CHECK(rows() == once);
    view_build(ViewByCategory);
    view_build(ViewByYear);
    CHECK(rows() == once);
}

}  // TEST_SUITE
