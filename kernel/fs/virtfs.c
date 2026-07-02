#include "virtfs.h"
#include "kprintf.h"   // kprintf_atomic (locked whole-line output for selftest result lines)
#include "acl.h"
#include "errno.h"
#include "kstr.h"    // str_eq (shared string utils; was local s_streq)
#include "ext2.h"
#include "sched.h"
#include "rcu.h"     // rcu_read_lock around sched_find_pid

// ── Per-node device table ────────────────────────────────────────────────
// Single source of truth for all /dev nodes: name, ownership, permissions.

typedef struct {
    const char* name;
    uint32_t    uid;
    uint32_t    gid;
    uint16_t    mode;    // permission bits only (no type bits)
    int         type;    // FS_TYPE_CHAR etc.
} dev_node_t;

static const dev_node_t s_dev_nodes[] = {
    { "tty",     0, 0, 0666, FS_TYPE_CHAR },
    { "tty0",    0, 0, 0620, FS_TYPE_CHAR },  // owner+group rw, no other
    { "kbd",     0, 0, 0660, FS_TYPE_CHAR },
    { "kbdraw",  0, 0, 0660, FS_TYPE_CHAR },
    { "vga",     0, 0, 0660, FS_TYPE_CHAR },
    { "mouse",   0, 0, 0666, FS_TYPE_CHAR },
    { "dsp",     0, 0, 0666, FS_TYPE_CHAR },
    { "null",    0, 0, 0666, FS_TYPE_CHAR },
    { "zero",    0, 0, 0666, FS_TYPE_CHAR },
    { "urandom", 0, 0, 0444, FS_TYPE_CHAR },  // read-only
    { "input",         0, 0, 0755, FS_TYPE_DIR  },
    { "input/event0",  0, 0, 0660, FS_TYPE_CHAR },
    { "input/event1",  0, 0, 0660, FS_TYPE_CHAR },
    { "input/event2",  0, 0, 0660, FS_TYPE_CHAR },
    { "dri",           0, 0, 0755, FS_TYPE_DIR  },
    { "dri/card0",     0, 0, 0660, FS_TYPE_CHAR },
    { "ptmx",          0, 0, 0666, FS_TYPE_CHAR },
    { "pts",           0, 0, 0755, FS_TYPE_DIR  },
    { NULL, 0, 0, 0, 0 }
};

// Template returned for /dev/pts/<N> — slave indices are dynamic, so
// they can't be rows in the static table.  Mode 0620 matches Linux
// (owner rw, tty-group w).
static const dev_node_t s_pts_slave_node = { "pts/N", 0, 0, 0620, FS_TYPE_CHAR };

static const dev_node_t* dev_find_node(const char* name) {
    for (int i = 0; s_dev_nodes[i].name; i++) {
        const char* a = s_dev_nodes[i].name;
        const char* b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return &s_dev_nodes[i];
    }
    // pts/<digits> — any live slave index.  Existence of the pty is
    // checked at open time (pty_open_slave_by_index); lookups on a
    // stale N just report metadata for a node whose open returns
    // ENOENT, same as Linux devpts after the master closes.
    if (name[0]=='p' && name[1]=='t' && name[2]=='s' && name[3]=='/'
        && name[4] >= '0' && name[4] <= '9') {
        int i = 4;
        while (name[i] >= '0' && name[i] <= '9') i++;
        if (name[i] == '\0') return &s_pts_slave_node;
    }
    return NULL;
}

// ── Virtual mount table ──────────────────────────────────────────────────
// Mount-level metadata for the directory nodes themselves (/dev, /proc).

typedef struct {
    const char* prefix;
    uint8_t     prefix_len;
    uint32_t    dir_uid;
    uint32_t    dir_gid;
    uint16_t    dir_mode;
} virtmount_t;

static const virtmount_t s_mounts[] = {
    { "/proc", 5,  0, 0, 0555 },
    { "/dev",  4,  0, 0, 0755 },
};
#define N_MOUNTS ((int)(sizeof(s_mounts)/sizeof(s_mounts[0])))

