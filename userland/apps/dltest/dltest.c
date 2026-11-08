/*
 * dltest -- exercises the real dlopen loader end to end.  Headless serial can't
 * see stdout, so the result is signalled via a sentinel page fault whose low 12
 * bits encode independent checks; the PF-KILL dump prints CR2 with comm=dltest.
 * Base 0x5EC00000 has a clean low 16 bits so every bit is a real signal.
 * Full pass => CR2=0x000000005EC00FFF.
 *
 *   bit0  dlopen mapped + relocated /lib/libdso.so
 *   bit1  dlsym found dso_answer
 *   bit2  calling into the .so returns 42
 *   bit3  the .so's own RELATIVE reloc applied (dso_name() == "libdso")
 *   bit4  dlclose returned 0
 *   bit5  open(/etc/passwd) succeeded (deny-by-default test is live)
 *   bit6  SECURITY (ASVS V4): PROT_EXEC mmap on the non-exec passwd is REFUSED
 *   bit7  the .so's libc import (strlen) bound to the exe's exported .dynsym
 *   bit8  a WEAK undefined symbol resolves to 0 (dlopen still succeeds)
 *   bit9  a .so importing a STRONG undefined symbol is REJECTED + cleaned up
 *   bit10 DT_NEEDED: libdso.so pulls in libdso2.so; dso_dep() == 100
 *   bit11 dlopen(NULL) global scope: dlsym finds the exe's strlen
 *   bit12 THREAD-SAFETY: 3 threads hammer dlopen/dlsym/call/dlclose of the same
 *         .so concurrently; the loader lock + refcount keep it correct.
 *   bit13 DYNAMIC TLS: a dlopen'd .so's __thread var is per-thread -- a spawned
 *         thread sees the template value (7), not the main thread's mutation.
 *         Full pass => CR2=0x5EC03FFF.
 */
#include <pthread.h>
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int   dlclose(void*);
extern int   open(const char*, int, ...);
extern void* mmap(void*, unsigned long, int, int, int, long);

// Dynamic-TLS test: the spawned thread must see the .so's __thread var at its
// template value (7), NOT the main thread's mutation -- proving per-thread TLS.
static int (*g_tget)(void);
static void (*g_tset)(int);
static void* tls_thread(void* a) {
    int* out = (int*)a;
    *out = g_tget();          // this thread's own copy -> must be 7, not main's 100
    g_tset(200);
    if (g_tget() != 200) *out = -1;   // this thread's write must stick locally
    return (void*)0;
}

// Concurrent load/use/unload of the same object -- races the loaded-object list
// + refcount; without the lock this corrupts or double-frees.
static void* hammer(void* a) {
    (void)a;
    for (int k = 0; k < 300; k++) {
        void* hh = dlopen("/lib/libdso.so", 2);
        if (!hh) return (void*)0;
        int (*ans)(void) = (int (*)(void))dlsym(hh, "dso_answer");
        if (!ans || ans() != 42) { dlclose(hh); return (void*)0; }
        if (dlclose(hh) != 0) return (void*)0;
    }
    return (void*)1;
}

int main(void) {
    unsigned long r = 0x5EC00000UL;

    void* h = dlopen("/lib/libdso.so", 2 /*RTLD_NOW*/);
    if (h) {
        r |= 0x001UL;
        int (*answer)(void) = (int (*)(void))dlsym(h, "dso_answer");
        if (answer) r |= 0x002UL;
        if (answer && answer() == 42) r |= 0x004UL;
        const char* (*nm)(void) = (const char* (*)(void))dlsym(h, "dso_name");
        if (nm) { const char* n = nm(); if (n && n[0]=='l' && n[3]=='d' && n[5]=='o') r |= 0x008UL; }
        int (*dslen)(void) = (int (*)(void))dlsym(h, "dso_strlen");
        if (dslen && dslen() == 4) r |= 0x080UL;               /* libc import bound to exe */
        void* (*wk)(void) = (void* (*)(void))dlsym(h, "dso_weak");
        if (wk && wk() == (void*)0) r |= 0x100UL;              /* weak undefined -> 0 */
        int (*dep)(void) = (int (*)(void))dlsym(h, "dso_dep");
        if (dep && dep() == 100) r |= 0x400UL;                 /* DT_NEEDED dep called */
        if (dlclose(h) == 0) r |= 0x010UL;
    }

    /* SECURITY: PROT_EXEC on a non-executable file must be refused (ASVS V4). */
    int pf = open("/etc/passwd", 0 /*O_RDONLY*/, 0);
    if (pf >= 0) {
        r |= 0x020UL;
        void* m = mmap((void*)0, 4096, 1 /*PROT_READ*/ | 4 /*PROT_EXEC*/, 2 /*MAP_PRIVATE*/, pf, 0);
        if (m == (void*)-1) r |= 0x040UL;
    }

    /* reject: a .so with a strong undefined import must fail (return NULL). */
    if (dlopen("/lib/libdsobad.so", 2) == (void*)0) r |= 0x200UL;

    /* dlopen(NULL): the global scope resolves the exe's exported symbols. */
    void* g = dlopen((const char*)0, 2);
    if (g && dlsym(g, "strlen") != (void*)0) r |= 0x800UL;

    /* thread-safety: 3 concurrent hammer loops of the same .so must all succeed. */
    pthread_t t1, t2;
    void *r1 = (void*)0, *r2 = (void*)0;
    int c1 = pthread_create(&t1, (void*)0, hammer, (void*)0);
    int c2 = pthread_create(&t2, (void*)0, hammer, (void*)0);
    void* rm = hammer((void*)0);
    if (c1 == 0) pthread_join(t1, &r1);
    if (c2 == 0) pthread_join(t2, &r2);
    if (rm == (void*)1 && c1 == 0 && r1 == (void*)1 && c2 == 0 && r2 == (void*)1) r |= 0x1000UL;

    /* bit13: dynamic TLS -- a dlopen'd .so's __thread var is per-thread. */
    void* ht = dlopen("/lib/libtlsdso.so", 2);
    if (ht) {
        g_tget = (int (*)(void))dlsym(ht, "dso_tls_get");
        g_tset = (void (*)(int))dlsym(ht, "dso_tls_set");
        if (g_tget && g_tset) {
            int ok = (g_tget() == 7);       /* main thread: template value */
            g_tset(100);
            ok = ok && (g_tget() == 100);
            int other = -2;
            pthread_t tt;
            if (pthread_create(&tt, (void*)0, tls_thread, &other) == 0) pthread_join(tt, (void*)0);
            /* the other thread saw 7 (isolation), and main still sees 100 */
            if (ok && other == 7 && g_tget() == 100) r |= 0x2000UL;
        }
        dlclose(ht);
    }

    *(volatile unsigned char*)r = 0;   /* PF-KILL: CR2=0x5EC03FFF on full pass */
    return 0;
}
