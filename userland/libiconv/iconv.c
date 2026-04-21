// MakaOS identity-only iconv.  UTF-8 is MakaOS's canonical encoding;
// every conversion between UTF-8 aliases (case-insensitive) is a byte
// copy.  Anything else returns EINVAL / (iconv_t)-1.

#include "iconv.h"
#include <errno.h>
#include <string.h>

static int is_utf8(const char* code) {
    if (!code) return 0;
    // Case-insensitive comparison to "UTF-8" / "UTF8" / "US-ASCII".
    // MakaOS treats US-ASCII as a subset of UTF-8 for transport.
    const char* aliases[] = { "UTF-8", "UTF8", "US-ASCII", "ASCII", 0 };
    for (int i = 0; aliases[i]; i++) {
        const char* a = aliases[i]; const char* b = code;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (*a == 0 && *b == 0) return 1;
    }
    return 0;
}

iconv_t iconv_open(const char* to, const char* from) {
    if (is_utf8(to) && is_utf8(from)) return (iconv_t)(size_t)1;
    errno = EINVAL;
    return (iconv_t)-1;
}

size_t iconv(iconv_t cd, char** inbuf, size_t* inleft,
                         char** outbuf, size_t* outleft) {
    (void)cd;
    if (!inbuf || !*inbuf) return 0;   // state reset — no-op
    size_t n = *inleft < *outleft ? *inleft : *outleft;
    memcpy(*outbuf, *inbuf, n);
    *inbuf  += n; *inleft  -= n;
    *outbuf += n; *outleft -= n;
    if (*inleft > 0) { errno = E2BIG; return (size_t)-1; }
    return 0;
}

int iconv_close(iconv_t cd) { (void)cd; return 0; }
