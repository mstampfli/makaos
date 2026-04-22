// ── c11_threads.c — C11 <threads.h> on top of pthread ───────────────
//
// fcft (and a handful of other ports) use the C11 API even when
// pthread is available.  One-to-one mapping since our pthread types
// are already the same structs.  syslog client bodies also live here
// for convenience — both are small.

#include <threads.h>
#include <syslog.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>

// ── threads ─────────────────────────────────────────────────────────
// pthread_create takes `void* (*)(void*)` but C11 takes `int (*)(void*)`.
// Wrap via a trampoline that discards the int return.
typedef struct { thrd_start_t fn; void* arg; } thrd_trampoline_t;

extern void* malloc(unsigned long);
extern void  free(void*);

static void* thrd_trampoline(void* raw) {
    thrd_trampoline_t t = *(thrd_trampoline_t*)raw;
    free(raw);
    int r = t.fn(t.arg);
    return (void*)(long)r;
}

int thrd_create(thrd_t* tid, thrd_start_t fn, void* arg) {
    if (!tid || !fn) return thrd_error;
    thrd_trampoline_t* t = (thrd_trampoline_t*)malloc(sizeof(*t));
    if (!t) return thrd_nomem;
    t->fn = fn; t->arg = arg;
    int r = pthread_create(tid, NULL, thrd_trampoline, t);
    if (r != 0) { free(t); return r == ENOMEM ? thrd_nomem : thrd_error; }
    return thrd_success;
}

int thrd_join(thrd_t tid, int* retval) {
    void* p = NULL;
    if (pthread_join(tid, &p) != 0) return thrd_error;
    if (retval) *retval = (int)(long)p;
    return thrd_success;
}

int thrd_detach(thrd_t tid) {
    return pthread_detach(tid) == 0 ? thrd_success : thrd_error;
}

thrd_t thrd_current(void)           { return pthread_self(); }
int    thrd_equal(thrd_t a, thrd_t b) { return pthread_equal(a, b); }

__attribute__((noreturn))
void thrd_exit(int retval) {
    pthread_exit((void*)(long)retval);
}

void thrd_yield(void) { sched_yield(); }

extern int nanosleep(const struct timespec*, struct timespec*);
int thrd_sleep(const struct timespec* dur, struct timespec* rem) {
    int r = nanosleep(dur, rem);
    if (r == 0) return 0;
    return errno == EINTR ? -1 : -2;
}

// ── mutex ───────────────────────────────────────────────────────────
int mtx_init(mtx_t* m, int type) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (type & mtx_recursive)
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    int r = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return r == 0 ? thrd_success : thrd_error;
}
void mtx_destroy(mtx_t* m)      { pthread_mutex_destroy(m); }
int  mtx_lock(mtx_t* m)         { return pthread_mutex_lock(m)    == 0 ? thrd_success : thrd_error; }
int  mtx_trylock(mtx_t* m)      { int r = pthread_mutex_trylock(m); return r == 0 ? thrd_success : r == EBUSY ? thrd_busy : thrd_error; }
int  mtx_timedlock(mtx_t* m, const struct timespec* abs) {
    int r = pthread_mutex_timedlock(m, abs);
    if (r == 0) return thrd_success;
    return r == ETIMEDOUT ? thrd_timedout : thrd_error;
}
int  mtx_unlock(mtx_t* m)       { return pthread_mutex_unlock(m)  == 0 ? thrd_success : thrd_error; }

// ── cond var ────────────────────────────────────────────────────────
int  cnd_init(cnd_t* c)         { return pthread_cond_init(c, NULL)     == 0 ? thrd_success : thrd_error; }
void cnd_destroy(cnd_t* c)      { pthread_cond_destroy(c); }
int  cnd_wait(cnd_t* c, mtx_t* m) { return pthread_cond_wait(c, m)       == 0 ? thrd_success : thrd_error; }
int  cnd_timedwait(cnd_t* c, mtx_t* m, const struct timespec* abs) {
    int r = pthread_cond_timedwait(c, m, abs);
    if (r == 0) return thrd_success;
    return r == ETIMEDOUT ? thrd_timedout : thrd_error;
}
int  cnd_signal(cnd_t* c)       { return pthread_cond_signal(c)    == 0 ? thrd_success : thrd_error; }
int  cnd_broadcast(cnd_t* c)    { return pthread_cond_broadcast(c) == 0 ? thrd_success : thrd_error; }

// ── thread-specific storage ─────────────────────────────────────────
int  tss_create(tss_t* k, tss_dtor_t d) { return pthread_key_create(k, d) == 0 ? thrd_success : thrd_error; }
void tss_delete(tss_t k)                { pthread_key_delete(k); }
void* tss_get(tss_t k)                  { return pthread_getspecific(k); }
int  tss_set(tss_t k, void* v)          { return pthread_setspecific(k, v) == 0 ? thrd_success : thrd_error; }

// ── call_once ───────────────────────────────────────────────────────
void call_once(once_flag* f, void (*fn)(void)) { pthread_once(f, fn); }

// ── syslog (client stub) ─────────────────────────────────────────────
// No syslog daemon on MakaOS; forward the formatted message to stderr
// so port libraries' error messages don't vanish.  openlog / closelog
// are no-ops; setlogmask just stores the mask.  Enough to satisfy
// every port we've seen.
static int s_logmask = 0xFF;
void openlog(const char* ident, int option, int facility) { (void)ident; (void)option; (void)facility; }
void closelog(void) {}
int  setlogmask(int mask) { int old = s_logmask; if (mask) s_logmask = mask; return old; }

void vsyslog(int priority, const char* fmt, va_list ap) {
    if (!(s_logmask & (1 << (priority & 7)))) return;
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
void syslog(int priority, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);
}
