#include "virtfs.h"
#include "acl.h"
#include "errno.h"
#include "ext2.h"

// ── Virtual mount table ───────────────────────────────────────────────────

typedef struct {
    const char* prefix;
    uint8_t     prefix_len;
    // Mount-root directory: uid, gid, mode
    uint32_t    dir_uid;
    uint32_t    dir_gid;
    uint16_t    dir_mode;
    // Files/nodes inside the mount: uid, gid, mode
    uint32_t    file_uid;
    uint32_t    file_gid;
    uint16_t    file_mode;
    int         file_type;   // FS_TYPE_DIR or FS_TYPE_FILE or FS_TYPE_CHAR
} virtmount_t;

static const virtmount_t s_mounts[] = {
    // /proc — root:root, dir 0555, files 0444 (world-readable, no write)
    { "/proc", 5,  0, 0, 0555,  0, 0, 0444, FS_TYPE_FILE },
    // /dev  — root:root, dir 0755, char nodes 0666
    { "/dev",  4,  0, 0, 0755,  0, 0, 0666, FS_TYPE_CHAR },
};
#define N_MOUNTS ((int)(sizeof(s_mounts)/sizeof(s_mounts[0])))

// ── Internal helpers ──────────────────────────────────────────────────────

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
    return (void*)0;
}

static int perm_check(const virtmount_t* m, const char* path,
                      const cred_t* cred, uint8_t need) {
    if (cred->euid == 0) return 0;  // root bypasses

    int n = (int)m->prefix_len;
    int is_dir = (path[n] == '\0' || (path[n] == '/' && path[n+1] == '\0'));

    uint32_t uid  = is_dir ? m->dir_uid  : m->file_uid;
    uint32_t gid  = is_dir ? m->dir_gid  : m->file_gid;
    uint16_t mode = is_dir ? m->dir_mode : m->file_mode;

    acl_entry_t acl[3];
    acl_from_mode(acl, uid, gid, mode);
    return acl_check(acl, 3, cred, need) ? 0 : -EACCES;
}

// ── virtfs public API ─────────────────────────────────────────────────────

int virtfs_is_virtual(const char* path) {
    return find_mount(path) != (void*)0;
}

int virtfs_lookup(const char* path, const cred_t* cred, uint8_t need,
                  fs_node_t* out) {
    const virtmount_t* m = find_mount(path);
    if (!m) return -ENOENT;

    int r = perm_check(m, path, cred, need);
    if (r != 0) return r;

    if (out) {
        int n = (int)m->prefix_len;
        int is_dir = (path[n] == '\0' || (path[n] == '/' && path[n+1] == '\0'));
        out->is_virtual = 1;
        out->type       = is_dir ? FS_TYPE_DIR : m->file_type;
        out->uid        = is_dir ? m->dir_uid  : m->file_uid;
        out->gid        = is_dir ? m->dir_gid  : m->file_gid;
        out->mode       = is_dir ? (0x4000 | m->dir_mode)
                                 : (m->file_type == FS_TYPE_CHAR
                                        ? (0x2000 | m->file_mode)
                                        : (0x8000 | m->file_mode));
        out->inode_nr   = 0;
        out->size       = 0;
    }
    return 0;
}

// ── Path normalization ────────────────────────────────────────────────────

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
        for (int i = 0; i < len; i++) *dst++ = src[i];
        src += len;
        if (*src) *dst++ = '/';                        // separator (not trailing)
    }
    if (dst == path + 1) { /* root */ }
    else if (dst[-1] == '/') dst--;                    // strip trailing '/'
    *dst = '\0';
}

// ── Global entry point ────────────────────────────────────────────────────

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
