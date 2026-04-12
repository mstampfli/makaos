#include "shmem.h"
#include "pmm.h"
#include "kheap.h"
#include "errno.h"
#include "cred.h"
#include "vfs.h"

// ── Named namespace ──────────────────────────────────────────────────────
// Flat table of named shmem objects.  Protected by the single-threaded
// kernel (no SMP yet — when SMP arrives, this needs a spinlock).

typedef struct {
    shmem_t* shm;   // NULL = empty slot
} ns_entry_t;

static ns_entry_t s_namespace[SHMEM_NS_MAX];

// ── Internal helpers ─────────────────────────────────────────────────────

static int s_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

// ── shmem_create ─────────────────────────────────────────────────────────

shmem_t* shmem_create(uint32_t npages, uint32_t uid, uint32_t gid, uint16_t mode) {
    if (npages > SHMEM_MAX_PAGES) return NULL;

    shmem_t* shm = kmalloc(sizeof(shmem_t));
    if (!shm) return NULL;

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
    if (shm) shm->refcount++;
}

// ── shmem_unref ──────────────────────────────────────────────────────────

void shmem_unref(shmem_t* shm) {
    if (!shm) return;
    if (shm->refcount == 0) return; // safety — should never happen

    if (--shm->refcount > 0) return;

    // Last reference dropped — free all physical pages.
    for (uint32_t i = 0; i < shm->npages; i++) {
        if (shm->pages[i])
            pmm_buddy_free(shm->pages[i], 0);
    }

    // Remove from namespace if named.
    if (shm->name[0])
        shmem_ns_remove(shm);

    kfree(shm->pages);
    kfree(shm);
}

// ── shmem_get_page ───────────────────────────────────────────────────────

phys_addr_t shmem_get_page(shmem_t* shm, uint32_t pg_idx) {
    if (!shm || pg_idx >= shm->npages)
        return PMM_INVALID_ADDR;

    if (shm->pages[pg_idx])
        return shm->pages[pg_idx]; // already allocated

    // First touch: allocate a zeroed physical frame.
    phys_addr_t frame = pmm_buddy_alloc(0);
    if (frame == PMM_INVALID_ADDR)
        return PMM_INVALID_ADDR;

    // Zero the frame (security: never expose stale data).
    uint64_t* p = (uint64_t*)(frame + HHDM_OFFSET);
    for (int i = 0; i < 512; i++) p[i] = 0;

    shm->pages[pg_idx] = frame;
    return frame;
}

// ── shmem_resize ─────────────────────────────────────────────────────────

int shmem_resize(shmem_t* shm, uint32_t new_npages) {
    if (!shm) return -EINVAL;
    if (new_npages > SHMEM_MAX_PAGES) return -EINVAL;
    if (new_npages == shm->npages) return 0;

    if (new_npages < shm->npages) {
        // Shrinking: free pages beyond the new size.
        for (uint32_t i = new_npages; i < shm->npages; i++) {
            if (shm->pages[i]) {
                pmm_buddy_free(shm->pages[i], 0);
                shm->pages[i] = 0;
            }
        }
        shm->npages = new_npages;
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
        if (new_cap < new_npages) return -EINVAL;

        phys_addr_t* new_arr = kmalloc(new_cap * sizeof(phys_addr_t));
        if (!new_arr) return -ENOMEM;

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
    return 0;
}

// ── shmem_ns_find ────────────────────────────────────────────────────────

shmem_t* shmem_ns_find(const char* name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < SHMEM_NS_MAX; i++) {
        if (s_namespace[i].shm && s_streq(s_namespace[i].shm->name, name))
            return s_namespace[i].shm;
    }
    return NULL;
}

// ── shmem_ns_insert ──────────────────────────────────────────────────────

int shmem_ns_insert(shmem_t* shm) {
    if (!shm || !shm->name[0]) return -EINVAL;

    // Check for duplicate name.
    if (shmem_ns_find(shm->name)) return -EEXIST;

    // Find an empty slot.
    for (int i = 0; i < SHMEM_NS_MAX; i++) {
        if (!s_namespace[i].shm) {
            s_namespace[i].shm = shm;
            return 0;
        }
    }
    return -ENOSPC;
}

// ── shmem_ns_remove ──────────────────────────────────────────────────────

void shmem_ns_remove(shmem_t* shm) {
    if (!shm) return;
    for (int i = 0; i < SHMEM_NS_MAX; i++) {
        if (s_namespace[i].shm == shm) {
            s_namespace[i].shm = NULL;
            return;
        }
    }
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

    f->read     = shmem_fd_read;
    f->write    = shmem_fd_write;
    f->close    = shmem_fd_close;
    f->seek     = NULL;
    f->poll        = NULL;
    f->ioctl       = NULL;
    f->ctx         = shm;
    f->poll_waiter = NULL;
    f->flags       = 0;
    f->refcount    = 1;
    f->rights   = 0xFFFFFFFF; // all rights — narrowed by restrict_fd if needed
    f->path[0]  = '\0';

    shmem_ref(shm); // fd owns a reference
    return f;
}
