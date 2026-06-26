// ── /proc virtual filesystem ──────────────────────────────────────────────
//
// Architecture:
//
//   proc_open() dispatches on path pattern to a handler function.
//   Each handler synthesizes content into a heap buffer, then wraps it in a
//   membuf_vfs_file_t — a vfs_file_t backed by that buffer with a seek
//   pointer so repeated reads and lseek work correctly.
//
//   Per-process files are registered in s_proc_pid_files[].  Adding a new
//   file (e.g. /proc/<pid>/environ) means adding one entry to that table —
//   no other code changes needed.
//
//   Top-level synthetic files (e.g. /proc/meminfo) are registered in
//   s_proc_root_files[].

#include "proc.h"
#include "vfs.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "mm.h"
#include "common.h"
#include "rcu.h"

// ── Utility: integer → decimal string ────────────────────────────────────

static int uint_to_str(uint64_t v, char* buf, int bufsize) {
    if (bufsize < 2) return 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[24]; int n = 0;
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i;
    for (i = 0; i < n && i < bufsize - 1; i++) buf[i] = tmp[n - 1 - i];
    buf[i] = '\0';
    return i;
}

static uint32_t str_to_uint(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static uint64_t str_len(const char* s) {
    uint64_t n = 0; while (s[n]) n++; return n;
}

// ── membuf: a synthesized read-only vfs_file_t backed by a heap buffer ───
//
// Used for all /proc files: the handler fills a buffer, membuf_open wraps it.
// seek() works so tools like 'cat' (which may call lseek) function correctly.
// close() frees both the buffer and the vfs_file_t itself.

typedef struct {
    uint8_t* buf;
    uint64_t size;
    uint64_t pos;
} membuf_ctx_t;

static int64_t membuf_read(vfs_file_t* self, void* dst, uint64_t len) {
    membuf_ctx_t* ctx = (membuf_ctx_t*)self->ctx;
    if (ctx->pos >= ctx->size) return 0; // EOF
    uint64_t avail = ctx->size - ctx->pos;
    if (len > avail) len = avail;
    __builtin_memcpy(dst, ctx->buf + ctx->pos, len);
    ctx->pos += len;
    return (int64_t)len;
}

static int64_t membuf_seek(vfs_file_t* self, int64_t offset, int whence) {
    membuf_ctx_t* ctx = (membuf_ctx_t*)self->ctx;
    int64_t newpos;
    if      (whence == 0 /*SEEK_SET*/) newpos = offset;
    else if (whence == 1 /*SEEK_CUR*/) newpos = (int64_t)ctx->pos + offset;
    else if (whence == 2 /*SEEK_END*/) newpos = (int64_t)ctx->size + offset;
    else return -1;
    if (newpos < 0) newpos = 0;
    ctx->pos = (uint64_t)newpos;
    return newpos;
}

static void membuf_close(vfs_file_t* self) {
    membuf_ctx_t* ctx = (membuf_ctx_t*)self->ctx;
    kfree(ctx->buf);
    kfree(ctx);
    kfree(self);
}

// Wrap a heap buffer in a vfs_file_t.  Takes ownership of buf.
// Returns NULL on allocation failure (buf is NOT freed in that case —
// caller must free it).
static vfs_file_t* membuf_open(uint8_t* buf, uint64_t size) {
    membuf_ctx_t* ctx = kmalloc(sizeof(membuf_ctx_t));
    if (!ctx) return NULL;
    ctx->buf  = buf;
    ctx->size = size;
    ctx->pos  = 0;

    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(ctx); return NULL; }
    __builtin_memset(f, 0, sizeof(*f));
    f->read     = membuf_read;
    f->write    = NULL;  // read-only
    f->close    = membuf_close;
    f->seek     = membuf_seek;
    f->poll           = NULL;
    f->ioctl          = NULL;
    f->ctx            = ctx;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags          = 0;
    f->refcount    = 1;
    f->rights   = 0;
    f->path[0]  = '\0';
    return f;
}

