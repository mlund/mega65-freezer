#include "colours.h"
#include "common.h"
#include "dma.h"
#include "layout.h"
#include "mega65_regs.h"
#include "screen.h"
#include "sdcard.h"
#include "slot.h"

#include <mega65.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Global Variables
 */
static constexpr uint8_t BUFFER_LENGTH = 254;
/* Text. */
static char buffer[BUFFER_LENGTH + 2];
static char tempstr32[32];
/* Bytes. */
static uint8_t is_ntsc = 0;
static uint8_t has_rtc = 0;
static uint8_t m65model;
static uint8_t m65submodel;
static uint8_t code_buffer[SD_SECTOR_SIZE];
static uint8_t ymd[3];

/* Selects the inverse glyph -- bit 7 of the screen code -- not the colour-RAM
 * AttribReverse.  Kept above the colour byte so a role can be OR-ed with it. */
static constexpr uint16_t WRITE_TEXT_INVERSE_GLYPH = 0x100;

static void write_text_mapped(
    uint8_t x, uint8_t y, uint16_t colour, const char* text, uint8_t mask, uint8_t lower_offset) {
    uint8_t i;
    uint8_t c;
    for (i = 0; text[i]; i++) {
        c = text[i] & mask;
        if ((c >= 'A') && (c <= 'Z')) {
            c -= 0x40;
        } else if ((c >= 'a') && (c <= 'z')) {
            c -= lower_offset;
        } else if (c == '~') { // ~ become pi
            c = 94;
        }
        if (colour & WRITE_TEXT_INVERSE_GLYPH && c < 128) {
            c |= 0x80;
        }
        SCREEN[y * SCREEN_ROW_BYTES + x + i] = c;
        lpoke(COLOUR_RAM_ADDRESS + y * SCREEN_ROW_BYTES + x + i, (uint8_t)(colour & 0xff));
    }
}

void write_text(uint8_t x, uint8_t y, uint16_t colour, char* text) {
    write_text_mapped(x, y, colour, text, 0xff, 0x20);
}

/* As write_text(), but folds lower case to upper. */
void write_text_upper(uint8_t x, uint8_t y, uint16_t colour, char* text) {
    write_text_mapped(x, y, colour, text, 0x7f, 0x60);
}

/* Populates code_buffer, m65model and m65submodel from $FFD3628.  One lcopy
 * rather than 32 lpeeks: each lpeek is a DMA job on this target. */
void copy_hw_version(void) {
    lcopy(0xFFD3628L, (long)code_buffer, 33);
    m65model = code_buffer[1];
    m65submodel = (code_buffer[0] >> 4) & 0xf;
    /* R2 onwards carry an RTC.  The flag is set here rather than in
     * format_mega_model(), because the first RTC probe happens before
     * anything is drawn. */
    has_rtc = (m65model >= 0x02 && m65model <= 0x06);
}

/* copy_hw_version() must have filled m65model and m65submodel first. */
char* format_mega_model(void) {
    switch (m65model) {
        case 0x01:
            return "MEGA65 R1";
        case 0x02:
            return "MEGA65 R2";
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
            // format new boards with model/submodel scheme
            *append_str(tempstr32, "MEGA65 R3 ") = '\0';
            tempstr32[8] = 0x30 + m65model;
            tempstr32[9] = m65submodel ? 0x40 + m65submodel : 32;
            break;
        case 0x21:
            return "MEGAPHONE R1 PROTOTYPE";
        case 0x22:
            return "MEGAPHONE R4 PROTOTYPE";
        case 0x40:
            return "NEXYS 4 PSRAM";
        case 0x41:
            return "NEXYS 4 DDR (NO WIDGET)";
        case 0x42:
            return "NEXYS 4 DDR (WIDGET)";
        case 0x60:
            return "QMTECH A100T";
        case 0x61:
            return "QMTECH A200T";
        case 0x62:
            return "QMTECH A325T";
        case 0xfd:
            return "QMTECH WUKONG BOARD";
        case 0xfe:
            return "SIMULATED MEGA65";
        default: {
            char* at = append_str(tempstr32, "UNKNOWN MODEL $");
            at = append_hex(at, m65model, 2);
            *at++ = '.';
            at = append_hex(at, m65submodel, 1);
            *at = '\0';
        } break;
    }
    return tempstr32;
}

