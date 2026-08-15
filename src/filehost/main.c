/* The FileHost browser: a catalogue of downloadable titles, and the disk image
 * one of them attaches to the frozen machine's drive.
 *
 * The catalogue comes off the card at start-up and over the network on demand;
 * the images it names still have to be put on the card by something else.  Only
 * the fetch needs a wire, so everything else here is testable without one. */

#include "arp.h"
#include "browser.h"
#include "catalog.h"
#include "colours.h"
#include "common.h"
#include "dma.h"
#include "eth.h"
#include "fetch.h"
#include "format.h"
#include "ip.h"
#include "lineedit.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "shortname.h"
#include "slot.h"

#include <mega65.h>
#include <string.h>

/* The catalogue as it sits on the card.  A payload rather than a program, which
 * is the convention IOMAP.M65 and M65THUMB.M65 already follow. */
#define CATALOG_FILE "CATALOG.M65"
/* What the same catalogue is called on the server, which ether65's
 * docs/FILEHOST.md §1 fixes: the card needs an 8.3 name and the server does
 * not, so the two differ and neither is derived from the other. */
#define CATALOG_NAME "catalog"
/* Where to fetch from, when whoever runs the network does not say.  A file
 * rather than a screen to type on: the address is settled once per network and
 * a card is easier to edit than a machine with no keyboard driver of its own.
 * One line, an address in figures -- there is no resolver here to turn a name
 * into one. */
#define SERVER_FILE "TFTP-IP.TXT"
/* "255.255.255.255", its line ending, and a terminator. */
static constexpr uint8_t SERVER_TEXT_BYTES = 18;
/* Where the address starts before anything else says otherwise -- the machine
 * this was developed against.  A starting point for the editor rather than a
 * guess at the network: a lease, the card and the keyboard all override it. */
static constexpr uint8_t DEFAULT_SERVER[IPV4_BYTES] = {192, 168, 68, 57};

/* Read whole and indexed in place.  Chip RAM the tool has to itself: hyppo's
 * 384KB freeze region covers it, so the resume puts the frozen program's own
 * bytes back over whatever is left here.  The same scratch bank ROMLOAD stages
 * a ROM image in, and for the same reason.
 *
 * Not a linker section: link.ld's regions are the 16-bit space, and this is a
 * 28-bit DMA target.  ROM_CHARSET and CHARGEN_ADDRESS are constants for that
 * reason too. */
static constexpr Addr28 CATALOG_BUFFER = 0x40000;
static constexpr uint32_t CATALOG_BUFFER_BYTES = 0x20000;
/* One DMA fill's worth: lfill counts in 16 bits, and the buffer is wider. */
static constexpr uint16_t CLEAR_STEP = 0x8000;

/* Kilobytes, as a shift: the sizes run past what a 16-bit decimal holds, and
 * the browser is choosing between titles rather than auditing bytes. */
static constexpr uint8_t KILOBYTE_SHIFT = 10;

/* Where the frozen machine's internal 3.5" drive is.  One drive here: the
 * freeze menu's own chooser is what picks between the two. */
static constexpr uint8_t ATTACH_DRIVE = 0;

/* Hyppo's own code, from mega65-core src/hyppo/constants.asm.  The only one
 * this screen says anything of its own about -- see attach_selected(). */
static constexpr uint8_t DOS_FILE_NOT_FOUND = 0x88;

/* record_count is the header's, clamped in load_catalog() to what the buffer
 * holds, so it is the count the browser may index rather than the count the
 * file claims. */
static struct CatalogHeader header;
static uint16_t selected;
static uint16_t first_shown;

/* One record at a time in the 16-bit window; the rest stays in far memory. */
static uint8_t raw_record[CATALOG_HEADER_BYTES];
static struct CatalogRecord record;

/* Whether the freeze slot has been located.  Deferred until an attach actually
 * needs it: read_freeze_slot_start_sector() makes hyppo walk the system
 * partition, which is SD reads, and browsing without attaching is the common
 * case. */
static bool slot_located;

/* The TFTP server as this screen knows it, which is the default until the card
 * or the keyboard says otherwise.  A lease that names one beats all three, and
 * fetch.c is where that is decided. */
static uint8_t server_ip[IPV4_BYTES];

/* One whole row, which is also how a row is cleared. */
static void draw_line(uint8_t y, uint8_t colour, const char* text) {
    draw_field(SCREEN_CELL(0, y), colour, text, SCREEN_COLS);
}

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

