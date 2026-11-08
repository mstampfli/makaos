/*
 * piehello -- Phase 0 milestone: a REAL libc program built as a static PIE.
 *
 * Unlike freestanding pietest, this has main() and links crt0 (_entry) + the
 * PIC libc.a.  Running it end to end proves the whole PIC-libc PIE path:
 *   - kernel loads the ET_DYN at a bias and applies its R_X86_64_RELATIVE relocs
 *   - crt0 runs: __makaos_tls_init (needs a correct load_bias-adjusted AT_PHDR),
 *     then .init_array constructors, then main
 *   - libc calls work under PIC codegen
 *   - the executable's own __thread TLS works (local-exec / TPOFF)
 *
 * A headless serial capture can't see stdout (tty0 -> framebuffer), so the
 * result is signalled the one way the kernel always mirrors to serial: a fatal
 * page fault at a sentinel address whose low nibble encodes the checks, so the
 * PF-KILL dump prints  CR2=0x000000005EC0DE3F  with comm=piehello on full pass:
 *   bit0 R_X86_64_RELATIVE applied     bit1 libc call (strlen) under PIC
 *   bit2 local-exec __thread TLS        bit3 .init_array constructor ran
 *   bit4 libc heap (malloc/free)        bit5 stdio formatting (snprintf)
 */
extern unsigned long strlen(const char*);
extern void* malloc(unsigned long);
extern void  free(void*);
extern int   snprintf(char*, unsigned long, const char*, ...);

static const char msg[] = "piehello";
static const char *volatile pmsg = msg;         /* stored ptr -> R_X86_64_RELATIVE */
static volatile int ctor_ran = 0;
static __thread volatile int tlsvar = 0;        /* PT_TLS, local-exec (TPOFF) */

__attribute__((constructor)) static void ctor(void) { ctor_ran = 0xC7; }

int main(void) {
    unsigned long r = 0x5EC0DE00UL;
    /* pmsg checked as a VALUE first: a broken reloc leaves it base-0 (<0x100000),
     * so we never deref a wild pointer and corrupt the sentinel. */
    if ((unsigned long)pmsg >= 0x100000UL && pmsg[0] == 'p') r |= 0x01UL;
    if (strlen(msg) == 8)                                    r |= 0x02UL;
    tlsvar = 0x4242;
    if (tlsvar == 0x4242)                                    r |= 0x04UL;
    if (ctor_ran == 0xC7)                                    r |= 0x08UL;
    void* p = malloc(64);
    if (p) { *(volatile char*)p = 'X'; if (*(volatile char*)p == 'X') r |= 0x10UL; free(p); }
    char buf[32];
    for (int i = 0; i < 32; i++) buf[i] = 1;
    snprintf(buf, sizeof buf, "%d-%s", 42, msg);           /* stdio formatting under PIC */
    /* expect "42-piehello\0" -- check the produced bytes, not the return convention */
    if (buf[0]=='4' && buf[1]=='2' && buf[2]=='-' && buf[3]=='p'
        && buf[10]=='o' && buf[11]=='\0') r |= 0x20UL;
    *(volatile unsigned char*)r = 0;   /* PF-KILL: CR2=0x5EC0DE3F on full pass */
    return 0;
}