/* A FPGA datestamp is days since 2020-01-01; `offset` is where the three
 * fields sit in code_buffer (2 KEYBD, 8 ARTIX, 14 MAX10) and `msbmask` masks
 * the MSB byte, which MAX10 needs at 0x3f.  Leaves the parsed year (without
 * century), month and day in ymd[] for the caller to compare against.
 * copy_hw_version() must have filled code_buffer first. */
char* format_datestamp(uint8_t offset, uint8_t msbmask) {
    /* The RTC counts years from 2000, not 1900. */
    constexpr uint16_t RTC_YEAR_EPOCH = 2000;
    uint8_t m = 1;
    uint16_t y = 2020;
    uint16_t ds;

    ds = (((uint16_t)(code_buffer[offset + 1] & msbmask)) << 8) + (uint16_t)code_buffer[offset];

    /* Years first, counted as 366 days each: the RTC's day count is what is
     * being unpacked, not a calendar date. */
    while (ds > 366) {
        y++;
        ds -= 366;
    }

    /* Then the month and day.  Every fourth year is a leap year here; the
     * century rules cannot be reached in the range an RTC covers.  December
     * has no entry: whatever is left when November is spent falls in it. */
    constexpr uint8_t DECEMBER = 12;
    static const uint8_t month_length[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30};
    for (m = 1; m < DECEMBER; m++) {
        const uint8_t length = (m == 2 && !(y & 3)) ? 29 : month_length[m - 1];
        if (ds <= length) {
            break;
        }
        ds -= length;
    }

    {
        char* at = append_dec(buffer, y);
        *at++ = '-';
        at = append_dec2(at, m);
        *at++ = '-';
        at = append_dec2(at, (uint8_t)ds);
        *at = '\0';
    }

    // save date for external use
    ymd[0] = (uint8_t)(y - RTC_YEAR_EPOCH);
    ymd[1] = m;
    ymd[2] = (uint8_t)ds;

    return buffer;
}

/* The FPGA commit hash as 8 hex digits; `offset` locates the field in
 * code_buffer as in format_datestamp(), `reverse` reads it byte-reversed.
 * copy_hw_version() must have filled code_buffer first. */
char* format_fpga_hash(uint8_t offset, uint8_t reverse) {
    char* at = buffer;
    for (uint8_t i = 0; i < 4; i++) {
        const uint8_t from = reverse ? (uint8_t)(offset + 2 + i) : (uint8_t)(offset + 5 - i);
        at = append_hex(at, code_buffer[from], 2);
    }
    *at = '\0';

    return buffer;
}

/* The ROM version, or a static string for a ROM that cannot be identified. */
char* format_rom_version(void) {
    // we want to display the version in freeze slot 0!
    freeze_slot_start_sector = read_freeze_slot_start_sector(0);
    request_freeze_region_list();

    return detect_rom();
}

/* Hyppo and HDOS version as "hyppo.major.minor / hdos.major.minor". */
/* The 45E100's own address, read straight out of $D6E9 without disturbing the
 * controller: an information screen has no business resetting it.  Worth
 * showing because nothing else on the machine says what it is, and a machine
 * being loaded over ethernet is identified by it. */
char* format_ethernet_mac(void) {
    char* at = buffer;
    constexpr uint8_t MAC_BYTES = sizeof ETHERNET.macaddr;
    for (uint8_t i = 0; i < MAC_BYTES; i++) {
        if (i) {
            *at++ = ':';
        }
        at = append_hex(at, ETHERNET.macaddr[i], 2);
    }
    *at = '\0';
    return buffer;
}

char* format_hyppo_version(void) {
    struct hyppo_version v = {0xff, 0xff, 0xff, 0xff};

    // hypervisor call, external
    gethyppoversion(&v);

    if (v.hyppo_major == 0xff && v.hyppo_minor == 0xff && v.hdos_major == 0xff &&
        v.hdos_minor == 0xff) {
        strcpy(buffer, "?.? / ?.?");
    } else {
        char* at = append_dec(buffer, v.hyppo_major);
        *at++ = '.';
        at = append_dec(at, v.hyppo_minor);
        at = append_str(at, " / ");
        at = append_dec(at, v.hdos_major);
        *at++ = '.';
        at = append_dec(at, v.hdos_minor);
        *at = '\0';
    }

    return buffer;
}

/* Scans `len` bytes for `needle`, resuming the partial match left in *cmp_idx
 * so a needle may straddle two sectors.  Returns the offset just past the
 * match, or `len` if it did not complete. */
