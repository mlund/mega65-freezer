/* The FileHost browser: a catalogue of downloadable titles, and what one of
 * them does to the frozen machine.
 *
 * The catalogue is read from the card at start-up and F fetches a fresh one;
 * the wire has nothing to say.  A disk image goes onto the card and is attached
 * to the frozen machine's drive -- fetched first if the card has not got it,
 * and written over again if what arrived was rubbish.  A program goes nowhere
 * near the card: it is staged in far memory and then stored into the frozen
 * machine's own, which is what a .prg is.  Only the fetching needs a wire, so
 * everything else here is testable without one. */

#include "arp.h"
#include "browser.h"
#include "catalog.h"
#include "colours.h"
#include "common.h"
#include "dma.h"
#include "eth.h"
#include "fat32.h"
#include "fetch.h"
#include "format.h"
#include "ip.h"
#include "lineedit.h"
#include "mega65_regs.h"
#include "prgload.h"
#include "screen.h"
#include "sdcard.h"
#include "shortname.h"
#include "slot.h"
#include "jsoncat.h"
#include "view.h"

#include <mega65.h>
#include <string.h>

/* Said by more than one thing that noticed the same trouble.  A message
 * spelled twice is stored twice: the linker merges strings that match, and two
 * wordings of one idea never do. */
#define CUT_SHORT "CATALOGUE CUT SHORT, SHOWING IT"
#define NO_ROOM "THE CARD WOULD NOT TAKE IT"

/* The catalogue as it sits on the card.  A payload rather than a program, which
 * is the convention IOMAP.M65 and M65THUMB.M65 already follow. */
#define CATALOG_FILE "CATALOG.M65"
/* The endpoint that lists everything.  The proxy serves it as JSON with no
 * stated length, so the body ends at the connection close; jsoncat.h turns it
 * into the fixed-width records the rest of this file reads, which is the same
 * format the card's copy is in. */
#define CATALOG_PATH "/php/readfilespublic.php"
/* How much JSON a catalogue may be, which is not how much room the records
 * need: the limit bounds the reply as it arrives, and 292KB of JSON becomes
 * 33KB of records.  Passing the buffer's size instead refuses the real
 * catalogue four times over -- measured on hardware, which stopped after 63KB
 * with a quarter of the list.  What bounds the records is fetch_store(). */
static constexpr uint32_t CATALOGUE_MAX_BYTES = 1024UL * 1024;
/* Where to fetch from, when whoever runs the network does not say.  A file
 * rather than a screen to type on: the address is settled once per network and
 * a card is easier to edit than a machine with no keyboard driver of its own.
 * One line, an address in figures -- there is no resolver here to turn a name
 * into one. */
#define SERVER_FILE "HTTP-IP.TXT"
/* How long a proxy line can be, which fetch.h owns: this file does not know
 * what protocol is underneath and has no business sizing its host field. */
static constexpr uint8_t SERVER_TEXT_BYTES = FETCH_SERVER_TEXT_BYTES;

/* Read whole and indexed in place.  Chip RAM the tool has to itself: hyppo's
 * 384KB freeze region covers it, so the resume puts the frozen program's own
 * bytes back over whatever is left here.  The same scratch bank ROMLOAD stages
 * a ROM image in, and for the same reason.
 *
 * Not a linker section: link.ld's regions are the 16-bit space, and this is a
 * 28-bit DMA target.  ROM_CHARSET and CHARGEN_ADDRESS are constants for that
 * reason too. */
static constexpr Addr28 CATALOG_BUFFER = 0x40000;
static constexpr uint32_t CATALOG_BUFFER_BYTES = 0x10000;
/* Where a program is staged, above the catalogue in the same bank.  Whole
 * before any of it reaches the frozen machine: there is no transaction on the
 * freeze slot, so half a program written into it resumes as a broken machine,
 * and a transfer that fails here costs nothing but the buffer.
 *
 * 64KB is also all a program can be, the frozen machine's memory being 16 bits
 * wide.  It leaves the catalogue 511 records at the format's own stride, one
 * under what the browser can index, so the buffer is the bound that bites and
 * the index never is. */
static constexpr Addr28 PRG_BUFFER = CATALOG_BUFFER + CATALOG_BUFFER_BYTES;
static constexpr uint32_t PRG_BUFFER_BYTES = 0x10000;
/* One DMA fill's worth: lfill counts in 16 bits, and the buffer is wider. */
static constexpr uint16_t CLEAR_STEP = 0x8000;

/* Kilobytes, as a shift: the sizes run past what a 16-bit decimal holds, and
 * the browser is choosing between titles rather than auditing bytes. */
static constexpr uint8_t KILOBYTE_SHIFT = 10;

/* Where the frozen machine's internal 3.5" drive is.  One drive here: the
 * freeze menu's own chooser is what picks between the two. */
static constexpr uint8_t ATTACH_DRIVE = 0;

/* record_count is the header's, clamped in load_catalog() to what the buffer
 * holds, so it is the count the browser may index rather than the count the
 * file claims. */
static struct CatalogHeader header;
static uint16_t selected;
static uint16_t first_shown;

/* One record at a time in the 16-bit window; the rest stays in far memory. */
/* Up in .netbuf for the same reason as the sector above: the 16-bit window is
 * the scarce thing here, and a record being unpacked is not on the path any
 * frame takes. */
static __attribute__((section(".netbuf"))) uint8_t raw_record[CATALOG_HEADER_BYTES];
/* The JSON turned into records as it arrives, because 292KB of it will not fit
 * anywhere on this machine and never exists whole. */
static struct JsonCatalog transcoder;
/* Where the next record goes, and so how long the catalogue is when the reply
 * ends.  Counts from past the header, which cannot be written until the record
 * count is known. */
static uint32_t catalog_at;
/* Whether what is arriving is JSON to transcode or bytes to keep as they are:
 * a program is fetched into the same window and must not go through the
 * transcoder. */
static bool transcoding;
/* One sector, for copying the finished catalogue out of far memory and onto
 * the card -- fat32's writer takes a pointer in the 16-bit window.  Up in
 * .netbuf with the frame buffers: half a kilobyte is far more than the window
 * has to spare, and this is only touched once a fetch is over. */
static __attribute__((section(".netbuf"))) uint8_t catalog_sector[SD_SECTOR_SIZE];
static struct CatalogRecord record;

/* Whether the freeze slot has been located.  Deferred until an attach actually
 * needs it: read_freeze_slot_start_sector() makes hyppo walk the system
 * partition, which is SD reads, and browsing without attaching is the common
 * case. */
static bool slot_located;