// ── Internal helpers ─────────────────────────────────────────────────────

static const virtmount_t* find_mount(const char* path) {
    for (int i = 0; i < N_MOUNTS; i++) {
        const virtmount_t* m = &s_mounts[i];
        int n = (int)m->prefix_len;
        int match = 1;
        for (int j = 0; j < n; j++) {
            if (path[j] != m->prefix[j]) { match = 0; break; }
        }
        if (match && (path[n] == '/' || path[n] == '\0'))
            return m;
    }
    return NULL;
}

// Resolve per-node uid/gid/mode/type for any virtual path.
// For directory-level paths (/dev, /proc), uses mount defaults.
// For nodes inside, looks up per-node metadata.
static int virt_resolve(const char* path, const virtmount_t* m,
                        uint32_t* out_uid, uint32_t* out_gid,
                        uint16_t* out_mode, int* out_type) {
    int n = (int)m->prefix_len;
    int is_dir = (path[n] == '\0');

    if (is_dir) {
        // Mount root directory itself
        *out_uid  = m->dir_uid;
        *out_gid  = m->dir_gid;
        *out_mode = m->dir_mode;
        *out_type = FS_TYPE_DIR;
        return 0;
    }

    // path[n] == '/', node name starts at path[n+1]
    const char* node_name = path + n + 1;
    if (node_name[0] == '\0') {
        // Trailing slash: still the directory
        *out_uid  = m->dir_uid;
        *out_gid  = m->dir_gid;
        *out_mode = m->dir_mode;
        *out_type = FS_TYPE_DIR;
        return 0;
    }

    // /dev/<node>
    if (str_eq(m->prefix, "/dev")) {
        const dev_node_t* dn = dev_find_node(node_name);
        if (!dn) return -ENOENT;
        *out_uid  = dn->uid;
        *out_gid  = dn->gid;
        *out_mode = dn->mode;
        *out_type = dn->type;
        return 0;
    }

    // /proc — per-node permissions based on what's being accessed
    if (str_eq(m->prefix, "/proc")) {
        // /proc/self, /proc/<pid> — directories owned by the process
        // /proc/<pid>/<file> — files owned by the process
        // We need to figure out the pid to get its uid.

        // Skip to the first component after /proc/
        const char* p = node_name;

        // Check for "self"
        int is_self = (p[0]=='s' && p[1]=='e' && p[2]=='l' && p[3]=='f'
                       && (p[4]=='/' || p[4]=='\0'));

        uint32_t pid = 0;
        const char* after = p;

        if (is_self) {
            pid = g_current ? g_current->pid : 0;
            after = p + 4;
        } else {
            // Parse numeric pid
            while (*after >= '0' && *after <= '9') {
                pid = pid * 10 + (uint32_t)(*after - '0');
                after++;
            }
        }

        if (pid) {
            // rcu_read_lock keeps the looked-up task alive across the cred
            // reads: sched_find_pid returns a bare pointer that a concurrent
            // exit+task_destroy (RCU-deferred free) could otherwise free.
            rcu_read_lock();
            task_t* t = sched_find_pid(pid);
            uint32_t owner_uid = t ? t->cred.euid : 0;
            uint32_t owner_gid = t ? t->cred.egid : 0;
            rcu_read_unlock();

            if (after[0] == '\0' || (after[0] == '/' && after[1] == '\0')) {
                // /proc/<pid> directory
                *out_uid  = owner_uid;
                *out_gid  = owner_gid;
                *out_mode = 0555;
                *out_type = FS_TYPE_DIR;
                return 0;
            }
            // /proc/<pid>/fd is a directory
            if (after[0] == '/' && after[1] == 'f' && after[2] == 'd'
                && (after[3] == '\0' || after[3] == '/')) {
                if (after[3] == '\0' || (after[3] == '/' && after[4] == '\0')) {
                    *out_uid  = owner_uid;
                    *out_gid  = owner_gid;
                    *out_mode = 0500;
                    *out_type = FS_TYPE_DIR;
                    return 0;
                }
                // /proc/<pid>/fd/<n> — file
                *out_uid  = owner_uid;
                *out_gid  = owner_gid;
                *out_mode = 0400;
                *out_type = FS_TYPE_FILE;
                return 0;
            }
            // /proc/<pid>/<file> (status, cmdline, maps, etc.)
            *out_uid  = owner_uid;
            *out_gid  = owner_gid;
            *out_mode = 0444;
            *out_type = FS_TYPE_FILE;
            return 0;
        }

        // Unknown /proc path — return not found
        return -ENOENT;
    }

    return -ENOENT;
}

