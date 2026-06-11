// ── wordexp.c — shell-style word expansion (subset) ──────────────────
//
// Implements what sway's config loading actually exercises: leading-~
// expansion to $HOME, $VAR and ${VAR} substitution, and whitespace
// field splitting.  Command substitution is rejected (WRDE_CMDSUB) and
// glob patterns pass through literally — a non-matching include line
// is a soft error in every consumer we ship.

#include <wordexp.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

extern char* getenv(const char* name);

// Append one expanded character, growing the scratch buffer as needed.
static int we_putc(char** buf, size_t* len, size_t* cap, char c) {
    if (*len + 1 >= *cap) {
        size_t ncap = *cap ? *cap * 2 : 128;
        char* nb = realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    (*buf)[(*len)++] = c;
    return 0;
}

static int we_puts(char** buf, size_t* len, size_t* cap, const char* s) {
    while (*s)
        if (we_putc(buf, len, cap, *s++) != 0) return -1;
    return 0;
}

int wordexp(const char* words, wordexp_t* we, int flags) {
    if (!words || !we) return WRDE_BADVAL;
    if (!(flags & WRDE_APPEND)) {
        we->we_wordc = 0;
        we->we_wordv = NULL;
        we->we_offs  = 0;
    }

    // Expansion pass: ~, $VAR, ${VAR} into one flat buffer; quoting
    // ('...' literal, "..." allows $) is honoured enough for config
    // lines; ` and $( are rejected as command substitution.
    char*  buf = NULL;
    size_t len = 0, cap = 0;
    int in_squote = 0, in_dquote = 0;

    for (const char* p = words; *p; p++) {
        char c = *p;
        if (in_squote) {
            if (c == '\'') { in_squote = 0; continue; }
            if (we_putc(&buf, &len, &cap, c) != 0) goto nospace;
            continue;
        }
        if (c == '\'' && !in_dquote) { in_squote = 1; continue; }
        if (c == '"') { in_dquote = !in_dquote; continue; }
        if (c == '\\' && p[1]) {
            if (we_putc(&buf, &len, &cap, p[1]) != 0) goto nospace;
            p++;
            continue;
        }
        if (c == '`' || (c == '$' && p[1] == '(')) {
            free(buf);
            return WRDE_CMDSUB;
        }
        if (c == '~' && (p == words || p[-1] == ' ' || p[-1] == '\t')
            && !in_dquote) {
            const char* home = getenv("HOME");
            if (home) {
                if (we_puts(&buf, &len, &cap, home) != 0) goto nospace;
                continue;
            }
            // No HOME: keep the literal '~'.
        } else if (c == '$') {
            const char* name      = p + 1;
            const char* name_end  = name;
            int braced = (*name == '{');
            if (braced) { name++; name_end = name; }
            while ((*name_end >= 'A' && *name_end <= 'Z') ||
                   (*name_end >= 'a' && *name_end <= 'z') ||
                   (*name_end >= '0' && *name_end <= '9') ||
                   *name_end == '_')
                name_end++;
            if (name_end > name && (!braced || *name_end == '}')) {
                char vname[128];
                size_t vlen = (size_t)(name_end - name);
                if (vlen < sizeof(vname)) {
                    memcpy(vname, name, vlen);
                    vname[vlen] = '\0';
                    const char* val = getenv(vname);
                    if (!val && (flags & WRDE_UNDEF)) {
                        free(buf);
                        return WRDE_BADVAL;
                    }
                    if (val && we_puts(&buf, &len, &cap, val) != 0)
                        goto nospace;
                    p = braced ? name_end : name_end - 1;
                    continue;
                }
            }
            // Not a recognisable variable — keep the literal '$'.
        }
        if (we_putc(&buf, &len, &cap, c) != 0) goto nospace;
    }
    if (we_putc(&buf, &len, &cap, '\0') != 0) goto nospace;

    // Field splitting on spaces/tabs.
    size_t wordc = 0;
    char** wordv = NULL;
    char* cursor = buf;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (!*cursor) break;
        char* start = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t') cursor++;
        size_t wl = (size_t)(cursor - start);
        char* w = malloc(wl + 1);
        if (!w) goto nospace_v;
        memcpy(w, start, wl);
        w[wl] = '\0';
        char** nv = realloc(wordv, (wordc + 2) * sizeof(char*));
        if (!nv) { free(w); goto nospace_v; }
        wordv = nv;
        wordv[wordc++] = w;
        wordv[wordc]   = NULL;
    }

    free(buf);
    we->we_wordc = wordc;
    we->we_wordv = wordv;
    return 0;

nospace_v:
    if (wordv) {
        for (size_t i = 0; wordv[i]; i++) free(wordv[i]);
        free(wordv);
    }
nospace:
    free(buf);
    return WRDE_NOSPACE;
}

void wordfree(wordexp_t* we) {
    if (!we || !we->we_wordv) return;
    for (size_t i = 0; i < we->we_wordc; i++)
        free(we->we_wordv[i]);
    free(we->we_wordv);
    we->we_wordv = NULL;
    we->we_wordc = 0;
}