// ── Dynamic string builder ────────────────────────────────────────────────
// Builds a NUL-terminated string in a heap buffer, growing as needed.
// Used by all content generators to avoid fixed-size buffers.

#define STRBUF_INIT_CAP 512

typedef struct {
    char*    data;
    uint64_t len;
    uint64_t cap;
} strbuf_t;

static int strbuf_init(strbuf_t* b) {
    b->data = kmalloc(STRBUF_INIT_CAP);
    if (!b->data) return 0;
    b->len = 0;
    b->cap = STRBUF_INIT_CAP;
    b->data[0] = '\0';
    return 1;
}

static int strbuf_grow(strbuf_t* b, uint64_t need) {
    if (b->len + need + 1 <= b->cap) return 1;
    uint64_t newcap = b->cap * 2;
    while (newcap < b->len + need + 1) newcap *= 2;
    char* newdata = kmalloc(newcap);
    if (!newdata) return 0;
    __builtin_memcpy(newdata, b->data, b->len);
    newdata[b->len] = '\0';
    kfree(b->data);
    b->data = newdata;
    b->cap  = newcap;
    return 1;
}

static int strbuf_append(strbuf_t* b, const char* s) {
    uint64_t l = str_len(s);
    if (!strbuf_grow(b, l)) return 0;
    for (uint64_t i = 0; i < l; i++) b->data[b->len + i] = s[i];
    b->len += l;
    b->data[b->len] = '\0';
    return 1;
}

static int strbuf_append_uint(strbuf_t* b, uint64_t v) {
    char tmp[24];
    uint_to_str(v, tmp, sizeof(tmp));
    return strbuf_append(b, tmp);
}

static int strbuf_append_hex(strbuf_t* b, uint64_t v) {
    // 16 hex digits + NUL
    char tmp[17];
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        tmp[i] = hex[v & 0xF];
        v >>= 4;
    }
    tmp[16] = '\0';
    return strbuf_append(b, tmp);
}

// ── Content generators ────────────────────────────────────────────────────

// /proc/<pid>/status
static uint8_t* gen_status(task_t* t, uint64_t* out_size) {
    static const char* state_names[] = {
        [TASK_READY]    = "runnable",
        [TASK_RUNNING]  = "running",
        [TASK_DEAD]     = "dead",
        [TASK_SLEEPING] = "sleeping",
        [TASK_ZOMBIE]   = "zombie",
    };
    strbuf_t b;
    if (!strbuf_init(&b)) return NULL;

    strbuf_append(&b, "Name:\t");
    strbuf_append(&b, t->comm[0] ? t->comm : "(unknown)");
    strbuf_append(&b, "\nPid:\t");  strbuf_append_uint(&b, t->pid);
    strbuf_append(&b, "\nTgid:\t"); strbuf_append_uint(&b, t->tgid);
    strbuf_append(&b, "\nPPid:\t"); strbuf_append_uint(&b, t->ppid);
    strbuf_append(&b, "\nPgid:\t"); strbuf_append_uint(&b, t->pgid);
    strbuf_append(&b, "\nSid:\t");  strbuf_append_uint(&b, t->sid);
    strbuf_append(&b, "\nState:\t");
    uint32_t st = (uint32_t)t->state;
    if (st < 5) strbuf_append(&b, state_names[st]);
    else        strbuf_append(&b, "unknown");
    strbuf_append(&b, "\nUmask:\t"); strbuf_append_uint(&b, t->umask);
    strbuf_append(&b, "\nVmRSS:\t"); // placeholder — real page counting is future work
    strbuf_append(&b, "0 kB");
    strbuf_append(&b, "\n");

    *out_size = b.len;
    return (uint8_t*)b.data;
}

// /proc/<pid>/cmdline
static uint8_t* gen_cmdline(task_t* t, uint64_t* out_size) {
    // Linux format: argv[0]\0argv[1]\0... with no trailing newline.
    // We only store comm (argv[0] basename) for now.
    const char* name = t->comm[0] ? t->comm : "";
    uint64_t len = str_len(name) + 1; // include NUL
    uint8_t* buf = kmalloc(len ? len : 1);
    if (!buf) return NULL;
    __builtin_memcpy(buf, name, len);
    *out_size = len;
    return buf;
}

