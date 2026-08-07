#pragma once

/* The seven tools' colours, named for what they mark rather than for a hue.  A
 * scheme is sixteen palette entries, the handful of roles it places differently,
 * and its own cursor ramp; selected at compile time by COLOR_SCHEME in
 * src/CMakeLists.txt.
 *
 * Roles are enum constants, so a themed call site costs exactly what the bare
 * index it carries costs, and the table is read once by set_palette().  Two
 * roles may share an index without being the same role: what decides is whether
 * a scheme could give them different colours.
 *
 * Redefining the palette is safe because hyppo's freeze.asm saves and restores
 * all four palette banks and $D070, so the frozen program gets its own colours
 * back on resume.
 *
 * Two parts of the display depict the frozen machine rather than decorating our
 * interface, and only one of them can be kept out of a scheme's reach.  The
 * freezer's thumbnail carries RGB332 and routes through the colour cube, so it
 * is independent.  The sprite editor's canvas cannot be: it draws palette
 * indices taken from the frozen VIC's registers against whatever palette is
 * loaded, so it misreports a sprite's colour under every scheme, this one
 * included.  Only reading the frozen palette out of the freeze slot fixes that.
 */

#include <stdint.h>

/* $D100-$D300 take each channel with its nybbles swapped, so a scheme is
 * written in ordinary RGB and converted here.  A macro because C23 has
 * constexpr objects but not constexpr functions, and an initialiser cannot call
 * one. */
#define SCHEME_SWAP(v) ((uint8_t)((((v) & 0x0f) << 4) | (((v) >> 4) & 0x0f)))
/* One palette entry: three channels and a pad, so the stride is a shift. */
#define SCHEME_RGB(r, g, b) SCHEME_SWAP(r), SCHEME_SWAP(g), SCHEME_SWAP(b), 0x00

/* Bytes per SCHEME_PALETTE entry. */
constexpr uint8_t SCHEME_PALETTE_STRIDE = 4;

#if defined(COLOR_SCHEME_CLASSIC)

/* The C64's sixteen. */
// clang-format off
static const uint8_t SCHEME_PALETTE[16 * SCHEME_PALETTE_STRIDE] = {
    SCHEME_RGB(0x00, 0x00, 0x00), /*  0 black      */
    SCHEME_RGB(0xff, 0xff, 0xff), /*  1 white      */
    SCHEME_RGB(0xab, 0x31, 0x26), /*  2 red        */
    SCHEME_RGB(0x66, 0xda, 0xff), /*  3 cyan       */
    SCHEME_RGB(0xbb, 0x3f, 0xb8), /*  4 purple     */
    SCHEME_RGB(0x55, 0xce, 0x58), /*  5 green      */
    SCHEME_RGB(0x1d, 0x0e, 0x97), /*  6 blue       */
    SCHEME_RGB(0xea, 0xf5, 0x7c), /*  7 yellow     */
    SCHEME_RGB(0xb9, 0x74, 0x18), /*  8 orange     */
    SCHEME_RGB(0x78, 0x73, 0x00), /*  9 brown      */
    SCHEME_RGB(0xdd, 0x93, 0x87), /* 10 pink       */
    SCHEME_RGB(0x5b, 0x5b, 0x5b), /* 11 dark grey  */
    SCHEME_RGB(0x8b, 0x8b, 0x8b), /* 12 grey       */
    SCHEME_RGB(0xb0, 0xf4, 0xac), /* 13 lt. green  */
    SCHEME_RGB(0xaa, 0x9d, 0xef), /* 14 lt. blue   */
    SCHEME_RGB(0xb8, 0xb8, 0xb8), /* 15 lt. grey   */
};
// clang-format on

/* The five roles this scheme places for itself; see SchemeRole below. */
#define SCHEME_HEADING 1
#define SCHEME_WARNING 10
#define SCHEME_ATTENTION 8
#define SCHEME_UNKNOWN 9
#define SCHEME_BAR 10

/* The freezer's thumbnail is RGB332 and expands identity into the colour cube
 * at entries 16-255, so it needs nothing from a scheme -- except that pointing
 * the C64's own sixteen shades at exact entries is closer than the cube gets,
 * which holds only while entries 0-15 *are* those shades. */
constexpr bool SCHEME_IS_C64_PALETTE = true;

/* One step per pass of the sprite editor's main loop, so its cursor cycles.
 * Not quite a symmetric pulse: index 13 is red where the descent would
 * otherwise mirror the climb. */
// clang-format off
static const uint8_t SCHEME_CURSOR_RAMP[16] = {
    0, 6, 9, 11, 12, 15, 13, 1,
    1, 13, 15, 12, 11, 2, 9, 6,
};
// clang-format on

#elif defined(COLOR_SCHEME_GRUVBOX)