// ── virtfs public API ────────────────────────────────────────────────────

int virtfs_is_virtual(const char* path) {
    return find_mount(path) != NULL;
}

// Deterministic test of the path-boundary matching that sys_open's unveil
// exemption depends on.  The bug this guards: a bare-prefix test treated any
// real file named /devsecrets or /processData as virtual, letting it bypass the
// unveil sandbox.  virtfs_is_virtual must match only a mount root or a child
// under it (next char '/' or '\0'), never a longer sibling name.
void virtfs_is_virtual_selftest(void) {
    extern void kprintf(const char*, ...);
    struct { const char* path; int want; } c[] = {
        { "/dev",              1 },  // exact mount root
        { "/dev/",             1 },  // root with trailing slash
        { "/dev/tty",          1 },  // child under mount
        { "/proc",             1 },
        { "/proc/self/status", 1 },
        { "/devsecrets",       0 },  // THE BUG: prefix, no boundary -> NOT virtual
        { "/processData",      0 },  // THE BUG
        { "/device_keys",      0 },
        { "/home/user",        0 },  // unrelated
        { "/",                 0 },
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        if (virtfs_is_virtual(c[i].path) != c[i].want) {
            kprintf_atomic("[virtfs_unveil] FAIL \"%s\" got=%d want=%d\n",
                    c[i].path, virtfs_is_virtual(c[i].path), c[i].want);
            fails++;
        }
    }
    kprintf_atomic(fails ? "[virtfs_unveil] SELF-TEST FAILED\n"
                  : "[virtfs_unveil] SELF-TEST PASSED (mount boundary, no prefix bypass)\n");
}

int virtfs_lookup(const char* path, const cred_t* cred, uint8_t need,
                  fs_node_t* out) {
    const virtmount_t* m = find_mount(path);
    if (!m) return -ENOENT;

    uint32_t uid, gid;
    uint16_t mode;
    int type;
    int r = virt_resolve(path, m, &uid, &gid, &mode, &type);
    if (r != 0) return r;

    // Permission check
    if (cred->euid != 0) {
        acl_entry_t acl[3];
        uint16_t full_mode;
        if (type == FS_TYPE_DIR)       full_mode = 0x4000 | mode;
        else if (type == FS_TYPE_CHAR) full_mode = 0x2000 | mode;
        else                           full_mode = 0x8000 | mode;
        acl_from_mode(acl, uid, gid, full_mode);
        if (!acl_check(acl, 3, cred, need)) {
            // Middle-ground: return EACCES only if the user can already
            // see this node exists (has read+exec on the parent directory).
            // Otherwise return ENOENT to avoid leaking file existence.
            int n = (int)m->prefix_len;
            // Find the parent path (everything up to last '/')
            int last_slash = 0;
            for (int i = 0; path[i]; i++)
                if (path[i] == '/') last_slash = i;

            if (last_slash > 0 && last_slash > n) {
                // Nested path (e.g. /proc/1/fd/3) — resolve parent
                char parent[256];
                for (int i = 0; i < last_slash && i < 255; i++)
                    parent[i] = path[i];
                parent[last_slash] = '\0';

                uint32_t puid, pgid;
                uint16_t pmode;
                int ptype;
                if (virt_resolve(parent, m, &puid, &pgid, &pmode, &ptype) == 0) {
                    acl_entry_t pacl[3];
                    acl_from_mode(pacl, puid, pgid, 0x4000 | pmode);
                    if (acl_check(pacl, 3, cred, ACL_PERM_READ | ACL_PERM_EXEC))
                        return -EACCES;
                }
            } else {
                // Direct child of mount root (e.g. /dev/kbd) — check mount dir
                acl_entry_t pacl[3];
                acl_from_mode(pacl, m->dir_uid, m->dir_gid, 0x4000 | m->dir_mode);
                if (acl_check(pacl, 3, cred, ACL_PERM_READ | ACL_PERM_EXEC))
                    return -EACCES;
            }
            return -ENOENT;
        }
    }

    if (out) {
        out->is_virtual = 1;
        out->type       = type;
        out->uid        = uid;
        out->gid        = gid;
        out->inode_nr   = 0;
        out->size       = 0;
        if (type == FS_TYPE_DIR)       out->mode = 0x4000 | mode;
        else if (type == FS_TYPE_CHAR) out->mode = 0x2000 | mode;
        else                           out->mode = 0x8000 | mode;
    }
    return 0;
}