/* Which file a fetch is filling, or 0 for far memory.  fetch.c owns the
 * network and nothing else, so where the bytes land is decided here. */
static uint32_t store_sector;
/* Which far buffer a fetch fills when it is not filling a file: the catalogue's
 * own, or the one a program is staged in. */
static Addr28 store_buffer;

/* What was typed at the `/` prompt, folded to upper case once so the comparison
 * against each title does not fold it again per record.  Empty is no search,
 * which is why an empty prompt is how a search is cleared. */
/* Kept out of zero page deliberately.  The LTO allocator put it there, which
 * is what it is for -- but forty-one bytes of it, for a buffer touched only
 * while somebody is typing, are forty-one the imaginary registers cannot have.
 * Measured at -364 bytes, almost none of it here: the saving is in the code
 * the allocator then generates everywhere else.
 *
 * Safe as a bare section name because this file is compiled for the machine
 * alone; a file that also builds for the host would need the __mos__ guard,
 * Mach-O wanting "segment,section". */
__attribute__((section(".bss"))) static char search[CATALOG_TITLE_BYTES + 1];

/* One keypress, with the queue left empty behind it so the next wait is not
 * answered by this one. */
static uint8_t wait_key(void) {
    uint8_t key;
    while (!(key = ASCIIKEY)) {
        ;
    }
    ASCIIKEY = 0;
    return key;
}

/* Reads an address, and a port after it if the text names one, and tells
 * fetch.c.  Every way of naming a server goes through here, so none of them can
 * set one without the fetch hearing about it.
 *
 * The port starts at HTTP's own rather than at whatever is in force: a bare
 * address means the reserved port, and a text that says nothing about a port
 * cannot be meant to keep one somebody typed an hour ago.
 *
 * The text is written over where it names a host, so it must be the caller's
 * own copy. */
static bool set_server(char* text) {
    uint8_t ip[IPV4_BYTES];
    /* Zero unless the text names one, which fetch.c reads as "the protocol's
     * own": which port that is belongs down there, not here. */
    uint16_t port = 0;
    if (!ip_parse(text, ip, &port)) {
        return false;
    }
    /* Whatever follows the first space, up to whatever ends the line.  Nothing
     * where the line named no host, which fetch.c reads as "ask by the
     * address" -- right for a proxy that answers for whatever it is asked. */
    char* host = text;
    while (*host && *host != ' ') {
        host++;
    }
    while (*host == ' ') {
        host++;
    }
    for (char* at = host; *at; at++) {
        if (*at == ' ' || *at == '\r' || *at == '\n') {
            *at = 0;
            break;
        }
    }
    fetch_set_server(ip, port, *host ? host : nullptr);
    return true;
}

/* One whole row, which is also how a row is cleared. */
static void draw_line(uint8_t y, uint8_t colour, const char* text) {
    draw_field(SCREEN_CELL(0, y), colour, text, SCREEN_COLS);
}

/* The status line, which is the one row that says what just happened. */
static void show_status(uint8_t colour, const char* text) {
    draw_line(STATUS_Y, colour, text);
}

/* Reads record `index` out of far memory into `record`. */
static void fetch_record(uint16_t index) {
    lcopy(CATALOG_BUFFER + (Addr28)catalog_record_offset(header.record_bytes, index),
        (Addr28)(uint16_t)raw_record,
        CATALOG_HEADER_BYTES);
    catalog_record(raw_record, &record);
}

/* Kilobytes, rounded up so that a file always reads as at least 1KB, and a
 * transfer never reads as having fetched nothing. */
static char* append_kb(char* at, uint32_t bytes) {
    at = append_dec(at, (uint16_t)((bytes + (1 << KILOBYTE_SHIFT) - 1) >> KILOBYTE_SHIFT));
    return append_str(at, "KB");
}

/* The year as four figures, or nothing where the catalogue did not say. */
static void draw_year(uint16_t cell, uint8_t colour, uint16_t year) {
    char text[CATALOG_YEAR_BYTES + 1];
    text[0] = 0;
    if (year) {
        *append_dec(text, year) = 0;
    }
    draw_field(cell, colour, text, WIDTH_YEAR);
}

/* One row of the list, or blank where the index has run out. */
static void draw_row(uint16_t row) {
    const uint8_t y = LIST_TOP_Y + (uint8_t)(row - first_shown);
    if (row >= view_count()) {
        draw_line(y, SchemeText, "");
        return;
    }

    fetch_record(view_row(row));
    const uint8_t colour = (row == selected) ? (SchemeSelected | AttribReverse) : SchemeText;

    draw_field(SCREEN_CELL(COLUMN_TITLE, y), colour, record.title, WIDTH_TITLE);
    draw_field(SCREEN_CELL(COLUMN_AUTHOR, y), colour, record.author, WIDTH_AUTHOR);
    draw_field(SCREEN_CELL(COLUMN_CATEGORY, y),
        colour,
        catalog_category_name(record.category),
        WIDTH_CATEGORY);
    draw_year(SCREEN_CELL(COLUMN_YEAR, y), colour, record.year);
    draw_field(
        SCREEN_CELL(COLUMN_KIND, y), colour, record.kind == CatalogD81 ? "D81" : "PRG", WIDTH_KIND);
}

/* Every visible row, from whichever record is at the top. */
static void draw_list(void) {
    for (uint8_t row = 0; row < LIST_ROWS; row++) {
        draw_row(first_shown + row);
    }
}

/* ASCII, which is what the catalogue carries; the screen's own conversion is
 * screencode.h's business and happens after this. */
static uint8_t to_upper(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - ('a' - 'A')) : c;
}

/* Case-insensitively, and anywhere in the title rather than at the start: a
 * user looking for "MONKEY" should not have to know the title begins with a
 * quotation mark. */
static bool title_contains(const char* text, const char* upper_needle) {
    for (uint8_t start = 0; text[start]; start++) {
        uint8_t i = 0;
        while (upper_needle[i] && to_upper((uint8_t)text[start + i]) == (uint8_t)upper_needle[i]) {
            i++;
        }
        if (!upper_needle[i]) {
            return true;
        }
    }
    return false;
}

/* The two questions view.c cannot answer, since it holds no records: what this
 * one contains, and what the user is looking for. */
bool view_matches(uint16_t index) {
    if (!search[0]) {
        return true;
    }
    fetch_record(index);
    return title_contains(record.title, search);
}

/* Years are ranked from this rather than stored: a byte cannot hold a year, and
 * the catalogue's own run from 2020.  One before it and everything earlier
 * share a rank, which puts them together at the far end where they belong. */
static constexpr uint16_t VIEW_YEAR_BASE = 1900;

