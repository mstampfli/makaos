#include "shmem.h"
#include "pmm.h"
#include "kheap.h"
#include "errno.h"
#include "cred.h"
#include "vfs.h"
#include "smp.h"
#include "rcu.h"

// ── Named namespace (RCU-protected) ──────────────────────────────────────
// The whole namespace is a single object (shmem_ns_table_t) published via
// rcu_assign_pointer.  Readers walk lock-free inside rcu_read_lock() and
// atomically acquire a reference on the found shmem_t via shmem_tryget.
// Writers serialise on s_ns_wlock, build a fresh table, rcu_assign_pointer,
// and call_rcu the old table.

#define SHMEM_NS_INIT_CAP 32u

typedef struct {
    shmem_t* shm;   // NULL = empty slot
} ns_entry_t;

typedef struct shmem_ns_table {
    uint32_t    cap;
    uint32_t    cnt;
    rcu_head_t  rcu_head;   // Phase 5B: embedded for call_rcu_head
    ns_entry_t  slots[];
} shmem_ns_table_t;

static shmem_ns_table_t* s_namespace      = NULL;  // RCU-protected
static spinlock_t        s_namespace_lock = SPINLOCK_INIT;

// ── Internal helpers ─────────────────────────────────────────────────────

static int s_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static uint32_t shm_hash_str(const char* s, uint32_t cap) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h & (cap - 1u);
}

static shmem_ns_table_t* shm_ns_table_alloc(uint32_t cap) {
    shmem_ns_table_t* t = (shmem_ns_table_t*)kmalloc(
        sizeof(shmem_ns_table_t) + (uint64_t)cap * sizeof(ns_entry_t));
    if (!t) return NULL;
    t->cap = cap;
    t->cnt = 0;
    __builtin_memset(t->slots, 0, (uint64_t)cap * sizeof(ns_entry_t));
    return t;
}

static void shm_ns_raw_insert(shmem_ns_table_t* t, shmem_t* shm) {
    uint32_t i = shm_hash_str(shm->name, t->cap);
    while (t->slots[i].shm) i = (i + 1u) & (t->cap - 1u);
    t->slots[i].shm = shm;
    t->cnt++;
}

static void shm_ns_table_free_rcu(void* p) { kfree(p); }

// ── shmem_create ─────────────────────────────────────────────────────────

shmem_t* shmem_create(uint32_t npages, uint32_t uid, uint32_t gid, uint16_t mode) {
    if (npages > SHMEM_MAX_PAGES) return NULL;

    shmem_t* shm = kmalloc(sizeof(shmem_t));
    if (!shm) return NULL;
    shm->lock = (spinlock_t)SPINLOCK_INIT;

    // Allocate page array.  Start with capacity = npages (or minimum 1
    // so we always have a valid pointer even for zero-size objects).
    uint32_t cap = npages > 0 ? npages : 1;
    shm->pages = kmalloc(cap * sizeof(phys_addr_t));
    if (!shm->pages) { kfree(shm); return NULL; }

    // Zero all page slots (0 = not yet allocated).
    for (uint32_t i = 0; i < cap; i++)
        shm->pages[i] = 0;

    shm->npages    = npages;
    shm->max_pages = cap;
    shm->refcount  = 1;
    shm->uid       = uid;
    shm->gid       = gid;
    shm->mode      = mode;
    shm->name[0]   = '\0';

    return shm;
}

// ── shmem_ref ────────────────────────────────────────────────────────────

void shmem_ref(shmem_t* shm) {
    if (shm) atomic_add(&shm->refcount, 1u);
}