// /proc/<pid>/maps  — one line per VMA:  addr_start-addr_end perms
static uint8_t* gen_maps(task_t* t, uint64_t* out_size) {
    strbuf_t b;
    if (!strbuf_init(&b)) return NULL;

    mm_t* mm = t->mm_shared ? t->mm_shared->mm : NULL;
    if (mm) {
        // RCU walk: readers take zero locks.  strbuf_append may kmalloc
        // on growth — that's non-sleeping in this kernel so it's safe
        // to keep inside the reader section.
        rcu_read_lock();
        for (vma_t* v = rcu_dereference(mm->vmas); v;
                     v = rcu_dereference(v->next)) {
            strbuf_append_hex(&b, v->start);
            strbuf_append(&b, "-");
            strbuf_append_hex(&b, v->end);
            strbuf_append(&b, " ");
            strbuf_append(&b, (v->flags & VMA_R) ? "r" : "-");
            strbuf_append(&b, (v->flags & VMA_W) ? "w" : "-");
            strbuf_append(&b, (v->flags & VMA_X) ? "x" : "-");
            strbuf_append(&b, "p\n");
        }
        rcu_read_unlock();
    } else {
        strbuf_append(&b, "(no address space)\n");
    }

    *out_size = b.len;
    return (uint8_t*)b.data;
}

// ── Per-process file table ────────────────────────────────────────────────
// Each entry maps a path suffix (relative to /proc/<pid>/) to a generator.
// Adding a new file: add one row here. Zero other changes needed.

typedef uint8_t* (*pid_file_gen_t)(task_t* t, uint64_t* out_size);

typedef struct {
    const char*    suffix;   // e.g. "status", "cmdline", "maps"
    pid_file_gen_t gen;
} proc_pid_file_t;

static const proc_pid_file_t s_proc_pid_files[] = {
    { "status",  gen_status  },
    { "cmdline", gen_cmdline },
    { "maps",    gen_maps    },
    { NULL, NULL }
};

// ── /proc/<pid>/fd/ handling ──────────────────────────────────────────────

// Open /proc/<pid>/fd/<n> — returns a dup of the target task's fd n.
// Cross-process: snapshot the target's fdtable_t under RCU, tryget the
// file so a concurrent close on the target can't free it under us.
static vfs_file_t* proc_fd_open(task_t* t, const char* fd_str) {
    uint32_t n = str_to_uint(fd_str);
    vfs_file_t* f = NULL;
    rcu_read_lock();
    // Snapshot files_shared ONCE under rcu: task_files_release RCU-defers the
    // table struct free, so a non-NULL snapshot stays valid for this whole
    // section (and a single load avoids a NULL-deref race vs a concurrent exit
    // that unpublishes files_shared).
    task_files_t* tf = __atomic_load_n(&t->files_shared, __ATOMIC_ACQUIRE);
    fdtable_t* ft = tf ? __atomic_load_n(&tf->ft, __ATOMIC_ACQUIRE) : NULL;
    if (ft && n < ft->cap)
        f = vfs_tryget(__atomic_load_n(&ft->fd_table[n], __ATOMIC_ACQUIRE));
    rcu_read_unlock();
    return f;  // +1 ref from tryget, or NULL
}