static uint16_t scan_for(
    const uint8_t* hay, uint16_t len, const char* needle, uint8_t needle_len, uint8_t* cmp_idx) {
    uint16_t i = 0;
    while (i < len && *cmp_idx < needle_len) {
        *cmp_idx = (needle[*cmp_idx] == (char)hay[i]) ? (uint8_t)(*cmp_idx + 1)
                                                      : (uint8_t)(needle[0] == (char)hay[i]);
        i++;
    }
    return i;
}

/* "YYMMDD" in ASCII against a ymd triple: 0 if the text is that date or newer,
 * 1 if it is older or is not six digits. */
static uint8_t date_is_older(const char* text, const uint8_t* date) {
    for (uint8_t p = 0; p < 3; p++) {
        const char hi = text[p * 2];
        const char lo = text[p * 2 + 1];
        if (hi < '0' || hi > '9' || lo < '0' || lo > '9') {
            return 1;
        }
        const uint8_t temp = (uint8_t)((hi - '0') * 10 + lo - '0');
        if (temp > date[p]) {
            return 0;
        }
        if (temp < date[p]) {
            return 1;
        }
    }
    return 0;
}

/* Scans one sector from `addr` for a "v:20" version string and compares it to
 * `date` (an ARTIX ymd triple).  0 if the found version is that date or newer,
 * 1 if older or the string is not there. */
uint8_t format_util_version(long addr, const uint8_t* date) {
    uint8_t result = 0;

    lcopy(addr, (long)code_buffer, SD_SECTOR_SIZE);

    *append_str(buffer, "UNKNOWN VERSION") = '\0';
    uint8_t cmp_idx = 0;
    uint16_t i = scan_for(code_buffer, SD_SECTOR_SIZE, "V:20", 4, &cmp_idx);
    if (cmp_idx == 4) {
        uint16_t j = 0;
        while (j < 64 && i < SD_SECTOR_SIZE && code_buffer[i]) {
            buffer[j++] = code_buffer[i++];
        }
        buffer[j] = 0;
    }

    result = date_is_older(buffer, date);

    // cuts on the left side, we always want the rightmost part with the commit hash
    uint16_t len = strlen(buffer);
    if (len > 25) {
        len -= 25;
        for (uint16_t j = 0; j < 25; j++) {
            buffer[j] = buffer[len + j];
        }
        buffer[25] = 0;
    }

    return result;
}

/* As format_util_version(), but for a "GIT: " tag rather than "v:20", scanning
 * upward from `addr`. */
uint8_t format_hickup_version(long addr, const uint8_t* date) {
    static const char needle[] = "GIT: ";
    constexpr uint8_t NEEDLE_LEN = sizeof needle - 1;
    /* What fits the screen column the hash is shown in. */
    constexpr uint8_t VERSION_LENGTH = 38;
    uint8_t j = 0;
    uint8_t cmp_idx = 0;
    uint8_t finished = 0;

    for (uint16_t p = 0; p < 64 && !finished; p++) {
        lcopy(addr + (long)SD_SECTOR_SIZE * p, (long)code_buffer, SD_SECTOR_SIZE);
        uint16_t i = scan_for(code_buffer, SD_SECTOR_SIZE, needle, NEEDLE_LEN, &cmp_idx);
        while (i < SD_SECTOR_SIZE) {
            buffer[j] = code_buffer[i];
            /* The result is cut to VERSION_LENGTH below, so stopping there
             * loses nothing -- and an unbounded copy runs off buffer[]. */
            if (code_buffer[i] == 0 || j == VERSION_LENGTH) {
                finished = 1;
                break;
            }
            i++;
            j++;
        }
    }

    if (!finished) {
        strcpy(buffer, "VERSION NOT FOUND");
        return 1;
    }

    buffer[VERSION_LENGTH] = 0;
    const uint16_t len = strlen(buffer);

    // try to parse date
    constexpr uint8_t NEEDLE2_LEN = 3;
    cmp_idx = 0;
    const uint16_t k = scan_for((const uint8_t*)buffer, len, ",20", NEEDLE2_LEN, &cmp_idx);
    if (cmp_idx < NEEDLE2_LEN) {
        return 2;
    }

    return date_is_older(buffer + k, date);
}

/*
 * RTC Globals
 */