uint8_t view_key(uint16_t index, enum ViewOrder order) {
    fetch_record(index);
    if (order == ViewByYear) {
        /* Newest first, counted down so the sort itself stays ascending. */
        if (record.year <= VIEW_YEAR_BASE) {
            return VIEW_KEY_UNKNOWN;
        }
        const uint16_t rank = record.year - VIEW_YEAR_BASE;
        return rank >= VIEW_KEY_UNKNOWN ? 0 : (uint8_t)(VIEW_KEY_UNKNOWN - rank);
    }
    /* Alphabetically by the name drawn in the column, which catalog.c derives
     * from that table; a record naming no category sorts after all of them. */
    const uint8_t rank = catalog_category_rank(record.category);
    return rank ? rank : VIEW_KEY_UNKNOWN;
}

/* Scrolls only when the selection has left the window, and repaints only the
 * two rows that changed when it has not. */
static void move_to(uint16_t index) {
    const uint16_t previous = selected;
    selected = index;

    /* The status describes the record it was raised for, so it stops being
     * true the moment a different one is highlighted. */
    show_status(SchemeText, "");

    if (selected < first_shown) {
        first_shown = selected;
    } else if (selected >= first_shown + LIST_ROWS) {
        first_shown = selected - LIST_ROWS + 1;
    } else {
        draw_row(previous);
        draw_row(selected);
        return;
    }
    draw_list();
}

/* One clamp for every key that moves the selection, so the ends of the list
 * cannot be guarded four different ways. */
static void move_by(int16_t delta) {
    if (!view_count()) {
        return;
    }
    int16_t at = (int16_t)selected + delta;
    if (at < 0) {
        at = 0;
    }
    if (at >= (int16_t)view_count()) {
        at = (int16_t)view_count() - 1;
    }
    move_to((uint16_t)at);
}

/* Why a fetch did not happen, in the same words wherever it was asked for.
 *
 * A refusal is the one that carries a number: the HTTP status is the
 * difference between a file the server has not got and one it will not part
 * with, and only the second is worth arguing with. */
static void say_fetch_failed(enum FetchResult result, const char* also) {
    static const char* const why[] = {
        "",
        "NO ADDRESS: NOTHING ANSWERED",
        ("NO PROXY: SET " SERVER_FILE),
        "THE PROXY DID NOT ANSWER",
        "", /* refused: the code is named below instead */
        "THE TRANSFER STOPPED PART WAY",
        "MORE THAN THERE IS ROOM FOR",
        NO_ROOM,
        "THE FETCH WAS STOPPED",
    };
    static_assert(sizeof why / sizeof *why == FetchStopped + 1,
        "a fetch result with no message would index past this table");

    /* Wider than the row on purpose: the two halves together run past 80
     * characters -- naming no server and then what is left on the card is 84 --
     * and draw_field() clips what will not fit.  The buffer is what must not be
     * the thing that gives. */
    char text[SCREEN_COLS + 16];
    char* at = append_str(text, why[result]);
    if (result == FetchRefused) {
        at = append_dec(append_str(text, "THE SERVER REFUSED IT, CODE "), fetch_status());
    }
    /* Both on one line: the reason is what to do something about, and what is
     * now on the card is what to know before trying again. */
    if (also) {
        at = append_str(at, " -- ");
        at = append_str(at, also);
    }
    *at = 0;
    show_status(SchemeError, text);
}

/* The image the selected record names, onto the card under `name`, whole or not
 * at all.
 *
 * Contiguous because the hardware reads a mounted image by counting sectors
 * from a base rather than by walking the FAT (mega65-core sdcardio.vhdl:380),
 * which is also why nothing can be mounted out of memory.
 *
 * The length comes from the catalogue rather than from the server, the file
 * having to exist at its full size before the first block arrives.  So a
 * transfer that stops part way leaves a file of the right name and length
 * holding a correct head and a stale tail, and hyppo checks an image's length
 * and never its contents -- it would mount as a working disk.  Removing it is
 * the whole of the answer: this owns the cleanup so that no caller can be the
 * one that forgot it.
 *
 * `replace` is the file that is already there and wrong.  It is written over
 * where it lies rather than removed and remade: the file is the length the
 * catalogue names and hyppo has just mounted it, so it is contiguous and the
 * right size, and remaking it could fail after the delete had already destroyed
 * it -- the writer takes only wholly free FAT sectors, so the space a delete
 * returns is usually not space it can take again. */
static bool fetch_image(const char* name, bool replace) {
    if (!record.size) {
        show_status(SchemeError, "UNKNOWN CATALOGUE SIZE");
        return false;
    }
    if (fat32_open_file_system() != FreezerOk) {
        show_status(SchemeError, "CANNOT READ THE CARD");
        return false;
    }

    store_sector = replace ? fat32_file_first_sector(name, record.size)
                           : fat32_create_contiguous_file(name, record.size);
    if (!store_sector) {
        show_status(SchemeError, replace ? "WRONG CARD SIZE" : "COULDNT MAKE FILE");
        return false;
    }

    uint32_t got = 0;
    const enum FetchResult result = fetch_file(record.path, record.size, &got);
    store_sector = 0;
    if (result != FetchOk) {
        /* Only a file this call created is removed.  One being replaced is
         * already the right length and contiguous, so leaving it lets the user
         * press R again and write over it a second time -- where deleting it
         * would give back space the writer usually cannot take again, and lose
         * the only slot the repair could have used.
         *
         * Said as it happened rather than as it was meant to: a delete that
         * fails leaves exactly the file this was written to prevent. */
        const char* also = replace    ? "IT IS STILL WRONG -- R RETRIES"
            : fat32_delete_file(name) ? "THE FILE WAS REMOVED"
                                      : "A BAD FILE IS LEFT ON THE CARD";
        say_fetch_failed(result, also);
        return false;
    }
    return true;
}

/* Mounts it, saying why if it will not.  Not the first attach of all, which is
 * a question rather than an instruction and reads its own answer. */
/* `name` is not const because hyppo's attach takes it as it is given. */
static bool attached(char* name) {
    if (mega65_dos_attach(name, ATTACH_DRIVE)) {
        show_status(SchemeError, hyppoerror_to_screen(mega65_h_geterrorcode()));
        return false;
    }
    return true;
}

