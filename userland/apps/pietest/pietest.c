/*
 * pietest -- PIE self-relocation smoke test (dynamic loader Phase 0/1).
 *
 * Freestanding, no libc.  A headless serial capture cannot see userland stdout
 * (the tty0 console renders to the framebuffer once it is up), so pietest
 * signals its result the one way the kernel ALWAYS mirrors to serial: a fatal
 * page fault.  It stores to a sentinel address whose low nibble encodes the
 * outcome, so the kernel's PF-KILL dump prints  CR2=0x000000005EC0DE0X  with
 * comm=pietest:
 *
 *   bit0  R_X86_64_RELATIVE applied: `pmsg` relocated to a real mapped address
 *         (>= 0x100000, not the base-0 link value) AND the byte it points at is
 *         the expected 'P'.
 *   bit1  the reloc-target page kept its file backing: `canary` is intact, i.e.
 *         the loader did NOT zero-fill the page around the relocated word.
 *
 * Full pass  => CR2=0x000000005EC0DE03.  Any other value pinpoints what broke
 * (…01 = data-backing lost, …02 = reloc not applied, …00 = both).
 *
 * Built with: cc -fPIC -ffreestanding -c ; ld -pie --export-dynamic -e _start
 */
static const char msg[] = "PIE-RELOC-OK\n";
static const char *volatile pmsg = msg;          /* stored ptr -> R_X86_64_RELATIVE */
static volatile unsigned canary = 0xCAFEBABEu;   /* .data, NOT relocated */

void _start(void) {
    unsigned long p = (unsigned long)pmsg;
    unsigned long r = 0x5EC0DE00UL;
    /* Check pmsg as a VALUE first (a broken reloc leaves it base-0, < 0x100000),
     * so a wild deref can never corrupt the sentinel we are about to fault on. */
    if (p >= 0x100000UL && ((const char*)p)[0] == 'P') r |= 0x01UL;
    if (canary == 0xCAFEBABEu)                         r |= 0x02UL;
    *(volatile unsigned char*)r = 0;   /* PF-KILL: CR2 encodes the result */
    for (;;) { }
}