static uint8_t clock_init = 1, tod_buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static uint8_t rtc_state = 0, rtc_last_state = 0, rtc_settle = 0, no_extrtc = 0;
static uint8_t rtc_check = 1, rtc_diff = 0, rtc_buf[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static uint8_t rtc_pmu = 0xff;
static uint16_t tod_ov = 0, rtc_ov = 0;
static short tod_last = -1, tod_ticks = 0, rtc_ticks = 0;

/* Copies the TOD and RTC registers.  `reinit` takes the first of two copies
 * needed to detect a tick; the next call with reinit clear takes the second
 * and compares them, updating every clock display if TOD moved.  Returns
 * whether it did. */
uint8_t get_rtc_stats(uint8_t reinit) {
    // fetch external RTC state
    if (no_extrtc || lpeek(0xffd7400) == 0xff) { // external not installed
        if (has_rtc) {
            rtc_state = 1;
        } else {
            rtc_state = 0;
        }
    } else {
        if (lpeek(0xffd74fd) & 0x80) {
            rtc_state = 3; // external installed & active
        } else {
            rtc_state = 2; // external installed but inactive
        }
    }

    // clock changed, reinit
    if (rtc_state != rtc_last_state) {
        clock_init = 1;
        rtc_last_state = rtc_state;
        // if clock is active, give it a few seconds to settle
        if (rtc_state & 1) {
            rtc_settle = 3;
        }
    }

    if (clock_init || reinit) {
        lcopy(RTC_SECONDS, (long)rtc_buf, 6);
        lcopy(0xffd3c08l, (long)tod_buf, 4);
        if (m65model >= 0x04 && m65model <= 0x06) {
            rtc_pmu = lpeek(0xffd71d0UL);
            if (rtc_pmu != 0x22) {
                // disable eeprom refresh
                lpoke(0xffd7120UL, 0x04);
                usleep(20000L); // need to wait for slow RTC getting updated
                // set backup switchover mode to LSM, TCM 3V (Battery protection)
                lpoke(0xffd71d0UL, 0x22);
                usleep(20000L);
                // EECMD Update EEPROM
                lpoke(0xffd714fUL, 0x11);
                usleep(20000L);
                // enable eeprom refresh
                lpoke(0xffd7120UL, 0x00);
                usleep(20000L);
                rtc_pmu = lpeek(0xffd71d0UL);
            }
        } else {
            rtc_pmu = 0xff;
        }
        clock_init = 0;
        tod_ov = 0;
        rtc_ov = 0;
        rtc_check = 1;
        rtc_diff = 0;
    }
    lcopy(RTC_SECONDS, (Addr28)(rtc_buf + 6), 6);
    lcopy(0xffd3c08l, (Addr28)(tod_buf + 4), 4);

    // only looking at seconds here, derived from minutes + seconds
    const short pa = unbcd(tod_buf[1] & 0x7f) + unbcd(tod_buf[2] & 0x7f) * 60;
    const short pb = unbcd(tod_buf[5] & 0x7f) + unbcd(tod_buf[6] & 0x7f) * 60;
    tod_ticks = pb - pa;
    if (tod_ticks < 0) {
        tod_ticks += 3600; // we can work with one hour overflow
        tod_ov++;
        if (tod_ov > 1) { // after that we reinitialise (who lets this run for hours?)
            clock_init = 1;
            tod_ticks = 0;
        }
    }

    if (tod_ticks == tod_last) {
        return 0;
    }
    // tod_ticks changed, update rtc

    // external rtc needs a moment to settle
    if (rtc_settle > 0) {
        rtc_settle--;
        clock_init = 1;
        return 0;
    }

    tod_last = tod_ticks;

    // handle RTC
    if (rtc_state & 1) {
        const short pa = unbcd(rtc_buf[0] & 0x7f) + unbcd(rtc_buf[1] & 0x7f) * 60;
        const short pb = unbcd(rtc_buf[6] & 0x7f) + unbcd(rtc_buf[7] & 0x7f) * 60;
        rtc_ticks = pb - pa;
        if (rtc_ticks < 0) {
            rtc_ticks += 3600; // we can work with one hour overflow
            rtc_ov++;
            if (rtc_ov > 1) {
                clock_init = 1;
                return 0;
            }
        }

        if (rtc_ticks > tod_ticks) {
            rtc_diff = (uint8_t)(rtc_ticks - tod_ticks);
        } else {
            rtc_diff = (uint8_t)(tod_ticks - rtc_ticks);
        }
    }

    return 1;
}

/* Writes get_rtc_stats()'s return at (x, y).  All five strings are padded to
 * one width so a shorter one erases whatever stood there before. */
static void write_extrtc_status(uint8_t x, uint8_t y, uint8_t status) {
    switch (status) {
        case 0:
            write_text(x, y, SchemeAttention, "NO RTC AVAILABLE  ");
            break;
        case 1:
            write_text(x, y, SchemeValue, "INTERNAL          ");
            break;
        case 2:
            write_text(x, y, SchemeAttention, "EXTERNAL, INACTIVE");
            break;
        case 3:
            write_text(x, y, SchemeValue, "EXTERNAL, ACTIVE  ");
            break;
        default:
            write_text(x, y, SchemeUnknown, "UNKNOWN           ");
            break;
    }
}

/* Pads what has been built in buffer[] out to `width` so a shorter line erases
 * whatever stood there before, then draws it. */
static void write_padded(uint8_t x, uint8_t y, uint8_t colour, char* end, uint8_t width) {
    while (end < buffer + width) {
        *end++ = ' ';
    }
    *end = '\0';
    write_text(x, y, colour, buffer);
}

/* Prints the internal RTC's status at (x, y); a second line beneath it
 * appears once enough ticks have been seen to judge whether it is running. */
void display_rtc_status(uint8_t x, uint8_t y) {
    /* How long the tick probe runs, and what its counters have to show. */
    constexpr uint8_t PROBE_SECONDS = 20;
    constexpr uint8_t MIN_TICKS = 2;
    constexpr uint8_t MAX_DRIFT = 1;
    /* $D71D0 bits 4 and 5 name the backup source; 0x20 is the battery. */
    constexpr uint8_t PMU_BACKUP_MASK = 0x30;
    constexpr uint8_t PMU_BACKUP_ON = 0x20;
    constexpr uint8_t PMU_UNKNOWN = 0xff;
    constexpr uint8_t VERDICT_WIDTH = 24;
    constexpr uint8_t CLOCK_WIDTH = 20;

    write_extrtc_status(x, y, rtc_state);

    if (!(rtc_state & 1)) { // 1 and 3 are the active states
        return;
    }

    if (rtc_check && tod_ticks <= PROBE_SECONDS) {
        write_padded(x, y + 1, SchemeValue, append_str(buffer, "CHECKING"), VERDICT_WIDTH);
        write_padded(x, y + 2, SchemeTextDim, buffer, CLOCK_WIDTH);
        return;
    }

    if (rtc_check) {
        rtc_check = 0;

        uint8_t colour = SchemeWarning;
        /* The CIA verdict already fills the line; the rest have room for the
         * backup source after them. */
        bool room_for_backup = true;
        char* at;
        if (rtc_ticks <= MIN_TICKS) {
            at = append_str(buffer, "NOT TICKING");
        } else if (rtc_diff <= MAX_DRIFT) {
            at = append_str(buffer, "TICKING");
            colour = SchemeValue;
        } else if (no_extrtc && is_ntsc) {
            at = append_str(buffer, "SLOW TICK, SLOW CIA TOD!");
            room_for_backup = false;
        } else {
            at = append_str(buffer, "SLOW TICK");
        }

        if (room_for_backup && rtc_pmu != PMU_UNKNOWN) {
            at = append_str(
                at, (rtc_pmu & PMU_BACKUP_MASK) == PMU_BACKUP_ON ? ", BACKUP ON" : ", BACKUP OFF");
        }
        write_padded(x, y + 1, colour, at, VERDICT_WIDTH);
    }

    /* The clock itself, straight out of the RTC's BCD registers. */
    char* at = append_str(buffer, "20");
    at = append_hex(at, rtc_buf[11], 2);
    *at++ = '-';
    at = append_hex(at, rtc_buf[10] & 0x1f, 2);
    *at++ = '-';
    at = append_hex(at, rtc_buf[9] & 0x3f, 2);
    *at++ = ' ';
    at = append_hex(at, rtc_buf[8] & 0x3f, 2);
    *at++ = ':';
    at = append_hex(at, rtc_buf[7] & 0x7f, 2);
    *at++ = ':';
    at = append_hex(at, rtc_buf[6], 2);
    *at = '\0';
    write_text(x, y + 2, SchemeTextDim, buffer);
}

void display_rtc_debug(uint8_t x, uint8_t y, uint8_t colour, uint8_t mode) {
    // DEBUG output in the bottom line
    switch (mode) {
        case 1: {
            char* at = append_str(buffer, " RTC ");
            at = append_hex(at, rtc_buf[7] & 0x7f, 2);
            *at++ = ':';
            at = append_hex(at, rtc_buf[6], 2);
            at = append_str(at, " ");
            at = append_hex(at, rtc_ticks, 4);
            at = append_str(at, " TOD ");
            at = append_hex(at, tod_buf[6] & 0x7f, 2);
            *at++ = ':';
            at = append_hex(at, tod_buf[5], 2);
            at = append_str(at, " ");
            at = append_hex(at, tod_ticks, 4);
            at = append_str(at, " DIFF ");
            at = append_hex(at, rtc_diff, 4);
            at = append_str(at, " PMU ");
            at = append_hex(at, rtc_pmu, 2);
            *at = '\0';
        }
            if (rtc_state == 1) {
                buffer[0] = 'I';
            } else if (rtc_state == 2 || rtc_state == 3) {
                buffer[0] = 'E';
            }
            break;
        default:
            strcpy(buffer, "                                                     ");
    }
    write_text(x, y, colour, buffer);
    if (mode > 0) {
        if (is_ntsc) {
            write_text((uint8_t)(strlen(buffer) + 1), y, colour, "NTSC");
        } else {
            write_text((uint8_t)(strlen(buffer) + 1), y, colour, "PAL ");
        }
    }
}

/*
 * draw_screen
 *
 * update the whole screen (except RTC tick stuff)
 */
void draw_screen(void) {
    /* Where every version probe below loads the file it inspects. */
    constexpr uint32_t FILE_SCRATCH = 0x40000;
    uint8_t artix_ymd[3];

    // clear screen
    lfill(SCREEN_ADDRESS, 0x20, SCREEN_BYTES);

    // write header
    write_text(0, 0, SchemeHeading, "MEGA65 INFORMATION");
    write_text(54, 0, SchemeTextDim, "(C) 2022 MEGA - MUSEUM OF");
    write_text(54, 1, SchemeTextDim, "   ELECTRONIC GAMES & ART");
    write_text(0, 1, SchemeHeading, "cccccccccccccccccc");
    write_text(62, 24, SchemeHeading, "F3-EXIT F5-RESTART");

    // get Hardware information
    copy_hw_version();

    // write model
    write_text(0, 3, SchemeText, "MEGA65 MODEL:");
    write_text(15, 3, SchemeValue, format_mega_model());
    write_text(40, 3, SchemeText, "SCREEN MODE:");
    if (is_ntsc) {
        write_text(54, 3, SchemeValue, "NTSC");
    } else {
        write_text(54, 3, SchemeValue, "PAL");
    }

    // output fpga versions
    write_text(0, 5, SchemeText, "ARTIX VERSION:");
    write_text(15, 5, SchemeValue, format_fpga_hash(8, 0));
    write_text(25, 5, SchemeValue, format_datestamp(8, 0xff));
    // save artix date for hickup date check
    artix_ymd[0] = ymd[0];
    artix_ymd[1] = ymd[1];
    artix_ymd[2] = ymd[2];
    if (ymd[0] < 22 || (ymd[0] == 22 && (ymd[1] < 6 || (ymd[1] == 6 && ymd[2] < 23)))) {
        no_extrtc = 1;
        write_text(19,
            1,
            WRITE_TEXT_INVERSE_GLYPH | AttribBlink | SchemeAttention,
            "UPDATE CORE FOR EXTERNAL RTC SUPPORT!");
    }

    write_text(0, 6, SchemeText, "KEYBD VERSION:");
    write_text(15, 6, SchemeValue, format_fpga_hash(2, 0));
    write_text(25, 6, SchemeValue, format_datestamp(2, 0xff));

    if (m65model > 0x00 && m65model < 0x04) {
        // only mega65r1-3 have a MAX10 FPGA, it was removed for mega65r4
        write_text(0, 7, SchemeText, "MAX10 VERSION:");
        write_text(15, 7, SchemeValue, format_fpga_hash(14, 1));
        write_text(25, 7, SchemeValue, format_datestamp(14, 0x3f));
    }

    // The machine's name on the wire, beside the other things that identify it.
    // Not below: display_rtc_status() writes three rows from the one it is
    // given -- state, ticking and the time at y + 2 -- so this side of rows 5
    // to 7 is the RTC's.
    write_text(40, 4, SchemeText, "ETHERNET MAC:");
    write_text(54, 4, SchemeValue, format_ethernet_mac());

    // RTC (labels only, rest is done in mainloop)
    write_text(40, 5, SchemeText, "RTC STATUS:");

    // HYPPO/HDOS Version
    write_text(0, 9, SchemeText, "HYPPO/HDOS:");
    write_text(15, 9, SchemeValue, format_hyppo_version());

    // check for HICKUP
    write_text(40, 9, SchemeText, "HYPPO STATUS:");
    if (read_file_from_sdcard("HICKUP.M65", FILE_SCRATCH)) {
        write_text(54, 9, SchemeValue, "NORMAL");
    } else {
        const uint8_t fail = format_hickup_version(FILE_SCRATCH, artix_ymd);
        write_text_upper(41, 10, fail ? SchemeWarning : SchemeValue, buffer);
        if (fail) {
            write_text(54, 9, SchemeWarning, "OUT OF DATE HICKUP.M65");
        } else {
            write_text(54, 9, SchemeWarning, "LOCKED BY HICKUP.M65");
        }
    }

    // ROM version
    write_text(0, 10, SchemeText, "ROM VERSION:");
    write_text(15, 10, SchemeValue, format_rom_version());

    // Utility versions (need to load file to parse...)
    /* The tools whose version this screen reports; an empty name ends it. */
    static const char essentials[][13] = {"FREEZER.M65",
        "ETHLOAD.M65",
        "MEGAINFO.M65",
        "MONITOR.M65",
        "MAKEDISK.M65",
        "ROMLOAD.M65",
        "AUDIOMIX.M65",
        "SPRITED.M65",
        ""};
    uint8_t row = 12;
    uint8_t col = 0;
    for (uint8_t i = 0; essentials[i][0] != 0; i++) {
        uint8_t fail = read_file_from_sdcard(essentials[i], FILE_SCRATCH);
        char* at = append_str(buffer, essentials[i]);
        *at++ = ':';
        *at = '\0';
        write_text(col, row, SchemeText, buffer);
        if (fail) {
            write_text(col + 14, row, SchemeWarning, "FILE NOT FOUND");
        } else {
            fail = format_util_version(FILE_SCRATCH, artix_ymd);
            write_text_upper(col + 14, row, fail ? SchemeWarning : SchemeValue, buffer);
        }
        if (!col) {
            col = 40;
        } else {
            col = 0;
            row++;
        }
    }
}

/*
 * init_megainfo
 *
 * initialize basic structures and screen
 */
void init_megainfo(void) {
    is_ntsc = (lpeek(0xFFD306fL) & 0x80) == 0x80;
    // fix TOD frequency
    if (is_ntsc) {
        lpoke(0xffd3c0el, lpeek(0xffd3c0el) & 0x7f);
    } else { // is PAL, set 50Hz bit
        lpoke(0xffd3c0el, lpeek(0xffd3c0el) | 0x80);
    }

    copy_hw_version(); // has_rtc, which the probe below reads
    get_rtc_stats(1);  // initialise rtc data cache
    draw_screen();
}

/*
 * do_megainfo
 */
void do_megainfo(void) {
    uint8_t rtc_debug = 0;

    init_megainfo();

    // clear keybuffer
    for (uint8_t key; (key = ASCIIKEY);) {
        ASCIIKEY = key;
    }

    // mainloop
    while (1) {
        const uint8_t x = ASCIIKEY;

        // update clocks
        if (get_rtc_stats(0)) {
            display_rtc_status(54, 5);
            display_rtc_debug(0, 24, 12, rtc_debug);
        }

        if (x == 0) {
            continue;
        }
        ASCIIKEY = x;

        switch (x) {
            case KEY_F1: // F1 - Toggle DEBUG
                rtc_debug = 1 - rtc_debug;
                display_rtc_debug(0, 24, 12, rtc_debug);
                break;
            case KEY_F5: // F5 - REFRESH
                init_megainfo();
                break;
            case KEY_F3:       // F3
            case KEY_ESC:      // ESC
            case KEY_RUN_STOP: // RUN-STOP
                return;        // EXIT!
            default:
                break;
        }
    }
}