// Atomic tryget — increments refcount only if it is currently > 0.
// Returns 1 on success, 0 if the object is being destroyed (count == 0).
// Used by shmem_ns_find so a concurrent unref-to-zero cannot free the
// object under the caller.
int shmem_tryget(shmem_t* shm) {
    if (!shm) return 0;
    uint32_t old = __atomic_load_n(&shm->refcount, __ATOMIC_RELAXED);
    for (;;) {
        if (old == 0) return 0;
        if (__atomic_compare_exchange_n(&shm->refcount, &old, old + 1u,
                                         0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
    }
}

// Deferred final free — runs after an RCU grace period so any concurrent
// shmem_ns_find reader that observed the pointer has dropped out.
static void shmem_free_rcu(void* data) {
    shmem_t* shm = (shmem_t*)data;
    for (uint32_t i = 0; i < shm->npages; i++) {
        if (shm->pages[i])
            // Drop the OBJECT's ref.  Frames still mapped by a surviving
            // PTE (a process that hasn't munmapped yet) stay alive until
            // that PTE is torn down — they are NOT recycled out from
            // under a live mapping.  Same per-frame refcount every other
            // RAM page uses.
            pmm_ref_dec(shm->pages[i]);
    }
    kfree(shm->pages);
    kfree(shm);
}

// ── shmem_unref ──────────────────────────────────────────────────────────

void shmem_unref(shmem_t* shm) {
    if (!shm) return;
    // Atomic decrement-and-test — only the CPU that observes the
    // transition 1→0 owns the teardown.
    uint32_t prev = atomic_sub(&shm->refcount, 1u);
    if (prev != 1u) return; // still live (or was already 0 — a bug)

    // Remove from namespace if named.
    if (shm->name[0])
        shmem_ns_remove(shm);

    // Defer the physical-page and struct free until the current RCU
    // grace period ends so any concurrent shmem_ns_find reader that
    // observed this pointer has dropped out.  Expedited: invoked from
    // the last close() or last unmap of a shm fd — user-syscall latency.
    call_rcu_expedited(shmem_free_rcu, shm);
}

// ── shmem_get_page ───────────────────────────────────────────────────────

phys_addr_t shmem_get_page(shmem_t* shm, uint32_t pg_idx) {
    if (!shm) return PMM_INVALID_ADDR;

    // Hold shm->lock across the whole check-then-set: it serialises against a
    // concurrent shmem_resize (which kfree's/reallocs `pages` and mutates
    // npages) and against another faulter touching the same index.  Without it,
    // reading shm->pages[pg_idx] here could dereference an array a concurrent
    // grow kfree'd (UAF), and two first-touches of one index would double-
    // allocate the frame (leak + incoherent shared page).  Per-object lock, so
    // faults on different objects never contend; the hold spans one
    // pmm_buddy_alloc + a page zero, bounded.  Plain spin_lock (NOT irqsave):
    // get_page and shmem_resize run only in process context (#PF / ftruncate),
    // never from an IRQ handler, so no IRQ can take this lock, and we avoid
    // holding IRQs off across the page zero.
    spin_lock(&shm->lock);
    if (pg_idx >= shm->npages) {
        spin_unlock(&shm->lock);
        return PMM_INVALID_ADDR;
    }

    // Caller installs a PTE for the returned frame, so it gets one
    // reference.  The shmem object also holds one reference per
    // allocated page (taken at first touch below).  A frame is only
    // physically freed when BOTH the object (shmem_unref/resize) and
    // every mapping PTE (munmap/teardown) have released it — so a
    // shrink or destroy can never recycle a frame a process still maps.
    if (shm->pages[pg_idx]) {
        phys_addr_t f = shm->pages[pg_idx];
        pmm_ref_inc(f);   // ref for the caller's PTE
        spin_unlock(&shm->lock);
        return f;
    }

    // First touch: allocate a zeroed physical frame (rc=1 = object ref).
    phys_addr_t frame = pmm_buddy_alloc(0);
    if (frame == PMM_INVALID_ADDR) {
        spin_unlock(&shm->lock);
        return PMM_INVALID_ADDR;
    }

    __builtin_memset((void*)(frame + HHDM_OFFSET), 0, PAGE_SIZE);

    shm->pages[pg_idx] = frame;
    pmm_ref_inc(frame);    // +1 for the caller's PTE (object keeps rc=1)
    spin_unlock(&shm->lock);
    return frame;
}

// ── shmem_resize ─────────────────────────────────────────────────────────

int shmem_resize(shmem_t* shm, uint32_t new_npages) {
    if (!shm) return -EINVAL;
    if (new_npages > SHMEM_MAX_PAGES) return -EINVAL;

    // Serialise the whole resize against shmem_get_page (the #PF handler): the
    // grow path kfree's and reallocates `pages`, and both paths mutate npages
    // and the slots.  Same per-object lock get_page takes (process-context
    // only, so plain spin_lock).  Without it a concurrent fault dereferences a
    // kfree'd `pages` array (UAF).
    spin_lock(&shm->lock);
    if (new_npages == shm->npages) { spin_unlock(&shm->lock); return 0; }

    if (new_npages < shm->npages) {
        // Shrinking: drop the OBJECT's ref on pages beyond the new
        // size.  If a process still maps one, the PTE's ref keeps the
        // frame alive (stale-but-private) until munmap — never
        // recycled under the live mapping.  This was the heisenbug:
        // foot ftruncate-shrinks its grid/scrollback shm while still
        // mapped, and the old pmm_buddy_free recycled the frame into a
        // new thread's stack while foot kept writing to it.
        for (uint32_t i = new_npages; i < shm->npages; i++) {
            if (shm->pages[i]) {
                pmm_ref_dec(shm->pages[i]);
                shm->pages[i] = 0;
            }
        }
        shm->npages = new_npages;
        spin_unlock(&shm->lock);
        return 0;
    }

    // Growing: may need to expand the pages[] array.
    if (new_npages > shm->max_pages) {
        // Grow to next power-of-2 or new_npages, whichever is larger.
        uint32_t new_cap = shm->max_pages;
        while (new_cap < new_npages) {
            if (new_cap == 0) new_cap = 1;
            else              new_cap *= 2;
        }
        if (new_cap > SHMEM_MAX_PAGES) new_cap = SHMEM_MAX_PAGES;
        if (new_cap < new_npages) { spin_unlock(&shm->lock); return -EINVAL; }

        phys_addr_t* new_arr = kmalloc(new_cap * sizeof(phys_addr_t));
        if (!new_arr) { spin_unlock(&shm->lock); return -ENOMEM; }

        // Copy existing entries.
        for (uint32_t i = 0; i < shm->npages; i++)
            new_arr[i] = shm->pages[i];
        // Zero new entries.
        for (uint32_t i = shm->npages; i < new_cap; i++)
            new_arr[i] = 0;

        kfree(shm->pages);
        shm->pages     = new_arr;
        shm->max_pages = new_cap;
    } else {
        // Array is large enough — just zero the new slots.
        for (uint32_t i = shm->npages; i < new_npages; i++)
            shm->pages[i] = 0;
    }

    shm->npages = new_npages;
    spin_unlock(&shm->lock);
    return 0;
}

// ── shmem_ns_find ────────────────────────────────────────────────────────

// Reader — lock-free inside rcu_read_lock.  Returns shm with +1 refcount
// on success (via shmem_tryget); caller must shmem_unref when done.
// Returns NULL if not found or if the object is mid-teardown.
shmem_t* shmem_ns_find(const char* name) {
    if (!name || !name[0]) return NULL;
    shmem_t* result = NULL;
    rcu_read_lock();
    shmem_ns_table_t* t = rcu_dereference(s_namespace);
    if (t) {
        uint32_t cap = t->cap;
        uint32_t i = shm_hash_str(name, cap);
        for (uint32_t n = 0; n < cap; n++) {
            shmem_t* shm = t->slots[i].shm;
            if (!shm) break;
            if (s_streq(shm->name, name)) {
                if (shmem_tryget(shm)) result = shm;
                break;
            }
            i = (i + 1u) & (cap - 1u);
        }
    }
    rcu_read_unlock();
    return result;
}

// ── shmem_ns_insert ──────────────────────────────────────────────────────

int shmem_ns_insert(shmem_t* shm) {
    if (!shm || !shm->name[0]) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&s_namespace_lock);
    shmem_ns_table_t* old = s_namespace;
    uint32_t old_cap = old ? old->cap : 0;
    uint32_t old_cnt = old ? old->cnt : 0;

    // Duplicate check directly on the live table under the writer lock.
    if (old) {
        uint32_t i = shm_hash_str(shm->name, old_cap);
        for (uint32_t n = 0; n < old_cap; n++) {
            if (!old->slots[i].shm) break;
            if (s_streq(old->slots[i].shm->name, shm->name)) {
                spin_unlock_irqrestore(&s_namespace_lock, flags);
                return -EEXIST;
            }
            i = (i + 1u) & (old_cap - 1u);
        }
    }

    uint32_t new_cap = old_cap ? old_cap : SHMEM_NS_INIT_CAP;
    if ((old_cnt + 1u) * 4u >= new_cap * 3u)
        new_cap = old_cap ? old_cap * 2u : SHMEM_NS_INIT_CAP;

    shmem_ns_table_t* neu = shm_ns_table_alloc(new_cap);
    if (!neu) { spin_unlock_irqrestore(&s_namespace_lock, flags); return -ENOMEM; }

    if (old) {
        for (uint32_t i = 0; i < old->cap; i++)
            if (old->slots[i].shm)
                shm_ns_raw_insert(neu, old->slots[i].shm);
    }
    shm_ns_raw_insert(neu, shm);

    rcu_assign_pointer(s_namespace, neu);
    spin_unlock_irqrestore(&s_namespace_lock, flags);
    if (old) call_rcu_head(&old->rcu_head, shm_ns_table_free_rcu, old);
    return 0;
}

// ── shmem_ns_remove ──────────────────────────────────────────────────────

void shmem_ns_remove(shmem_t* shm) {
    if (!shm || !shm->name[0]) return;
    uint64_t flags = spin_lock_irqsave(&s_namespace_lock);
    shmem_ns_table_t* old = s_namespace;
    if (!old) { spin_unlock_irqrestore(&s_namespace_lock, flags); return; }

    shmem_ns_table_t* neu = shm_ns_table_alloc(old->cap);
    if (!neu) { spin_unlock_irqrestore(&s_namespace_lock, flags); return; }
    int removed = 0;
    for (uint32_t i = 0; i < old->cap; i++) {
        shmem_t* cur = old->slots[i].shm;
        if (!cur) continue;
        if (!removed && cur == shm) { removed = 1; continue; }
        shm_ns_raw_insert(neu, cur);
    }

    rcu_assign_pointer(s_namespace, neu);
    spin_unlock_irqrestore(&s_namespace_lock, flags);
    call_rcu_head(&old->rcu_head, shm_ns_table_free_rcu, old);
}

// ── shmem_check_access ──────────────────────────────────────────────────
// POSIX permission check: owner / group / other × read / write.
// oflags: 0=O_RDONLY, 1=O_WRONLY, 2=O_RDWR.
// Returns 0 if access is granted, -EACCES otherwise.

int shmem_check_access(const shmem_t* shm, const cred_t* cred, int oflags) {
    if (!shm || !cred) return -EINVAL;

    // Root bypasses permission checks.
    if (cred->euid == 0) return 0;

    uint16_t mode = shm->mode;
    uint16_t bits;

    if (cred->euid == shm->uid) {
        bits = (mode >> 6) & 7;    // owner bits
    } else if (cred_in_group(cred, shm->gid)) {
        bits = (mode >> 3) & 7;    // group bits
    } else {
        bits = mode & 7;           // other bits
    }

    int need_r = (oflags == 0 || oflags == 2);  // O_RDONLY or O_RDWR
    int need_w = (oflags == 1 || oflags == 2);  // O_WRONLY or O_RDWR

    if (need_r && !(bits & 4)) return -EACCES;
    if (need_w && !(bits & 2)) return -EACCES;
    return 0;
}

// ── shmem fd VFS operations ─────────────────────────────────────────────

// shmem fds are not seekable/readable/writable through read()/write() —
// the sole purpose is to pass to mmap() and ftruncate().
// read/write return -EINVAL; seek returns -ESPIPE.

static int64_t shmem_fd_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self; (void)buf; (void)len;
    return -EINVAL;
}

static int64_t shmem_fd_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self; (void)buf; (void)len;
    return -EINVAL;
}

void shmem_fd_close(vfs_file_t* self) {
    if (!self) return;
    shmem_t* shm = (shmem_t*)self->ctx;
    shmem_unref(shm);
    kfree(self);
}

vfs_file_t* shmem_fd_create(shmem_t* shm) {
    if (!shm) return NULL;
    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) return NULL;
    __builtin_memset(f, 0, sizeof(*f));

    f->read     = shmem_fd_read;
    f->write    = shmem_fd_write;
    f->close    = shmem_fd_close;
    f->seek     = NULL;
    f->poll           = NULL;
    f->ioctl          = NULL;
    f->ctx            = shm;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags          = 0;
    f->refcount    = 1;
    f->rights   = 0xFFFFFFFF; // all rights — narrowed by restrict_fd if needed
    f->path[0]  = '\0';

    shmem_ref(shm); // fd owns a reference
    return f;
}
