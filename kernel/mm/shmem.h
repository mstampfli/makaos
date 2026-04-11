#pragma once
#include "common.h"
#include "vmm.h"

// ── Shared Memory Objects ────────────────────────────────────────────────
//
// A shmem_t is a kernel-side backing store for MAP_SHARED mappings and
// POSIX shared memory (shm_open).  It holds an array of physical page
// frame addresses that can be mapped into multiple address spaces
// simultaneously.
//
// Design principles:
//   - Refcounted: each VMA and each open fd increments the refcount.
//     The object and its pages are freed when the last reference drops.
//   - Demand-paged: pages are allocated on first access (#PF handler
//     calls shmem_get_page, which allocates and zeros on first touch).
//   - Resizable: ftruncate can grow or shrink the object.  Shrinking
//     unmaps and frees pages beyond the new size.
//   - Named objects live in a flat kernel namespace ("/shm/<name>")
//     with POSIX creat/open semantics and permission checks.
//   - Security: each object carries uid/gid/mode.  shm_open checks
//     these against the caller's credentials before granting access.

// Maximum pages per shmem object (256 MB at 4K pages — raised if needed).
#define SHMEM_MAX_PAGES  (256UL * 1024 * 1024 / PAGE_SIZE)

// Maximum named shmem objects in the kernel namespace.
#define SHMEM_NS_MAX     64

// Maximum name length (excluding NUL terminator).
#define SHMEM_NAME_MAX   63

typedef struct shmem {
    phys_addr_t* pages;       // array of physical frame addresses (0 = not yet faulted)
    uint32_t     npages;      // current capacity (set by ftruncate / create)
    uint32_t     max_pages;   // allocated length of pages[] array
    uint32_t     refcount;    // VMA refs + fd refs; freed at 0

    // Ownership and permissions (POSIX shared memory semantics).
    uint32_t     uid;
    uint32_t     gid;
    uint16_t     mode;        // permission bits (e.g. 0600)

    // Named object identity (empty string = anonymous).
    char         name[SHMEM_NAME_MAX + 1];
} shmem_t;


// ── Lifecycle ────────────────────────────────────────────────────────────

// Create an anonymous shmem object with `npages` capacity.
// All page slots are initialized to 0 (not yet allocated).
// refcount starts at 1 (caller owns the initial reference).
// Returns NULL on OOM.
shmem_t* shmem_create(uint32_t npages, uint32_t uid, uint32_t gid, uint16_t mode);

// Increment the reference count.
void shmem_ref(shmem_t* shm);

// Decrement the reference count.  When it reaches 0, all physical
// pages are freed and the shmem_t itself is freed.  If the object
// is in the named namespace, it is removed.
void shmem_unref(shmem_t* shm);

// ── Page access ──────────────────────────────────────────────────────────

// Get the physical frame for page index `pg_idx` within the object.
// If the page has not been allocated yet, allocates a zeroed frame.
// Returns the physical address, or PMM_INVALID_ADDR on OOM or out-of-range.
phys_addr_t shmem_get_page(shmem_t* shm, uint32_t pg_idx);

// ── Resize ───────────────────────────────────────────────────────────────

// Resize the object to `new_npages`.  If shrinking, pages beyond the
// new size are freed.  If growing, new slots are zero-initialized
// (demand-paged on access).
// Returns 0 on success, -ENOMEM on allocation failure, -EINVAL on bad size.
int shmem_resize(shmem_t* shm, uint32_t new_npages);

// ── Named namespace ──────────────────────────────────────────────────────
// Forward-declare cred_t to avoid circular include with cred.h.
struct cred_t;

// Look up a named shmem object.  Returns NULL if not found.
shmem_t* shmem_ns_find(const char* name);

// Insert a named shmem object into the namespace.
// Returns 0 on success, -EEXIST if name already taken, -ENOSPC if table full.
int shmem_ns_insert(shmem_t* shm);

// Remove a named shmem object from the namespace (called by shmem_unref
// when the last reference drops, or by shm_unlink).
void shmem_ns_remove(shmem_t* shm);

// ── fd-level interface ──────────────────────────────────────────────────
// Wrap a shmem_t in a vfs_file_t for use with mmap(fd), ftruncate, close.
// The file description's ->ctx is the shmem_t*, and ->close is shmem_fd_close.
// sys_mmap identifies shmem fds by comparing f->close == shmem_fd_close.

#include "vfs.h"

// Create a vfs_file_t wrapping `shm`.  Bumps shm->refcount (fd owns a ref).
// Returns NULL on OOM.
vfs_file_t* shmem_fd_create(shmem_t* shm);

// VFS close callback — drops the fd's reference on the shmem_t.
void shmem_fd_close(vfs_file_t* self);

// ── Permission check ────────────────────────────────────────────────────
// Check if credentials allow the requested access to a shmem object.
// `oflags` is a subset of O_RDONLY/O_WRONLY/O_RDWR.
// Returns 0 if allowed, -EACCES if denied.
#include "cred.h"
int shmem_check_access(const shmem_t* shm, const cred_t* cred, int oflags);
