#ifndef _MAKAOS_THREADS_H
#define _MAKAOS_THREADS_H 1
// C11 <threads.h> implemented as a thin shim over pthread.  fcft
// (and thus foot) uses the C11 API for its font-cache mutex + call-
// once; glibc provides this header too, musl only under _GNU_SOURCE.
// Thread-local storage (thread_local / _Thread_local) uses the
// compiler's __thread directly — no libc support needed.

#include <pthread.h>
#include <sched.h>
#include <time.h>

// Return values from every function below.
enum {
    thrd_success  = 0,
    thrd_busy     = 1,
    thrd_error    = 2,
    thrd_nomem    = 3,
    thrd_timedout = 4,
};

// Mutex flags.
#define mtx_plain      0
#define mtx_recursive  1
#define mtx_timed      2

typedef pthread_t        thrd_t;
typedef pthread_mutex_t  mtx_t;
typedef pthread_cond_t   cnd_t;
typedef pthread_key_t    tss_t;
typedef pthread_once_t   once_flag;
typedef int            (*thrd_start_t)(void*);
typedef void           (*tss_dtor_t)(void*);

#define ONCE_FLAG_INIT  PTHREAD_ONCE_INIT
#define TSS_DTOR_ITERATIONS 4

int  thrd_create (thrd_t* tid, thrd_start_t fn, void* arg);
int  thrd_join   (thrd_t tid, int* retval);
int  thrd_detach (thrd_t tid);
thrd_t thrd_current(void);
int  thrd_equal  (thrd_t a, thrd_t b);
void thrd_exit   (int retval) __attribute__((__noreturn__));
void thrd_yield  (void);
int  thrd_sleep  (const struct timespec* dur, struct timespec* remaining);

int  mtx_init    (mtx_t* m, int type);
void mtx_destroy (mtx_t* m);
int  mtx_lock    (mtx_t* m);
int  mtx_trylock (mtx_t* m);
int  mtx_timedlock(mtx_t* m, const struct timespec* abs);
int  mtx_unlock  (mtx_t* m);

int  cnd_init    (cnd_t* c);
void cnd_destroy (cnd_t* c);
int  cnd_wait    (cnd_t* c, mtx_t* m);
int  cnd_timedwait(cnd_t* c, mtx_t* m, const struct timespec* abs);
int  cnd_signal  (cnd_t* c);
int  cnd_broadcast(cnd_t* c);

int  tss_create  (tss_t* key, tss_dtor_t dtor);
void tss_delete  (tss_t key);
void* tss_get    (tss_t key);
int  tss_set     (tss_t key, void* val);

void call_once   (once_flag* flag, void (*fn)(void));

#endif