// Readdir /proc/<pid>/fd/ — list open fd numbers as entry names.
static int proc_fd_readdir(task_t* t, ext2_entry_t* out, int max) {
    int count = 0;
    rcu_read_lock();
    // Snapshot files_shared ONCE under rcu (task_files_release RCU-defers the
    // table free; a single load avoids a NULL-deref race vs a concurrent exit).
    task_files_t* tf = __atomic_load_n(&t->files_shared, __ATOMIC_ACQUIRE);
    fdtable_t* ft = tf ? __atomic_load_n(&tf->ft, __ATOMIC_ACQUIRE) : NULL;
    if (!ft) { rcu_read_unlock(); return 0; }
    for (uint32_t i = 0; i < ft->cap && count < max; i++) {
        if (!ft->fd_table[i]) continue;
        char name[24];
        uint_to_str(i, name, sizeof(name));
        uint32_t nlen = 0; while (name[nlen]) nlen++;
        for (uint32_t j = 0; j <= nlen; j++) out[count].name[j] = name[j];
        out[count].inode_num = i + 1000; // synthetic inode
        out[count].size      = 0;
        out[count].is_dir    = 0;
        count++;
    }
    rcu_read_unlock();
    return count;
}

// ── /proc/<pid>/ readdir ──────────────────────────────────────────────────

static int proc_pid_readdir(task_t* t, ext2_entry_t* out, int max) {
    (void)t;
    int count = 0;
    // Static entries for every process.
    static const char* names[] = { "status", "cmdline", "maps", "fd", NULL };
    for (int i = 0; names[i] && count < max; i++) {
        const char* n = names[i];
        uint32_t j = 0;
        while (n[j]) { out[count].name[j] = n[j]; j++; }
        out[count].name[j] = '\0';
        out[count].inode_num = (uint32_t)(0x70000000 + count);
        out[count].size      = 0;
        out[count].is_dir    = (n[0] == 'f' && n[1] == 'd'); // "fd" is a dir
        count++;
    }
    return count;
}

// ── /proc/ root readdir ───────────────────────────────────────────────────

typedef struct {
    ext2_entry_t* out;
    int           max;
    int           count;
} proc_root_rd_ctx_t;

static void proc_root_rd_visit(task_t* t, void* data) {
    proc_root_rd_ctx_t* ctx = (proc_root_rd_ctx_t*)data;
    if (ctx->count >= ctx->max) return;
    char name[24];
    uint_to_str(t->pid, name, sizeof(name));
    uint32_t j = 0;
    while (name[j]) { ctx->out[ctx->count].name[j] = name[j]; j++; }
    ctx->out[ctx->count].name[j]     = '\0';
    ctx->out[ctx->count].inode_num   = t->pid;
    ctx->out[ctx->count].size        = 0;
    ctx->out[ctx->count].is_dir      = 1;
    ctx->count++;
}

static int proc_root_readdir(ext2_entry_t* out, int max) {
    int count = 0;

    // First entry: "self" (symlink-like — we just report it as a dir).
    if (count < max) {
        out[count].name[0] = 's'; out[count].name[1] = 'e';
        out[count].name[2] = 'l'; out[count].name[3] = 'f';
        out[count].name[4] = '\0';
        out[count].inode_num = 0xFFFF;
        out[count].size      = 0;
        out[count].is_dir    = 1;
        count++;
    }

    // One entry per live task.
    proc_root_rd_ctx_t ctx = { out + count, max - count, 0 };
    sched_for_each(proc_root_rd_visit, &ctx);
    count += ctx.count;
    return count;
}

// ── Path parser helpers ───────────────────────────────────────────────────
// Path arrives as "/proc/..." — strip the prefix and parse.

// Skip leading "/proc" and return pointer to the rest (starting with '/' or '\0').
static const char* proc_strip_prefix(const char* path) {
    // path starts with "/proc"
    const char* p = path + 5; // skip "/proc"
    return p;
}

// Parse "/proc/self" → resolve to current pid string.
// Returns 1 if path starts with "/proc/self" and optionally sets *rest to
// the part after "/proc/self" (e.g. "/status").
static int proc_is_self(const char* rest, const char** after) {
    // rest starts after "/proc", so check for "/self"
    if (rest[0] != '/') return 0;
    if (rest[1]=='s' && rest[2]=='e' && rest[3]=='l' && rest[4]=='f'
        && (rest[5]=='/' || rest[5]=='\0')) {
        if (after) *after = rest + 5;
        return 1;
    }
    return 0;
}

