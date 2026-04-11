// bash_stubs.c — Minimal stubs for bash-internal symbols on MakaOS.
//
// These are NOT libc gaps — they are bash-specific features that were
// excluded from compilation (to reduce binary size / avoid dependencies)
// but whose symbols are still referenced by other bash translation units.
//
// Each stub documents why it exists and what would be needed to unstub it.

#include <stddef.h>
#include <stdint.h>

// ── bash $RANDOM — internal PRNG ─────────────────────────────────────────
// bash uses its own PRNG for $RANDOM rather than /dev/urandom.
// These are called from variables.c.  To unstub: compile bash's random.c.
static uint64_t _rng = 12345678901234567ULL;
static uint32_t _r32(void) {
    _rng ^= _rng << 13;
    _rng ^= _rng >> 7;
    _rng ^= _rng << 17;
    return (uint32_t)(_rng >> 32);
}
void          sbrand(unsigned long s)          { _rng = s ^ 0xdeadbeefcafeULL; }
void          seedrand(unsigned long s)        { sbrand(s); }
unsigned long brand(void)                      { return _r32(); }
uint32_t      get_urandom32(void)              { return _r32(); }
void          seedrand32(uint32_t a, uint32_t b) { _rng = ((uint64_t)a << 32) | b; }

// ── bash /dev/tcp — network redirection ──────────────────────────────────
// bash can open network connections via /dev/tcp/host/port.
// To unstub: compile bash's lib/sh/netopen.c + netconn.c.
int isnetconn(int fd)         { (void)fd; return 0; }
int netopen(const char* spec) { (void)spec; return -1; }

// ── bash unicode — multibyte/wide char support ───────────────────────────
// Used for wide-character display widths in readline.
// To unstub: compile bash's lib/sh/unicode.c + fnxform.c.
int   u32cconv(unsigned int c, char* s) { if (c < 0x80) { s[0] = (char)c; return 1; } return 0; }
void  u32reset(void) {}
char* fnx_fromfs(char* s, size_t n) { (void)n; return s; }

// ── oslib functions (from lib/sh/oslib.c, can't compile due to conflicts) ──
long getmaxgroups(void) { return 16; }
long getmaxchild(void)  { return 4096; }

// ── bash programmable completion ─────────────────────────────────────────
// Now compiled from bash source (pcomplete.c + pcomplib.c).
// Only stubs for symbols still missing after compilation go here.