/* Rounded up, so a file always reads as at least 1KB. */
static void draw_size(uint16_t cell, uint8_t colour, uint32_t bytes) {
    char text[WIDTH_SIZE + 1];
    char* at = append_dec(text, (uint16_t)((bytes + (1 << KILOBYTE_SHIFT) - 1) >> KILOBYTE_SHIFT));
    at = append_str(at, "KB");
    *at = 0;
    draw_field(cell, colour, text, WIDTH_SIZE);
}

static void draw_row(uint16_t index) {
    const uint8_t y = LIST_TOP_Y + (uint8_t)(index - first_shown);
    if (index >= header.record_count) {
        draw_line(y, SchemeText, "");
        return;
    }

    fetch_record(index);
    const uint8_t colour = (index == selected) ? (SchemeSelected | AttribReverse) : SchemeText;

    draw_field(SCREEN_CELL(COLUMN_TITLE, y), colour, record.title, WIDTH_TITLE);
    draw_field(SCREEN_CELL(COLUMN_AUTHOR, y), colour, record.author, WIDTH_AUTHOR);
    draw_field(
        SCREEN_CELL(COLUMN_KIND, y), colour, record.kind == CatalogD81 ? "D81" : "PRG", WIDTH_KIND);
    draw_size(SCREEN_CELL(COLUMN_SIZE, y), colour, record.size);
}

static void draw_list(void) {
    for (uint8_t row = 0; row < LIST_ROWS; row++) {
        draw_row(first_shown + row);
    }
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
    if (!header.record_count) {
        return;
    }
    int16_t at = (int16_t)selected + delta;
    if (at < 0) {
        at = 0;
    }
    if (at >= (int16_t)header.record_count) {
        at = (int16_t)header.record_count - 1;
    }
    move_to((uint16_t)at);
}

/* Attaching writes the frozen process descriptor as well as the live one: the
 * unfreeze path re-reads those fields and reattaches from them, so a mount that
 * only happened live would not survive the resume.  This is the pair
 * src/makedisk/main.c performs after creating an image. */
static void attach_selected(void) {
    char name[SHORT_NAME_BYTES];

    fetch_record(selected);
    if (record.kind != CatalogD81) {
        show_status(SchemeWarning, "ONLY DISK IMAGES CAN BE ATTACHED");
        return;
    }

    if (!slot_located) {
        request_freeze_region_list();
        /* Slot 0 is the live freeze: attaching to a stored slot would change a
         * machine nobody is about to resume. */
        freeze_slot_start_sector = read_freeze_slot_start_sector(0);
        slot_located = true;
    }

    catalog_short_name(record.path, record.kind, name);
    if (mega65_dos_attach(name, ATTACH_DRIVE)) {
        const uint8_t code = mega65_geterrorcode();
        /* Only this one is worth its own wording: to the freezer's disk chooser
         * a missing file is a missing file, but here it means the catalogue
         * named something the card has not been given yet. */
        show_status(SchemeError,
            code == DOS_FILE_NOT_FOUND ? "NOT ON THE CARD YET" : hyppoerror_to_screen(code));
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

/* Whether the ethernet controller works from the frozen state, which no source
 * answers and only hardware can.  An ARP request needs no address of our own
 * and no checksum, and its reply proves both directions at once: a machine that
 * hears one has transmitted and received while a freezer tool was running.
 *
 * The frame count beside it is the weaker but unconditional half -- a LAN
 * broadcasts constantly, so frames arriving proves the receiver alone even if
 * nothing answers us. */
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

static char* append_byte_hex(char* at, uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    *at++ = digits[value >> 4];
    *at++ = digits[value & 0x0F];
    return at;
}

static char* append_mac(char* at, const uint8_t* mac) {
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        if (i) {
            *at++ = ':';
        }
        at = append_byte_hex(at, mac[i]);
    }
    return at;
}

static void probe_line(uint8_t row, uint8_t colour, const char* text) {
    draw_line(LIST_TOP_Y + row, colour, text);
}

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
    for (uint8_t i = 0; i < IPV4_BYTES; i++) {
        if (i) {
            *at++ = '.';
        }
        at = append_dec(at, PROBE_TARGET[i]);
    }
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

    while (!ASCIIKEY) {
        ;
    }
    ASCIIKEY = 0;
    draw_list();
}

/* True when there is a catalogue to browse.
 *
 * The buffer is cleared first because the load reports success or failure and
 * not a length: a truncated catalogue -- which is what an interrupted download
 * leaves behind -- would otherwise leave the records past its end reading
 * whatever the last tool left in this bank, and ROMLOAD stages a 128KB ROM image
 * here.  Cleared, those rows decode to empty text and kind 0, which the browser
 * shows as blank and refuses to attach.  The load itself is still unbounded,
 * which only hyppo could fix; a fetch over the network is told the length by
 * TFTP's `tsize` before it writes a byte. */