// Parse "/proc/<pid>[/...]" → returns pid (0 if not a valid pid path).
// Sets *after to the part after the pid (e.g. "/status" or "").
static uint32_t proc_parse_pid(const char* rest, const char** after) {
    if (rest[0] != '/') return 0;
    const char* p = rest + 1;
    if (*p < '0' || *p > '9') return 0;
    uint32_t pid = 0;
    while (*p >= '0' && *p <= '9') { pid = pid * 10 + (uint32_t)(*p - '0'); p++; }
    if (*p != '/' && *p != '\0') return 0;
    if (after) *after = p;
    return pid;
}

// ── Public API ────────────────────────────────────────────────────────────

vfs_file_t* proc_open(const char* path) {
    const char* rest = proc_strip_prefix(path); // rest = "" | "/" | "/self/..." | "/<pid>/..."

    // "/proc" or "/proc/" — not a file, it's a directory.
    if (rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0')) return NULL;

    // Resolve /proc/self → /proc/<current-pid>.
    const char* after_pid = NULL;
    uint32_t pid = 0;

    if (proc_is_self(rest, &after_pid)) {
        pid = g_current ? g_current->pid : 0;
    } else {
        pid = proc_parse_pid(rest, &after_pid);
    }

    if (!pid) return NULL;

    // Hold rcu_read_lock across the lookup AND every dereference of `t`
    // (proc_fd_open / the gen() snapshot) so a concurrent exit+task_destroy
    // (RCU-deferred free via task_free_rcu) cannot free the task mid-use.  The
    // generators snapshot into a heap buffer and proc_fd_open takes its own fd
    // reference, so nothing referencing `t` outlives the section.  Single exit
    // (open_out) guarantees the section is never leaked.
    rcu_read_lock();
    task_t* t = sched_find_pid(pid);
    vfs_file_t* result = NULL;
    if (!t) goto open_out;

    // after_pid: "" → /proc/<pid>/ directory (not openable as file)
    //            "/status", "/cmdline", "/maps" → file generators
    //            "/fd" → directory
    //            "/fd/<n>" → dup of fd n
    if (!after_pid || after_pid[0] == '\0') goto open_out; // directory -> NULL

    {
        const char* sub = after_pid + 1; // skip leading '/'

        // /proc/<pid>/fd/<n>
        if (sub[0]=='f' && sub[1]=='d' && sub[2]=='/') {
            result = proc_fd_open(t, sub + 3);
            goto open_out;
        }

        // /proc/<pid>/fd  (directory — not a file)
        if (sub[0]=='f' && sub[1]=='d' && sub[2]=='\0') goto open_out;

        // /proc/<pid>/<known file>
        for (int i = 0; s_proc_pid_files[i].suffix; i++) {
            if (str_eq(sub, s_proc_pid_files[i].suffix)) {
                uint64_t size = 0;
                uint8_t* buf = s_proc_pid_files[i].gen(t, &size);
                if (!buf) goto open_out;
                result = membuf_open(buf, size);
                if (!result) kfree(buf);
                goto open_out;
            }
        }
    }
open_out:
    rcu_read_unlock();
    return result;
}

int proc_readdir(const char* path, ext2_entry_t* out, int max) {
    const char* rest = proc_strip_prefix(path);

    // "/proc" or "/proc/"
    if (rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'))
        return proc_root_readdir(out, max);

    const char* after_pid = NULL;
    uint32_t pid = 0;

    if (proc_is_self(rest, &after_pid)) {
        pid = g_current ? g_current->pid : 0;
    } else {
        pid = proc_parse_pid(rest, &after_pid);
    }

    if (!pid) return -1;

    // rcu_read_lock spans the lookup and the readdir walk of `t` (which fills
    // the caller's `out` array and does not store `t`) so a concurrent exit
    // cannot free the task mid-walk.  Single exit (rd_out).
    rcu_read_lock();
    task_t* t = sched_find_pid(pid);
    int rc = -1;
    if (!t) goto rd_out;

    // /proc/<pid>  or  /proc/<pid>/
    if (!after_pid || after_pid[0] == '\0' ||
        (after_pid[0] == '/' && after_pid[1] == '\0')) {
        rc = proc_pid_readdir(t, out, max);
        goto rd_out;
    }

    {
        const char* sub = after_pid + 1;

        // /proc/<pid>/fd/
        if (sub[0]=='f' && sub[1]=='d' && (sub[2]=='/' || sub[2]=='\0'))
            rc = proc_fd_readdir(t, out, max);
    }
rd_out:
    rcu_read_unlock();
    return rc;
}

int proc_stat(const char* path, struct stat* out) {
    const char* rest = proc_strip_prefix(path);

    // "/proc" itself
    if (rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0')) {
        out->st_ino    = 0x10000;
        out->st_size   = 0;
        out->st_mode   = 0x41ED; // S_IFDIR | 0755
        out->st_nlink  = 2;
        out->st_blksize= 4096;
        return 0;
    }

    const char* after_pid = NULL;
    uint32_t pid = 0;

    if (proc_is_self(rest, &after_pid)) {
        pid = g_current ? g_current->pid : 0;
    } else {
        pid = proc_parse_pid(rest, &after_pid);
    }

    if (!pid) return -1;

    // rcu_read_lock spans the lookup, the `!t` liveness gate, and the only
    // dereference of `t` (the fd/<n> branch reads t->files_shared) so a
    // concurrent exit cannot free the task in the window.  Single exit
    // (stat_out).  Branches that touch only `pid` still run under the section,
    // which is harmless (no sleep).
    rcu_read_lock();
    task_t* t = sched_find_pid(pid);
    int rc = -1;
    if (!t) goto stat_out;

    // /proc/<pid>  (directory)
    if (!after_pid || after_pid[0] == '\0' ||
        (after_pid[0] == '/' && after_pid[1] == '\0')) {
        out->st_ino    = pid;
        out->st_size   = 0;
        out->st_mode   = 0x41ED; // S_IFDIR | 0755
        out->st_nlink  = 2;
        out->st_blksize= 4096;
        rc = 0; goto stat_out;
    }

    {
        const char* sub = after_pid + 1;

        // /proc/<pid>/fd  or  /proc/<pid>/fd/
        if (sub[0]=='f' && sub[1]=='d' && (sub[2]=='/' || sub[2]=='\0')) {
            out->st_ino    = pid + 0x20000;
            out->st_size   = 0;
            out->st_mode   = 0x41ED; // S_IFDIR | 0755
            out->st_nlink  = 2;
            out->st_blksize= 4096;
            rc = 0; goto stat_out;
        }

        // /proc/<pid>/fd/<n>
        if (sub[0]=='f' && sub[1]=='d' && sub[2]=='/') {
            uint32_t n = str_to_uint(sub + 3);
            task_files_t* tf = __atomic_load_n(&t->files_shared, __ATOMIC_ACQUIRE);
            fdtable_t* ft = tf ? __atomic_load_n(&tf->ft, __ATOMIC_ACQUIRE) : NULL;
            if (!ft || n >= ft->cap || !ft->fd_table[n]) { rc = -1; goto stat_out; }
            out->st_ino    = pid + 0x30000 + n;
            out->st_size   = 0;
            out->st_mode   = 0x81A4; // S_IFREG | 0644
            out->st_nlink  = 1;
            out->st_blksize= 4096;
            rc = 0; goto stat_out;
        }

        // /proc/<pid>/<known file>
        for (int i = 0; s_proc_pid_files[i].suffix; i++) {
            if (str_eq(sub, s_proc_pid_files[i].suffix)) {
                out->st_ino    = pid + 0x10000 + (uint32_t)i;
                out->st_size   = 0; // synthesized — size unknown without generating
                out->st_mode   = 0x81A4; // S_IFREG | 0644
                out->st_nlink  = 1;
                out->st_blksize= 4096;
                rc = 0; goto stat_out;
            }
        }
    }
stat_out:
    rcu_read_unlock();
    return rc;
}