/* Gruvbox dark, in the slot order `classic` uses -- black, white, red, cyan,
 * purple, green, background, yellow, orange, dark orange, pale red, dark grey,
 * grey, light green, light blue, light grey -- so that an index no role names
 * still lands on a sensible hue. */
// clang-format off
static const uint8_t SCHEME_PALETTE[16 * SCHEME_PALETTE_STRIDE] = {
    SCHEME_RGB(0x1d, 0x20, 0x21), /*  0 bg0_hard       */
    SCHEME_RGB(0xeb, 0xdb, 0xb2), /*  1 fg1            */
    SCHEME_RGB(0xfb, 0x49, 0x34), /*  2 bright red     */
    SCHEME_RGB(0x8e, 0xc0, 0x7c), /*  3 bright aqua    */
    SCHEME_RGB(0xd3, 0x86, 0x9b), /*  4 bright purple  */
    SCHEME_RGB(0x98, 0x97, 0x1a), /*  5 neutral green  */
    SCHEME_RGB(0x28, 0x28, 0x28), /*  6 bg0            */
    SCHEME_RGB(0xfa, 0xbd, 0x2f), /*  7 bright yellow  */
    SCHEME_RGB(0xfe, 0x80, 0x19), /*  8 bright orange  */
    SCHEME_RGB(0xd6, 0x5d, 0x0e), /*  9 neutral orange */
    SCHEME_RGB(0xf3, 0x92, 0x73), /* 10 pale red       */
    SCHEME_RGB(0x66, 0x5c, 0x54), /* 11 bg3            */
    SCHEME_RGB(0x92, 0x83, 0x74), /* 12 gray           */
    SCHEME_RGB(0xb8, 0xbb, 0x26), /* 13 bright green   */
    SCHEME_RGB(0x83, 0xa5, 0x98), /* 14 bright blue    */
    SCHEME_RGB(0xfb, 0xf1, 0xc7), /* 15 fg0            */
};
// clang-format on

#define SCHEME_HEADING 3
/* Orange rather than a second red, so warning and error separate at a glance. */
#define SCHEME_WARNING 8
/* Adjacent to SchemeWarning's orange on purpose -- the two never appear side by
 * side, unlike a meter's highlight and lowlight. */
#define SCHEME_ATTENTION 9
#define SCHEME_UNKNOWN 4
#define SCHEME_BAR 4

constexpr bool SCHEME_IS_C64_PALETTE = false;

// clang-format off
static const uint8_t SCHEME_CURSOR_RAMP[16] = {
    0, 6, 11, 12, 9, 8, 7, 15,
    15, 7, 8, 9, 12, 11, 6, 0,
};
// clang-format on

#else
#error "No colour scheme selected -- see COLOR_SCHEME in src/CMakeLists.txt"
#endif

/* Every role, in one type so that any two can meet in one expression.  Most
 * carry a slot no scheme has wanted to move; the five spelled SCHEME_* come
 * from the block above, so a scheme that forgets one fails to compile. */
enum SchemeRole : uint8_t {
    SchemeBackground = 6,               /* the screen behind everything                */
    SchemeBorder = 6,                   /* the border, at rest                         */
    SchemeBorderBusy = 0,               /* the border while the card is being read     */
    SchemeBorderDark = 0,               /* a border deliberately darker than the screen*/
    SchemeText = 1,                     /* ordinary text and labels                    */
    SchemeHeading = SCHEME_HEADING,     /* a title, and the key legend        */
    SchemeTextDim = 12,                 /* text that is present but not to be read     */
    SchemeTextBright = 15,              /* text picked out from its neighbours         */
    SchemeValue = 7,                    /* a value read from the machine               */
    SchemeNotice = 7,                   /* a remark about what just happened           */
    SchemeAccent = 14,                  /* a name, a prompt for one, a flag that is set*/
    SchemeHighlight = 13,               /* something that has just succeeded           */
    SchemeSelected = 1,                 /* the chosen row or label                     */
    SchemeUnselected = 11,              /* an option available but not chosen          */
    SchemeError = 2,                    /* the operation failed                        */
    SchemeWarning = SCHEME_WARNING,     /* it worked, but look at this    */
    SchemeAttention = SCHEME_ATTENTION, /* absent, inactive, out of date  */
    SchemeUnknown = SCHEME_UNKNOWN,     /* a state the tool cannot name   */
    SchemeReject = 1,                   /* the flash for a key that does nothing       */
    SchemeAddress = 3,                  /* an address or a coordinate                  */

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
    SchemeBanner = 13,      /* the title bar                */
    SchemeBar = SCHEME_BAR, /* a section heading or prompt bar */
    SchemeCredits = 3,      /* the help screen's credit bar */
    SchemePointer = 7,      /* the mouse pointer sprite     */
    SchemeCursor = 1,       /* the edit cursor sprite       */
};