/* The card already holds this image, so which of the two was meant.
 *
 * Asked rather than assumed because neither answer is safe to guess: attaching
 * a corrupt image mounts rubbish, and fetching over a good one costs an 800KB
 * transfer nobody asked for.  A file that arrived wrong cannot be told from one
 * that arrived whole -- hyppo checks length and never contents -- so the user
 * is the only judge of which this is.
 *
 * The mount has already happened by the time this is asked -- the attach is
 * how the card was asked whether it holds the file at all.  Answering A only
 * has to write the descriptor that survives the resume.  Answering R writes
 * over a file the drive is still attached to, and if the fetch then fails the
 * file is removed under it; what the frozen machine resumes with is unaffected,
 * that descriptor being written only on the way out. */
static bool attach_or_replace(char* name) {
    char text[SHORT_NAME_BYTES + 32];
    char* at = append_str(text, "ATTACH OR REPLACE ");
    at = append_str(at, name);
    at = append_str(at, "? A/R");
    *at = 0;
    show_status(SchemeWarning, text);

    const uint8_t key = wait_key();
    if (key != 'R' && key != 'r') {
        /* Anything else leaves the mount as it is; only A and RETURN go on to
         * write the descriptor, so a mistyped key does nothing rather than
         * something. */
        return key == 'A' || key == 'a' || key == KEY_RETURN;
    }
    return fetch_image(name, true) && attached(name);
}

/* Slot 0 is the live freeze: writing to a stored slot would change a machine
 * nobody is about to resume.  Deferred until something needs it, since locating
 * it makes hyppo walk the system partition. */
static void locate_slot(void) {
    if (!slot_located) {
        request_freeze_region_list();
        freeze_slot_start_sector = read_freeze_slot_start_sector(0);
        slot_located = true;
    }
}

/* The selected program into the frozen machine's memory, which is where a .prg
 * belongs: no file on the card, and so nothing of the FAT involved.
 *
 * Fetched whole into a buffer first.  There is no transaction on the freeze
 * slot -- half a program stored into it resumes as a broken machine -- so the
 * transfer has to have finished before the first byte goes in, and every rule
 * that could refuse it is settled before that too.
 *
 * It is not started.  The frozen program counter is wherever the machine was,
 * which may be mid-interrupt or inside the KERNAL, and swapping a program in
 * under it is undefined; in MEGA65 mode the ROM's own source says the $02B0
 * keyboard buffer is no longer generally honoured.  So the machine is left
 * loaded and the user resumes and types RUN, which is a thing they can see. */
static void load_program(void) {
    show_status(SchemeText, "FETCHING THE PROGRAM...");
    store_sector = 0;
    store_buffer = PRG_BUFFER;
    uint32_t got = 0;
    const enum FetchResult result = fetch_file(record.path, PRG_BUFFER_BYTES, &got);
    if (result != FetchOk) {
        /* Nothing to clear up: the frozen machine has not been touched. */
        say_fetch_failed(result, nullptr);
        return;
    }

    locate_slot();

    uint8_t head[2];
    lcopy(PRG_BUFFER, (Addr28)(uint16_t)head, sizeof head);
    struct PrgPlan plan;
    const enum PrgVerdict verdict = prg_plan(head, got, freeze_io_peek(FROZEN_D030), &plan);
    if (verdict != PrgOk) {
        show_status(SchemeError, prg_verdict_text(verdict));
        return;
    }

    /* The border says the card is busy, as the ROM loader's own copy does. */
    VICIV.bordercol = SchemeHighlight;
    const enum FreezerError stored =
        freeze_store_range(plan.load, PRG_BUFFER + sizeof head, got - sizeof head);
    VICIV.bordercol = SchemeBorder;
    if (stored != FreezerOk) {
        /* Said as strongly as it deserves: the slot has no transaction, so what
         * is in the frozen machine now is part of this program over part of
         * whatever was there, and resuming runs that. */
        show_status(SchemeError, "PARTIAL WRITE ERROR-DO NOT RESUME");
        return;
    }

    /* Where the program ends, which is what BASIC reads to find the end of it.
     * Both bytes in one store: they share a sector, and each freeze_poke() is a
     * read and a write of the whole of it.  The line links need no repair, the
     * program going back to the address it was saved from. */
    const uint16_t end = plan.end;
    (void)freeze_store_range(plan.pointer, (Addr28)(uint16_t)&end, sizeof end);

    show_status(SchemeHighlight, "LOADED -- RESUME AND TYPE RUN");
}

/* Attaching writes the frozen process descriptor as well as the live one: the
 * unfreeze path re-reads those fields and reattaches from them, so a mount that
 * only happened live would not survive the resume.  This is the pair
 * makedisk/main.c performs after creating an image. */
static void attach_selected(void) {
    char name[SHORT_NAME_BYTES];

    fetch_record(view_row(selected));
    if (record.kind == CatalogPrg) {
        load_program();
        return;
    }
    if (record.kind != CatalogD81) {
        show_status(SchemeWarning, "ONLY D81 AND PRG");
        return;
    }

    locate_slot();

    catalog_short_name(record.path, record.kind, name);
    if (mega65_dos_attach(name, ATTACH_DRIVE)) {
        /* Not on the card is the ordinary case rather than a failure: the
         * catalogue names what FileHost has and the card holds what has been
         * fetched, so the first attach is also how the question is asked.
         * Anything else is a failure and is reported as one. */
        const uint8_t code = mega65_h_geterrorcode();
        if (!hyppo_file_absent(code)) {
            show_status(SchemeError, hyppoerror_to_screen(code));
            return;
        }
        if (!fetch_image(name, false) || !attached(name)) {
            return; /* which said why */
        }
    } else if (!attach_or_replace(name)) {
        return;
    }

    if (hdos_new_attach) {
        copy_imageproc_to_freezeregion(ATTACH_DRIVE, 0);
    } else {
        old_store_selected_disk_image(ATTACH_DRIVE, name);
    }

    char text[SHORT_NAME_BYTES + 16];
    char* at = append_str(text, "ATTACHED ");
    at = append_str(at, name);
    *at = 0;
    show_status(SchemeHighlight, text);
}

/* Dotted quad, which the server prompt spells as well as the probe. */
static char* append_ip(char* at, const uint8_t* ip) {
    for (uint8_t i = 0; i < IPV4_BYTES; i++) {
        if (i) {
            *at++ = '.';
        }
        at = append_dec(at, ip[i]);
    }
    return at;
}

#ifdef ETH_PROBE
/* Whether the ethernet controller works from the frozen state, which no source
 * answers and only hardware can.  An ARP request needs no address of our own
 * and no checksum, and its reply proves both directions at once: a machine that
 * hears one has transmitted and received while a freezer tool was running.
 *
 * The frame count beside it is the weaker but unconditional half -- a LAN
 * broadcasts constantly, so frames arriving proves the receiver alone even if
 * nothing answers us.
 *
 * Built only when asked for.  It answered its question -- the controller does
 * work from a frozen machine -- and what is left is a way to ask again on a
 * machine where the wire is suspect, which is not worth a kilobyte of every
 * shipped tool.  The addresses below are this LAN's, so it needs recompiling to
 * point elsewhere in any case. */