// ── /dev readdir ─────────────────────────────────────────────────────────

int dev_readdir(ext2_entry_t* out, int max) {
    int count = 0;
    for (int i = 0; s_dev_nodes[i].name && count < max; i++) {
        const char* n = s_dev_nodes[i].name;
        int ni = 0;
        while (n[ni]) { out[count].name[ni] = n[ni]; ni++; }
        out[count].name[ni] = '\0';
        out[count].inode_num = 0xE0000000 + (uint32_t)i;
        out[count].size      = 0;
        out[count].is_dir    = 0;
        count++;
    }
    return count;
}

// ── Path normalization ───────────────────────────────────────────────────

void normalize_path(char* path) {
    if (!path || path[0] != '/') return;

    // Two-pointer in-place: read from `src` (past leading '/'), write to `dst`.
    // We never read ahead of where we write, because we skip components (making
    // dst <= src always), so in-place is safe.
    char* dst = path + 1;
    const char* src = path + 1;
    int offs[128], depth = 0;

    while (*src) {
        if (*src == '/') { src++; continue; }          // collapse "//"
        int len = 0;
        while (src[len] && src[len] != '/') len++;
        if (len == 1 && src[0] == '.') {               // skip "."
            src += len; continue;
        }
        if (len == 2 && src[0] == '.' && src[1] == '.') {  // handle ".."
            if (depth > 0) { depth--; dst = path + offs[depth]; }
            src += len; continue;
        }
        if (depth < 128) offs[depth++] = (int)(dst - path);
        __builtin_memcpy(dst, src, len);
        dst += len;
        src += len;
        if (*src) *dst++ = '/';                        // separator (not trailing)
    }
    if (dst == path + 1) { /* root */ }
    else if (dst[-1] == '/') dst--;                    // strip trailing '/'
    *dst = '\0';
}

// ── Global entry point ───────────────────────────────────────────────────

int fs_lookup(char* path, const cred_t* cred, uint8_t need,
              fs_node_t* out) {
    normalize_path(path);

    // Route to virtfs first; if not a virtual path, fall through to ext2.
    if (virtfs_is_virtual(path))
        return virtfs_lookup(path, cred, need, out);

    // ext2 path.
    int err = 0;
    uint32_t ino = ext2_lookup_path(path, cred, &err);
    if (!ino) return err ? err : -ENOENT;

    if (out) {
        ext2_inode_t inode;
        if (!ext2_read_inode(ino, &inode)) return -ENOENT;
        int is_dir = ((inode.i_mode & 0xF000) == 0x4000);
        out->is_virtual = 0;
        out->type       = is_dir ? FS_TYPE_DIR : FS_TYPE_FILE;
        out->inode_nr   = ino;
        out->uid        = inode.i_uid;
        out->gid        = inode.i_gid;
        out->mode       = inode.i_mode;
        out->size       = inode.i_size;
    }
    return 0;
}
