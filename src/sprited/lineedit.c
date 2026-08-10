#include "lineedit.h"

/* The keys this answers to.  RETURN and DELETE are the ASCII the keyboard
 * queue reports, matching KEY_RETURN and KEY_DELETE in mega65_regs.h; the
 * header is not included here so the host build needs nothing from the
 * target's register map. */
constexpr uint8_t LINE_RETURN = 0x0D;
constexpr uint8_t LINE_DELETE = 0x14;

/* Printable ASCII only.  mega65-libc's cinput() with CINPUT_ACCEPT_ALL takes
 * anything that is not RETURN or DELETE, which lets a control code into a
 * filename; the two fields here are a filename and a yes/no answer, so
 * neither wants one. */
constexpr uint8_t LINE_FIRST_PRINTABLE = 0x20;
constexpr uint8_t LINE_LAST_PRINTABLE = 0x7E;

bool line_edit(char* buffer, uint8_t capacity, uint8_t* length, uint8_t key) {
    if (key == LINE_RETURN) {
        return true;
    }

    if (key == LINE_DELETE) {
        if (*length > 0) {
            buffer[--(*length)] = '\0';
        }
        return false;
    }

    /* One cell is the terminator's, so the last key that fits leaves capacity
     * - 1 characters. */
    if (key >= LINE_FIRST_PRINTABLE && key <= LINE_LAST_PRINTABLE && *length + 1 < capacity) {
        buffer[(*length)++] = (char)key;
        buffer[*length] = '\0';
    }

    return false;
}