static constexpr uint8_t PROBE_TARGET[IPV4_BYTES] = {192, 168, 68, 57};
/* A real address rather than zeros: an ARP probe with a zero sender is
 * answered by some stacks and ignored by others, and a request carrying a
 * sender is also cached by whoever receives it -- which makes the target's own
 * ARP table a witness that the frame was transmitted, without needing anything
 * privileged listening on the wire. */
static constexpr uint8_t PROBE_SENDER[IPV4_BYTES] = {192, 168, 68, 234};
static constexpr uint16_t PROBE_POLLS = 200;
static constexpr uint32_t PROBE_POLL_US = 10000;

/* Only ever holds an ARP exchange, and sized by what such a frame is on the
 * wire rather than by ARP's 42 bytes: padded to 60 and reported with its check
 * sequence as 64.  eth_receive() drops what will not fit rather than
 * delivering part of it, so anything shorter here receives nothing at all.  A
 * 1500-byte buffer would be budget the code cannot have -- see
 * cmake/checkbuffers.cmake. */
static uint8_t probe_frame[ETH_MIN_RECEIVED];
/* Separate from the frame that is sent: sharing one buffer is how a read that
 * copied nothing came back looking like our own transmission. */
static uint8_t probe_received[ETH_MIN_RECEIVED];

/* A byte as two hex digits, and where the next one goes. */
static char* append_byte_hex(char* at, uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    *at++ = digits[value >> 4];
    *at++ = digits[value & 0x0F];
    return at;
}

/* A hardware address, colon-separated, as the wire spells it. */
static char* append_mac(char* at, const uint8_t* mac) {
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        if (i) {
            *at++ = ':';
        }
        at = append_byte_hex(at, mac[i]);
    }
    return at;
}

/* A line of the probe report, counted from the top of the list area. */
static void probe_line(uint8_t row, uint8_t colour, const char* text) {
    draw_line(LIST_TOP_Y + row, colour, text);
}

/* Asks who holds a fixed address and reports what came back, or listens
 * without asking so the two can be told apart. */
static void ethernet_probe(bool transmit) {
    char text[64];
    uint8_t mac[MAC_BYTES];
    uint8_t found[MAC_BYTES];

    for (uint8_t row = 0; row < LIST_ROWS; row++) {
        probe_line(row, SchemeText, "");
    }
    show_status(SchemeText, "");

    eth_init();
    eth_mac(mac);

    char* at = append_str(text, "MAC ");
    at = append_mac(at, mac);
    *at = 0;
    probe_line(0, SchemeValue, text);
    probe_line(1, SchemeValue, eth_tx_idle() ? "TRANSMITTER IDLE" : "TRANSMITTER BUSY");

    at = append_str(text, "ASKING WHO HAS ");
    at = append_ip(at, PROBE_TARGET);
    *at = 0;
    probe_line(2, SchemeText, text);

    /* Listening without transmitting tells the two apart: if what arrives is
     * the same ARP broadcast either way, it is our own frame being read back
     * out of the buffer rather than anything off the wire. */
    if (transmit) {
        const uint16_t length = arp_request(probe_frame, mac, PROBE_SENDER, PROBE_TARGET);
        eth_send(probe_frame, length);
        probe_line(3, SchemeText, "SENT");
    } else {
        probe_line(3, SchemeText, "LISTENING ONLY, NOTHING SENT");
    }

    uint16_t frames = 0;
    uint16_t arps = 0;
    uint8_t last_to[MAC_BYTES] = {0};
    uint8_t last_from[MAC_BYTES] = {0};
    uint16_t last_type = 0;
    bool answered = false;
    /* eth_receive() and not net_poll(), which is what every loop holding an
     * address should use: this one holds none, so there is nothing to answer
     * for, and what the probe is measuring is the frames themselves -- a poll
     * that quietly swallowed some would be measuring itself. */
    for (uint16_t poll = 0; poll < PROBE_POLLS && !answered; poll++) {
        const uint16_t got = eth_receive(probe_received, sizeof probe_received);
        if (!got) {
            usleep(PROBE_POLL_US);
            continue;
        }
        frames++;
        /* What was actually read, not just that something was: a count alone
         * cannot tell a real frame from a stale buffer read back. */
        last_type = net_get16(&probe_received[ETH_TYPE]);
        for (uint8_t i = 0; i < MAC_BYTES; i++) {
            last_to[i] = probe_received[i];
            last_from[i] = probe_received[MAC_BYTES + i];
        }
        if (last_type == ETHERTYPE_ARP) {
            arps++;
        }
        answered = arp_reply_from(probe_received, got, PROBE_TARGET, found);
    }

    at = append_str(text, "FRAMES ");
    at = append_dec(at, frames);
    at = append_str(at, "  ARP ");
    at = append_dec(at, arps);
    at = append_str(at, "  LAST TYPE ");
    at = append_byte_hex(at, (uint8_t)(last_type >> 8));
    at = append_byte_hex(at, (uint8_t)last_type);
    *at = 0;
    probe_line(4, frames ? SchemeValue : SchemeWarning, text);

    at = append_str(text, "LAST TO ");
    at = append_mac(at, last_to);
    *at = 0;
    probe_line(6, SchemeValue, text);

    at = append_str(text, "LAST FROM ");
    at = append_mac(at, last_from);
    *at = 0;
    /* Our own address here means the read came back with what we transmitted,
     * not with anything the wire delivered. */
    bool ours = true;
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        ours = ours && last_from[i] == mac[i];
    }
    probe_line(7, ours ? SchemeError : SchemeValue, text);
    if (ours) {
        probe_line(8, SchemeError, "THAT IS OUR OWN FRAME READ BACK");
    }

    if (answered) {
        at = append_str(text, "REPLY FROM ");
        at = append_mac(at, found);
        *at = 0;
        probe_line(5, SchemeHighlight, text);
        show_status(SchemeHighlight, "ETHERNET WORKS FROM THE FROZEN STATE");
    } else {
        probe_line(5, SchemeWarning, "NO REPLY");
        show_status(
            SchemeWarning, frames ? "RECEIVES BUT NOTHING ANSWERED" : "NOTHING HEARD, PRESS A KEY");
    }

    (void)wait_key();
    draw_list();
}
#endif /* ETH_PROBE */

