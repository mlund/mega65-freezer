/* cc65 extensions the llvm-mos libc does not declare. */
char* itoa(int, char*, int);
char* utoa(unsigned, char*, int);

/* PETSCII key codes from cc65's <cbm.h>, which the -t c64 build supplied.
 * mega65-libc's conio.h does not define them. */
#define CH_CURS_UP 145
#define CH_CURS_DOWN 17
#define CH_CURS_LEFT 157
#define CH_CURS_RIGHT 29
#define CH_DEL 20
#define CH_INS 148
#define CH_ENTER 13
#define CH_STOP 3
#define CH_ESC 27