/* The server address off the card, if the card names one.
 *
 * Read into the catalogue buffer because load_catalog() is about to clear it
 * anyway, and because read_file_from_sdcard() is unbounded: a file left there
 * by mistake lands in 128KB of scratch rather than over something. */
/* The address as text, for the editor and for the screen. */
static uint8_t write_server_text(char* text) {
    char* at = text;
    for (uint8_t i = 0; i < IPV4_BYTES; i++) {
        if (i) {
            *at++ = '.';
        }
        at = append_dec(at, server_ip[i]);
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
 * that can replace a file, where MAKEDISK's can only create one, so TFTP-IP.TXT
 * is how an address survives a reset until then.
 *
 * line_edit() owns the buffer while this owns the screen and the keyboard,
 * which is what lets a full field and a backspace on an empty one be tested on
 * the host rather than typed at an emulator. */
static void edit_server_address(void) {
    /* The reverse space, which is the caret sprited's editor uses too. */
    static constexpr uint8_t CARET = 224;

    char prompt[32 + SERVER_TEXT_BYTES];
    char* at = append_str(prompt, "NEW TFTP SERVER (NOW ");
    at += write_server_text(at);
    at = append_str(at, "): ");
    *at = 0;
    const uint8_t field = (uint8_t)(at - prompt);

    char text[SERVER_TEXT_BYTES];
    uint8_t length = 0;
    text[0] = 0;
    for (;;) {
        char shown[sizeof prompt + SERVER_TEXT_BYTES];
        char* end = append_str(shown, prompt);
        end = append_str(end, text);
        *end = 0;
        draw_line(STATUS_Y, SchemeHighlight, shown);
        const uint8_t caret = CARET;
        draw_fragment(SCREEN_CELL(field + length, STATUS_Y), SchemeHighlight, &caret, 1);

        uint8_t key;
        while (!(key = ASCIIKEY)) {
            ;
        }
        ASCIIKEY = 0;
        if (key == KEY_RUN_STOP || key == KEY_ESC) {
            show_status(SchemeText, "");
            return;
        }
        if (line_edit(text, sizeof text, &length, key)) {
            break;
        }
    }

    if (!length) {
        show_status(SchemeText, "");
    } else if (ip_parse(text, server_ip)) {
        fetch_set_server(server_ip);
        show_status(SchemeHighlight, "SERVER SET -- F FETCHES THE CATALOGUE");
    } else {
        show_status(SchemeError, "THAT IS NOT AN ADDRESS");
    }
}

static void read_server_address(void) {
    /* Cleared first: the loader says nothing about the length, so without this
     * the bytes after the file's own are whatever the frozen machine left at
     * this address -- and a digit among them lengthens the address read. */
    lfill(CATALOG_BUFFER, 0, SERVER_TEXT_BYTES);
    if (read_file_from_sdcard(SERVER_FILE, CATALOG_BUFFER)) {
        return;
    }
    char text[SERVER_TEXT_BYTES];
    lcopy(CATALOG_BUFFER, (Addr28)(uint16_t)text, sizeof text);
    text[sizeof text - 1] = 0;

    if (ip_parse(text, server_ip)) {
        fetch_set_server(server_ip);
    } else {
        show_status(SchemeWarning, SERVER_FILE " IS NOT AN ADDRESS");
    }
}

static void clear_buffer(void) {
    for (uint32_t at = 0; at < CATALOG_BUFFER_BYTES; at += CLEAR_STEP) {
        lfill(CATALOG_BUFFER + (Addr28)at, 0, CLEAR_STEP);
    }
}

/* Whether what is in the buffer is a catalogue this browser may index, given
 * that `bytes` of it are real.
 *
 * The count is the file's claim and the browser's bound: fetch_record() reads
 * at the offset it names, so a count past what arrived -- a truncated transfer,
 * or a header that simply says more than it brought -- reads whatever lies
 * beyond the buffer and offers it as a title to attach. */
static bool accept_catalog(uint32_t bytes) {
    if (bytes > CATALOG_BUFFER_BYTES) {
        bytes = CATALOG_BUFFER_BYTES;
    }
    lcopy(CATALOG_BUFFER, (Addr28)(uint16_t)raw_record, CATALOG_HEADER_BYTES);
    if (bytes < CATALOG_HEADER_BYTES || !catalog_header(raw_record, &header)) {
        header = (struct CatalogHeader){0};
        show_status(SchemeError, "THAT IS NOT A CATALOGUE");
        return false;
    }

    const uint16_t fits = hw_div16(bytes - CATALOG_HEADER_BYTES, header.record_bytes);
    if (header.record_count > fits) {
        header.record_count = fits;
        show_status(SchemeWarning, "CATALOGUE CUT SHORT, SHOWING WHAT ARRIVED");
    }
    if (!header.record_count) {
        show_status(SchemeWarning, "THE CATALOGUE IS EMPTY");
        return false;
    }
    return true;
}

static bool load_catalog(void) {
    clear_buffer();
    if (read_file_from_sdcard(CATALOG_FILE, CATALOG_BUFFER)) {
        show_status(SchemeError, "NO " CATALOG_FILE " ON THE CARD");
        return false;
    }
    /* The loader says nothing about the length, so the whole buffer is what
     * may have been filled. */
    return accept_catalog(CATALOG_BUFFER_BYTES);
}

static void draw_count(void) {
    char text[8];
    char* at = append_dec(text, header.record_count);
    *at = 0;
    draw_field(SCREEN_CELL(COUNT_X, COUNT_Y), SchemeTextDim, text, 5);
}

/* Back to the top of a list that has just been replaced.  Not move_to(), which
 * clears the status line: what a fetch has to say outlives its redraw. */
static void show_from_top(void) {
    selected = 0;
    first_shown = 0;
    draw_count();
    draw_list();
}

/* The catalogue over the wire, into the buffer the card copy would occupy.
 *
 * Nothing is written to the card: the browser reads the catalogue from this
 * buffer either way, so a fetch that fails costs the list on screen and not the
 * copy on the card.  What the buffer holds afterwards is whatever the fetch
 * left, so the card copy is read back rather than trusted. */
static void fetch_catalog(void) {
    show_status(SchemeText, "FETCHING THE CATALOGUE...");

    /* Cleared first, so a catalogue shorter than the last one does not leave
     * the tail of the old one readable behind it. */
    clear_buffer();
    uint32_t got = 0;
    const enum FetchResult result =
        fetch_file(CATALOG_NAME, CATALOG_BUFFER, CATALOG_BUFFER_BYTES, &got);
    if (result != FetchOk) {
        static const char* const why[] = {
            "",
            "NO ADDRESS: NOTHING ANSWERED ON THE NETWORK",
            "NO TFTP SERVER: PUT ONE IN TFTP-IP.TXT ON THE CARD",
            "THE TFTP SERVER DID NOT ANSWER",
            "THE SERVER REFUSED TO SEND IT",
            "THE TRANSFER STOPPED PART WAY",
            "THE CATALOGUE IS LARGER THAN THE BUFFER",
        };
        static_assert(sizeof why / sizeof *why == FetchTooBig + 1,
            "a fetch result with no message would index past this table");
        /* Whatever landed is half a catalogue, so the card's copy goes back
         * first -- and its own complaints are said before the one the user
         * pressed a key to hear. */
        (void)load_catalog();
        show_from_top();
        if (result == FetchRefused) {
            /* The code is the difference between a file that is not there and
             * a server that will not part with it. */
            char text[40];
            char* at = append_str(text, "THE SERVER REFUSED IT, CODE ");
            at = append_dec(at, fetch_error());
            *at = 0;
            show_status(SchemeError, text);
        } else {
            show_status(SchemeError, why[result]);
        }
        return;
    }

    const bool usable = accept_catalog(got);
    show_from_top();
    if (usable) {
        show_status(SchemeHighlight, "FETCHED");
    }
}

static void browse(void) {
    draw_list();

    for (;;) {
        uint8_t key;
        while (!(key = ASCIIKEY)) {
            ;
        }
        ASCIIKEY = 0;

        switch (key) {
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
                if (header.record_count) {
                    attach_selected();
                }
                break;
            case 'S':
            case 's':
                edit_server_address();
                break;
            case 'F':
            case 'f':
                fetch_catalog();
                break;
            case 'E':
            case 'e':
                ethernet_probe(true);
                break;
            case 'R':
            case 'r':
                ethernet_probe(false);
                break;
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
    memcpy(server_ip, DEFAULT_SERVER, IPV4_BYTES);
    fetch_set_server(server_ip);
    read_server_address();
    load_catalog();
    draw_count();
    browse();

    mega65_dos_exechelper("FREEZER.M65");
    return 0;
}
