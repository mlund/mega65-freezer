#pragma once

/* The seven tools' colours, named for what they mark rather than for a hue.
 *
 * Colour RAM holds a four-bit index, not a colour, and so do VICIV.bordercol
 * and VICIV.screencol.  Rewriting the sixteen palette entries therefore
 * repaints everything already on screen on the next frame, with nothing
 * redrawn -- which is what lets a scheme be switched while the tools run.
 *
 * That only holds while every scheme puts a role on the same palette entry, so
 * the role-to-entry mapping below is fixed and a scheme chooses only the
 * sixteen colours those entries hold.  Roles are enum constants, so a themed
 * call site costs exactly what the bare index it carries costs.
 *
 * Redefining the palette is safe because hyppo's freeze.asm saves and restores
 * all four palette banks and $D070, so the frozen program gets its own colours
 * back on resume.
 *
 * Two parts of the display show the frozen machine rather than our own menus,
 * and only one can be kept clear of a scheme.  The freezer's thumbnail stores
 * each pixel as three bits of red, three of green and two of blue, and reaches
 * the palette entries above the sixteen, so it is unaffected.  The sprite
 * editor's canvas cannot be: it draws palette indices taken from the frozen
 * VIC's own registers, against whatever palette is loaded, so it misreports a
 * sprite's colour under every scheme.  Only reading the frozen palette out of
 * the freeze slot fixes that.
 */

#include <stdint.h>

/* $D100-$D300 take each channel with its nybbles swapped, so a scheme is
 * written in ordinary RGB and converted here.  A macro because C23 has
 * constexpr objects but not constexpr functions, and an initialiser cannot call
 * one. */
#define SCHEME_SWAP(v) ((uint8_t)((((v) & 0x0f) << 4) | (((v) >> 4) & 0x0f)))
#define SCHEME_RGB(r, g, b) {SCHEME_SWAP(r), SCHEME_SWAP(g), SCHEME_SWAP(b)}

struct SchemeSlot {
    uint8_t red, green, blue;
};

/* The sixteen palette entries, each named for a role that lands on it and
 * listing the others that share it.  The array view is the same sixteen in the
 * same order, so set_palette() can walk them without knowing the names. */
struct ColourScheme {
    union {
        struct {
            struct SchemeSlot shade;       /*  0  BorderBusy, BorderDark                */
            struct SchemeSlot text;        /*  1  Text, Heading, Selected, Reject,      */
                                           /*     AsmPlain, Cursor                      */
            struct SchemeSlot error;       /*  2  Error, MeterHigh                      */
            struct SchemeSlot address;     /*  3  Address, Credits                      */
            struct SchemeSlot spare;       /*  4  no role; sprite data still reaches it */
            struct SchemeSlot operand;     /*  5  AsmOperand, MeterLow                  */
            struct SchemeSlot background;  /*  6  Background, Border                    */
            struct SchemeSlot value;       /*  7  Value, Notice, AsmControlFlow,        */
                                           /*     MeterMidDim, Pointer                  */
            struct SchemeSlot attention;   /*  8  Attention, MeterMid                   */
            struct SchemeSlot unknown;     /*  9  Unknown                               */
            struct SchemeSlot warning;     /* 10  Warning, MeterHighDim, Bar            */
            struct SchemeSlot unselected;  /* 11  Unselected                            */
            struct SchemeSlot text_dim;    /* 12  TextDim, AsmBytes                     */
            struct SchemeSlot highlight;   /* 13  Highlight, AsmMega65, MeterLowDim,    */
                                           /*     Banner                                */
            struct SchemeSlot accent;      /* 14  Accent                               */
            struct SchemeSlot text_bright; /* 15  TextBright                           */
        };
        struct SchemeSlot entry[16];
    };
};

enum Scheme : uint8_t {
    SchemeGruvbox = 0,
    SchemeClassic = 1,
    SchemeCount = 2,
};

/* Which scheme a tool starts in.  Not a build option: every scheme is compiled
 * in, and the monitor's F1 cycles them and hands the choice on, so this only
 * picks where that cycle begins.  No other tool binds a key to it. */
constexpr uint8_t SCHEME_BOOT = SchemeClassic;

/* A tool leaves its choice here for the next one it launches, because
 * mega65_dos_exechelper() loads that tool over this one and no variable
 * survives.  The magic byte is what distinguishes our two bytes from whatever
 * the frozen program left there; without it the boot scheme is used. */
/* Placed by src/memory.ld, which owns the whole map so that no
 * two tools can claim the same bytes.  volatile because the reader is a
 * different program: the write must reach memory before this one is replaced. */
extern volatile uint8_t scheme_handoff[2];
constexpr uint8_t SCHEME_HANDOFF_MAGIC = 0x5C;

// clang-format off
/* C-only: designated array initialisers are not C++, and the one C++ file in
 * the project needs the roles below, not the table. */