/* Where a fetch puts what arrives: the card when an image is being written,
 * and the buffer the browser indexes otherwise.
 *
 * Which of the two is decided by `store_sector`, a file's first sector never
 * being 0 -- fat32_create_contiguous_file() answers 0 for failure, so there is
 * no second flag to keep in step with it.
 *
 * One sector at a time, or less: a stage of whole sectors goes through
 * fetch_store_blocks() instead, so the only lengths that reach the card here
 * are a single sector and the short tail store_stage() pads. */
bool fetch_store(uint32_t offset, const uint8_t* bytes, uint16_t length) {
    if (store_sector) {
        return fat32_write_file_sector(store_sector, offset, bytes, length);
    }
    if (!transcoding) {
        lcopy((Addr28)(uint16_t)bytes, store_buffer + (Addr28)offset, length);
        return true;
    }
    /* JSON in, 128-byte records out, a record at a time -- the offset means
     * nothing here, since what arrives and what is kept are different lengths.
     * raw_record is borrowed: nothing reads it between one fetch and the
     * catalogue being accepted at the end of one. */
    uint16_t left = length;
    while (left) {
        uint16_t taken = 0;
        if (jsoncat_take(&transcoder, bytes, left, &taken, raw_record)) {
            if (catalog_at + JSONCAT_RECORD_BYTES > CATALOG_BUFFER_BYTES) {
                return false; /* more records than there is room for */
            }
            lcopy((Addr28)(uint16_t)raw_record, CATALOG_BUFFER + (Addr28)catalog_at,
                JSONCAT_RECORD_BYTES);
            catalog_at += JSONCAT_RECORD_BYTES;
        }
        bytes += taken;
        left = (uint16_t)(left - taken);
    }
    return true;
}

bool fetch_stores_image(void) {
    return store_sector != 0;
}

bool fetch_store_blocks(uint32_t offset, const uint8_t* bytes, uint8_t count) {
    return fat32_write_file_sectors(store_sector, offset, bytes, count);
}

/* Kilobytes as they arrive.  Not every block: an image is sixteen hundred of
 * them, and a redraw each would cost more than the transfer.  Every 8KB moves
 * often enough to say the machine is alive.
 *
 * A repeat is drawn however far along it is: a stalled transfer and a hung
 * machine look exactly alike when the screen stops moving, and the number of
 * repeats is the only thing that tells them apart from the outside. */
void fetch_progress(uint32_t so_far, uint32_t total, bool last, bool waiting) {
    static constexpr uint32_t EVERY = 0x2000;
    static uint8_t waits;
    /* Which 8KB step was last drawn.  A fetch that starts again from zero
     * differs from wherever the last one stopped, so there is nothing to
     * reset. */
    static uint8_t drawn;

    if (waiting) {
        waits++;
    } else {
        waits = 0;
        /* Every 8KB, and always the last.  A short block is the last one, which
         * is how the end is known when the server declined to say how big the
         * file is and `total` is 0 -- without that the line would stop at the
         * last multiple rather than at what arrived.
         *
         * Which 8KB step the count is in, and not whether it is a multiple of
         * one: a body arrives in segments the server chose the length of, so
         * the running total steps over the multiples rather than onto them and
         * a mask would match almost never.  That draws nothing at all until
         * the file finishes -- seconds of a still screen, which reads as a
         * hung machine and is the one thing this line exists to prevent. */
        const uint8_t step = (uint8_t)(so_far / EVERY);
        if (step == drawn && so_far != total && !last) {
            return;
        }
        drawn = step;
    }

    char text[40];
    char* at = append_kb(append_str(text, "FETCHED "), so_far);
    if (total) {
        at = append_kb(append_str(at, " OF "), total);
    }
    if (waiting) {
        at = append_str(at, ", WAITING ");
        at = append_dec(at, waits);
    }
    *at = 0;
    show_status(SchemeText, text);
}

/* RUN/STOP, and only that: anything else pressed during a fetch stays on the
 * queue for the browser to answer afterwards.  Taken off the queue here, which
 * is what stops it reaching browse() and leaving the tool as well. */
bool fetch_cancelled(void) {
    if (ASCIIKEY != KEY_RUN_STOP) {
        return false;
    }
    ASCIIKEY = 0;
    return true;
}

/* The server a fetch would go to, as text.  Asked of fetch.c rather than
 * remembered here: a lease can name one, and a screen showing what was typed
 * instead would name an address nothing is fetching from. */
static uint8_t write_server_text(char* text) {
    uint16_t port = 0;
    char* at = append_ip(text, fetch_server(&port));
    /* The reserved port is left unsaid, being what a bare address means. */
    if (port) {
        *at++ = ':';
        at = append_dec(at, port);
    }
    *at = 0;
    return (uint8_t)(at - text);
}

/* Typing the server's address, on the status line.
 *
 * The field opens empty with the address in force named in the prompt, rather
 * than opening on that address: an editor prefilled with thirteen characters
 * is thirteen backspaces before the first useful keystroke, and RETURN on an
 * empty field is then the way to leave it alone.
 *
 * Kept for this session only.  Writing it back to the card wants a FAT writer
 * that can replace a file, where MAKEDISK's can only create one, so the file
 * on the card is how an address survives a reset until then.
 *
 * line_edit() owns the buffer while this owns the screen and the keyboard,
 * which is what lets a full field and a backspace on an empty one be tested on
 * the host rather than typed at an emulator. */
/* A prompt on the status line and an answer typed after it, or false where the
 * user backed out.  Both things this asks for are the same transaction: a
 * question, a field, and RUN/STOP meaning leave it alone.
 *
 * line_edit() owns the buffer while this owns the screen and the keyboard,
 * which is what lets a full field and a backspace on an empty one be tested on
 * the host rather than typed at an emulator. */
[[nodiscard]] static bool edit_line(
    const char* prompt, char* text, uint8_t capacity, uint8_t* length) {
    const uint8_t field = (uint8_t)strlen(prompt);
    *length = 0;
    text[0] = 0;
    for (;;) {
        /* The whole line each key, prompt included.  Drawing the prompt once
         * and repainting only the answer is the obvious economy and measured
         * 161 bytes worse: one draw of a whole row beats two of parts of it. */
        char shown[SCREEN_COLS + 1];
        char* end = append_str(shown, prompt);
        end = append_str(end, text);
        *end = 0;
        draw_line(STATUS_Y, SchemeHighlight, shown);
        static const uint8_t caret = LINE_EDIT_CARET;
        draw_fragment(SCREEN_CELL(field + *length, STATUS_Y), SchemeHighlight, &caret, 1);

        const uint8_t key = wait_key();
        if (key == KEY_RUN_STOP || key == KEY_ESC) {
            show_status(SchemeText, "");
            return false;
        }
        if (line_edit(text, capacity, length, key)) {
            return true;
        }
    }
}

