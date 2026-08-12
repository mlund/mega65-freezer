/* Host driver for src/monitor/cpumap.c, used only by test/verify_cpumap.py.
 *
 * cpumap.c touches neither hardware nor the card -- it only does arithmetic on
 * the registers the freeze slot saved -- so the host can run it and check it
 * against a transcription of gs4510.vhdl's own resolve_address_to_long, rather
 * than against this code's arithmetic repeated.
 *
 * Commands on stdin, one per line, all values hex without $ or 0x:
 *   state <maplo> <maphi> <mblo> <mbhi> <port> <ddr> <d030>
 *   read <address>
 *   write <address>
 *   runs                       -> one line per merged run, then END
 *   typed <address>            -> an address as the monitor would take it typed
 */

#include "../src/monitor/cpumap.c"

#include <stdio.h>
#include <string.h>

int main(void) {
    char line[128];

    while (fgets(line, sizeof line, stdin)) {
        unsigned map_lo;
        unsigned map_hi;
        unsigned mb_lo;
        unsigned mb_hi;
        unsigned port;
        unsigned ddr;
        unsigned rom;
        unsigned address;

        if (sscanf(line, "state %x %x %x %x %x %x %x", &map_lo, &map_hi, &mb_lo, &mb_hi, &port,
                &ddr, &rom) == 7) {
            /* Packed as the freeze slot packs it, so cpumap_load()'s reading of
             * the high-byte-first MAP words is what gets exercised. */
            const uint8_t saved[8] = {(uint8_t)(map_lo >> 8), (uint8_t)map_lo,
                (uint8_t)(map_hi >> 8), (uint8_t)map_hi, (uint8_t)mb_lo, (uint8_t)mb_hi,
                (uint8_t)ddr, (uint8_t)port};

            cpumap_load(saved, (uint8_t)rom);
        } else if (sscanf(line, "read %x", &address) == 1) {
            printf("%lx\n", (unsigned long)resolve_cpu_address((uint16_t)address, false));
        } else if (sscanf(line, "write %x", &address) == 1) {
            printf("%lx\n", (unsigned long)resolve_cpu_address((uint16_t)address, true));
        } else if (sscanf(line, "typed %x", &address) == 1) {
            printf("%lx\n", (unsigned long)cpumap_typed_address(address));
        } else if (strncmp(line, "runs", 4) == 0) {
            CpuMapRun run;
            uint16_t at = 0;
            do {
                cpumap_run(at, &run);
                printf("%x %x %lx %u\n", run.first, run.last, (unsigned long)run.target, run.by);
                at = (uint16_t)(run.last + 1);
            } while (run.last != 0xFFFF);
            puts("END");
        } else {
            puts("?");
        }
    }
    return 0;
}