#ifndef __cplusplus
constexpr struct ColourScheme SCHEMES[SchemeCount] __attribute__((section(".rodata"))) = {
    [SchemeGruvbox] = {
        /* Gruvbox dark. */
        .shade       = SCHEME_RGB(0x1d, 0x20, 0x21),
        .text        = SCHEME_RGB(0xeb, 0xdb, 0xb2),
        .error       = SCHEME_RGB(0xfb, 0x49, 0x34),
        .address     = SCHEME_RGB(0x8e, 0xc0, 0x7c),
        .spare       = SCHEME_RGB(0xd3, 0x86, 0x9b),
        .operand     = SCHEME_RGB(0x98, 0x97, 0x1a),
        .background  = SCHEME_RGB(0x28, 0x28, 0x28),
        .value       = SCHEME_RGB(0xfa, 0xbd, 0x2f),
        .attention   = SCHEME_RGB(0xfe, 0x80, 0x19),
        .unknown     = SCHEME_RGB(0xd6, 0x5d, 0x0e),
        /* Pale, so the meter's high zone reads selected against unselected the
         * way its green and orange zones do. */
        .warning     = SCHEME_RGB(0xf3, 0x92, 0x73),
        .unselected  = SCHEME_RGB(0x66, 0x5c, 0x54),
        .text_dim    = SCHEME_RGB(0x92, 0x83, 0x74),
        .highlight   = SCHEME_RGB(0xb8, 0xbb, 0x26),
        .accent      = SCHEME_RGB(0x83, 0xa5, 0x98),
        .text_bright = SCHEME_RGB(0xfb, 0xf1, 0xc7),
    },
    [SchemeClassic] = {
        /* The C64's sixteen, in the C64's own order -- which is why `spare` is
         * purple and `background` is blue. */
        .shade       = SCHEME_RGB(0x00, 0x00, 0x00),
        .text        = SCHEME_RGB(0xff, 0xff, 0xff),
        .error       = SCHEME_RGB(0xab, 0x31, 0x26),
        .address     = SCHEME_RGB(0x66, 0xda, 0xff),
        .spare       = SCHEME_RGB(0xbb, 0x3f, 0xb8),
        .operand     = SCHEME_RGB(0x55, 0xce, 0x58),
        .background  = SCHEME_RGB(0x1d, 0x0e, 0x97),
        .value       = SCHEME_RGB(0xea, 0xf5, 0x7c),
        .attention   = SCHEME_RGB(0xb9, 0x74, 0x18),
        .unknown     = SCHEME_RGB(0x78, 0x73, 0x00),
        .warning     = SCHEME_RGB(0xdd, 0x93, 0x87),
        .unselected  = SCHEME_RGB(0x5b, 0x5b, 0x5b),
        .text_dim    = SCHEME_RGB(0x8b, 0x8b, 0x8b),
        .highlight   = SCHEME_RGB(0xb0, 0xf4, 0xac),
        .accent      = SCHEME_RGB(0xaa, 0x9d, 0xef),
        .text_bright = SCHEME_RGB(0xb8, 0xb8, 0xb8),
    },
};
#endif
// clang-format on

/* Which palette entry each role draws in.  Fixed across schemes: a scheme
 * recolours an entry rather than moving a role to another one, which is what
 * keeps a switch to a palette write with nothing redrawn. */
enum SchemeRole : uint8_t {
    SchemeBackground = 6,  /* the screen behind everything                */
    SchemeBorder = 6,      /* the border, at rest                         */
    SchemeBorderBusy = 0,  /* the border while the card is being read     */
    SchemeBorderDark = 0,  /* a border deliberately darker than the screen*/
    SchemeText = 1,        /* ordinary text and labels                    */
    SchemeHeading = 1,     /* a title, and the key legend                 */
    SchemeTextDim = 12,    /* text that is present but not to be read     */
    SchemeTextBright = 15, /* text picked out from its neighbours         */
    SchemeValue = 7,       /* a value read from the machine               */
    SchemeNotice = 7,      /* a remark about what just happened           */
    SchemeAccent = 14,     /* a name, a prompt for one, a flag that is set*/
    SchemeHighlight = 13,  /* something that has just succeeded           */
    SchemeSelected = 1,    /* the chosen row or label                     */
    SchemeUnselected = 11, /* an option available but not chosen          */
    SchemeError = 2,       /* the operation failed                        */
    SchemeWarning = 10,    /* it worked, but look at this                 */
    SchemeAttention = 8,   /* absent, inactive, or out of date            */
    SchemeUnknown = 9,     /* a state the tool cannot identify            */
    SchemeReject = 1,      /* the flash for a key that does nothing       */
    SchemeAddress = 3,     /* an address or a coordinate                  */

    /* Disassembly, from colour_disassembly_line(); the address column takes
     * SchemeAddress. */
    SchemeAsmBytes = 12,
    SchemeAsmOperand = 5,
    SchemeAsmPlain = 1,
    SchemeAsmControlFlow = 7,
    SchemeAsmMega65 = 13,

    /* The audio mixer's dB meter: three zones across the bar, each drawn bright
     * on the selected channel and dim on the others. */
    SchemeMeterLow = 5,
    SchemeMeterLowDim = 13,
    SchemeMeterMid = 8,
    SchemeMeterMidDim = 7,
    SchemeMeterHigh = 2,
    SchemeMeterHighDim = 10,

    /* The sprite editor's own furniture. */
    SchemeBanner = 13, /* the title bar                   */
    SchemeBar = 10,    /* a section heading or prompt bar */
    SchemeCredits = 3, /* the help screen's credit bar    */
    SchemePointer = 7, /* the mouse pointer sprite        */
    SchemeCursor = 1,  /* the edit cursor sprite          */
};

/* One step per pass of the sprite editor's main loop, so its cursor cycles.
 * Entry numbers rather than colours, so it follows whichever scheme is loaded. */
// clang-format off
static const uint8_t SCHEME_CURSOR_RAMP[16] __attribute__((section(".rodata"))) = {
    0, 6, 9, 11, 12, 15, 13, 1,
    1, 13, 15, 12, 11, 2, 9, 6,
};
// clang-format on