/* Reads a new server address from the user and tells the fetch about it. */
static void edit_server_address(void) {
    char prompt[32 + SERVER_TEXT_BYTES];
    char* at = append_str(prompt, "NEW PROXY (NOW ");
    at += write_server_text(at);
    at = append_str(at, "): ");
    *at = 0;

    char text[SERVER_TEXT_BYTES];
    uint8_t length;
    if (!edit_line(prompt, text, sizeof text, &length)) {
        return;
    }

    if (!length) {
        show_status(SchemeText, "");
    } else if (set_server(text)) {
        show_status(SchemeHighlight, "SERVER SET -- F FETCHES");
    } else {
        show_status(SchemeError, "THAT IS NOT AN ADDRESS");
    }
}

/* The server address off the card, and whether the card had one to give.
 *
 * Read into the catalogue buffer, which load_catalog() clears and fills
 * afterwards: read_file_from_sdcard() is unbounded, so a file left under this
 * name by mistake lands in 64KB of scratch rather than over something.
 *
 * A whole sector is cleared first rather than the text's own width, because
 * hyppo writes a file a sector at a time: the bytes after the file's last are
 * that sector's, and a digit among them would lengthen the address read. */
static bool read_server_address(void) {
    lfill(CATALOG_BUFFER, 0, SD_SECTOR_SIZE);
    if (read_file_from_sdcard(SERVER_FILE, CATALOG_BUFFER)) {
        return true; /* no file is not a bad file */
    }
    char text[SERVER_TEXT_BYTES];
    lcopy(CATALOG_BUFFER, (Addr28)(uint16_t)text, sizeof text);
    text[sizeof text - 1] = 0;
    return set_server(text);
}

/* Zeroes the catalogue buffer, a fill at a time: lfill counts in 16 bits and
 * the buffer is wider. */
static void clear_buffer(void) {
    for (uint32_t at = 0; at < CATALOG_BUFFER_BYTES; at += CLEAR_STEP) {
        lfill(CATALOG_BUFFER + (Addr28)at, 0, CLEAR_STEP);
    }
}

/* What is in the buffer, read as a catalogue, given that `bytes` of it are
 * real.
 *
 * The count is the file's claim and the browser's bound: fetch_record() reads
 * at the offset it names, so a count past what arrived -- a truncated transfer,
 * or a header that simply says more than it brought -- reads whatever lies
 * beyond the buffer and offers it as a title to attach.
 *
 * `bytes` is what is in the buffer, so it is never more than the buffer holds:
 * the card path passes the buffer's own size and the fetch is refused above
 * that before a block is written. */
[[nodiscard]] static bool accept_catalog(uint32_t bytes) {
    lcopy(CATALOG_BUFFER, (Addr28)(uint16_t)raw_record, CATALOG_HEADER_BYTES);
    if (bytes < CATALOG_HEADER_BYTES || !catalog_header(raw_record, &header)) {
        header = (struct CatalogHeader){0};
        show_status(SchemeError, "THAT IS NOT A CATALOGUE");
        return false;
    }

    const uint16_t fits = hw_div16(bytes - CATALOG_HEADER_BYTES, header.record_bytes);
    /* Less than it says it is: the rows that arrived are worth showing, and
     * the warning is the whole of what the caller needs to know about it. */
    if (header.record_count > fits) {
        header.record_count = fits;
        show_status(SchemeWarning, CUT_SHORT);
    }
    if (!header.record_count) {
        show_status(SchemeWarning, "THE CATALOGUE IS EMPTY");
        return false;
    }
    return true;
}

/* The catalogue from the card, if there is one worth having. */
static bool load_catalog(void) {
    clear_buffer();
    if (read_file_from_sdcard(CATALOG_FILE, CATALOG_BUFFER)) {
        /* The buffer is cleared by now, so a count left over from whatever was
         * in it would draw that many blank rows and claim they are records. */
        header = (struct CatalogHeader){0};
        show_status(SchemeError, "NO " CATALOG_FILE " ON THE CARD");
        return false;
    }
    /* The loader says nothing about the length, so the whole buffer is what
     * may have been filled. */
    /* Cut short is still worth browsing, and the warning already said so. */
    return accept_catalog(CATALOG_BUFFER_BYTES);
}

/* How many rows are shown, and of how many there are whenever those differ --
 * a search hiding some, or a catalogue with more records than the index can
 * address.  Either way a list that is not the whole list says so rather than
 * reading as a complete one. */
static void draw_count(void) {
    char text[16];
    char* at = append_dec(text, view_count());
    if (view_count() != header.record_count) {
        at = append_str(at, " OF ");
        at = append_dec(at, header.record_count);
    }
    *at = 0;
    draw_field(SCREEN_CELL(COUNT_X, COUNT_Y), SchemeTextDim, text, SCREEN_COLS - COUNT_X);
}

/* Back to the top of a list that has just been replaced.  Not move_to(), which
 * clears the status line: what a fetch has to say outlives its redraw. */
static void show_from_top(void) {
    selected = 0;
    first_shown = 0;
    draw_count();
    draw_list();
}

/* A new order or a new search makes a different list, so the selection goes
 * back to the top: the row a user was on means nothing in it. */
static void rebuild(enum ViewOrder order) {
    view_build(order);
    show_from_top();
}

/* A catalogue that has just arrived: no search, and the order it came in.
 * Reset rather than rebuilt, since a search from the last catalogue would
 * silently hide records of this one. */
static void show_catalog(void) {
    search[0] = 0;
    view_reset(header.record_count);
    rebuild(ViewByTitle);
}

/* What to look for, and an empty answer to stop looking.  The list is rebuilt
 * under the order in force, so a search narrows what is shown without
 * disturbing how it is arranged. */
static void edit_search(void) {
    char text[CATALOG_TITLE_BYTES + 1];
    uint8_t length;
    if (!edit_line("FIND: ", text, sizeof text, &length)) {
        return;
    }

    /* Folded once here rather than per record: every title in the list is
     * compared against this, and there may be VIEW_MAX of them. */
    for (uint8_t i = 0; i <= length; i++) {
        search[i] = (char)to_upper((uint8_t)text[i]);
    }
    rebuild(view_order());
    show_status(SchemeText, view_count() ? "" : "NOTHING MATCHES THAT");
}

/* Title, then year, then category, and round again.  One key rather than
 * three, and nothing said about it: the list itself is the answer. */
static void cycle_order(void) {
    const uint8_t next = view_order() + 1;
    rebuild(next > ViewByCategory ? ViewByTitle : (enum ViewOrder)next);
}

/* Keeps the catalogue that just arrived, so the next run needs no network.
 *
 * After the transcode and not before: what is worth keeping is 33KB of records
 * the browser can index, not 292KB of JSON it would have to parse again.
 *
 * Replaced in place where the card's copy is already the right length, and
 * only otherwise deleted and remade.  The order matters: fat32's writer takes
 * only wholly free sectors, so a delete usually gives back space it cannot
 * take again -- and a failure here would leave no catalogue at all where there
 * had been a usable one. */
static bool save_catalog(uint32_t bytes) {
    if (fat32_open_file_system() != FreezerOk) {
        return false;
    }
    uint32_t sector = fat32_file_first_sector(CATALOG_FILE, bytes);
    if (!sector) {
        (void)fat32_delete_file(CATALOG_FILE);
        sector = fat32_create_contiguous_file(CATALOG_FILE, bytes);
    }
    if (!sector) {
        return false;
    }
    for (uint32_t at = 0; at < bytes; at += SD_SECTOR_SIZE) {
        const uint32_t rest = bytes - at;
        const uint16_t n = rest < SD_SECTOR_SIZE ? (uint16_t)rest : SD_SECTOR_SIZE;
        lcopy(CATALOG_BUFFER + (Addr28)at, (Addr28)(uint16_t)catalog_sector, n);
        if (!fat32_write_file_sector(sector, at, catalog_sector, n)) {
            return false;
        }
    }
    return true;
}

/* The catalogue over the wire, transcoded into the buffer the browser indexes
 * and then kept on the card.
 *
 * A fetch that fails costs the list on screen and not the copy on the card:
 * what the buffer holds afterwards is whatever the fetch left, so the card's
 * copy is read back rather than trusted. */
static void fetch_catalog(void) {
    show_status(SchemeText, "FETCHING THE CATALOGUE...");

    /* Cleared first, so a catalogue shorter than the last one does not leave
     * the tail of the old one readable behind it. */
    clear_buffer();
    store_sector = 0;
    transcoding = true;
    jsoncat_begin(&transcoder);
    catalog_at = CATALOG_HEADER_BYTES;
    uint32_t got = 0;
    const enum FetchResult result = fetch_file(CATALOG_PATH, CATALOGUE_MAX_BYTES, &got);
    transcoding = false;
    if (result != FetchOk) {
        /* Whatever landed is half a catalogue, so the card's copy goes back
         * first -- and its own complaints are said before the one the user
         * pressed a key to hear. */
        (void)load_catalog();
        show_catalog();
        say_fetch_failed(result, nullptr);
        return;
    }

    /* The header last, the record count not being known until here. */
    const bool whole = jsoncat_end(&transcoder, raw_record);
    lcopy((Addr28)(uint16_t)raw_record, CATALOG_BUFFER, CATALOG_HEADER_BYTES);

    const bool usable = accept_catalog(catalog_at);
    show_catalog();
    if (!usable) {
        return; /* accept_catalog() has already said what is wrong with it */
    }
    /* A reply that stopped part way is indistinguishable from a whole one by
     * its length -- the endpoint states none -- so only the shape of the JSON
     * says so, and half a list is not worth keeping over a whole one. */
    if (!whole) {
        show_status(SchemeWarning, CUT_SHORT);
        return;
    }
    /* Once, into a variable: written twice in one expression this would write
     * the card twice. */
    const bool kept = save_catalog(catalog_at);
    show_status(kept ? SchemeHighlight : SchemeWarning,
        kept ? "FETCHED AND KEPT ON THE CARD" : NO_ROOM);
}

/* The browser itself: draws the list and answers keys until RUN/STOP. */
static void browse(void) {
    draw_list();

    for (;;) {
        switch (wait_key()) {
            case KEY_CURSOR_UP:
                move_by(-1);
                break;
            case KEY_CURSOR_DOWN:
                move_by(1);
                break;
            /* Left and right page, as they do in the freezer's disk and ROM
             * choosers; the comma and full stop do the same, as they do in the
             * freeze menu itself, and are the pair a keyboard without a
             * reachable shift can still send. */
            case KEY_CURSOR_LEFT:
            case ',':
                move_by(-LIST_ROWS);
                break;
            case KEY_CURSOR_RIGHT:
            case '.':
                move_by(LIST_ROWS);
                break;
            case KEY_HOME:
                move_to(0);
                break;
            case KEY_RETURN:
                if (view_count()) {
                    attach_selected();
                }
                break;
            case 'S':
            case 's':
                cycle_order();
                break;
            case 'T':
            case 't':
                edit_server_address();
                break;
            case '/':
                edit_search();
                break;
            case 'F':
            case 'f':
                fetch_catalog();
                break;
#ifdef ETH_PROBE
            case 'E':
            case 'e':
                ethernet_probe(true);
                break;
            case 'R':
            case 'r':
                ethernet_probe(false);
                break;
#endif
            case KEY_RUN_STOP:
            case KEY_ESC:
                return;
            default:
                break;
        }
    }
}

int main(void) {
    freezer_tool_start();

    setup_menu_screen_80col();
    clear_colour_ram();
    blank_screen();
    draw_fragments(menu_fixed_stream());
    draw_rule(SCREEN_CELL(0, LIST_TOP_Y - 1), SCREEN_COLS);
    draw_rule(SCREEN_CELL(0, STATUS_Y - 1), SCREEN_COLS);

    /* A tool is exec'd over whatever ran before it, so hdos_new_attach has to
     * be established here; the freeze slot waits until an attach needs it. */
    mega65_dos_init();

    /* Browsing runs whether or not a catalogue was found: with none, the list
     * is empty and the message says why, but the ethernet probe -- the one
     * thing that does not need a catalogue -- is still reachable. */
    /* Reading the address wants the buffer a catalogue would occupy, so it
     * goes first; a complaint about the address file then waits until after
     * the catalogue's own, so neither buries the other. */
    if (!read_server_address()) {
        show_status(SchemeWarning, SERVER_FILE " IS NOT AN ADDRESS");
    }
    /* The card's copy, and nothing else.  Nothing reaches the network until
     * somebody presses F: a catalogue changes rarely, the card's copy loads in
     * no time where a fetch is seconds, and a machine with no cable in it
     * opens the browser rather than waiting out a lease that will not come.
     * With no copy the list is simply empty. */
    (void)load_catalog();
    show_catalog();
    browse();

    mega65_dos_exechelper("FREEZER.M65");
    return 0;
}
