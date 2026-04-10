#include "ext2.h"
#include "ahci.h"
#include "kheap.h"
#include "common.h"

// ── Mount state ────────────────────────────────────────────────────────────

static uint32_t s_part_lba        = 0;
static uint32_t s_block_size      = 0;   // bytes per block (1024, 2048, or 4096)
static uint32_t s_sectors_per_blk = 0;   // block_size / 512
static uint32_t s_inodes_per_grp  = 0;
static uint32_t s_blocks_per_grp  = 0;
static uint32_t s_inode_size      = 128; // bytes per inode on disk
static uint32_t s_num_groups      = 0;
static uint32_t s_first_data_blk  = 0;   // superblock's s_first_data_block
static uint8_t  s_mounted         = 0;

// Static I/O buffers — 1024 bytes to hold a full 1KB block.
// Two buffers so helpers can use one without clobbering the caller's.
static uint8_t s_blk_buf[1024];
static uint8_t s_blk_buf2[1024];

// ── Inode cache — 2-level radix tree ──────────────────────────────────────
// Key: uint32_t inode number.  Value: ext2_inode_t (cached from disk).
// L0 index = ino >> 8  (top 24 bits collapse into 256 buckets via masking)
// L1 index = ino & 0xFF
// Nodes are allocated on demand via kmalloc; never freed (kernel lifetime).
// write_inode() keeps the cache coherent — no explicit invalidation needed.

#define IRTREE_BITS  8
#define IRTREE_SIZE  (1u << IRTREE_BITS)   // 256
#define IRTREE_MASK  (IRTREE_SIZE - 1u)

typedef struct {
    ext2_inode_t inode;
    uint32_t     ino;
    uint8_t      valid;
} irtree_leaf_t;

typedef struct {
    irtree_leaf_t* leaves[IRTREE_SIZE];
} irtree_l1_t;

static irtree_l1_t* s_irtree[IRTREE_SIZE];  // L0: 256 pointers

static uint8_t irtree_get(uint32_t ino, ext2_inode_t* out) {
    uint32_t l0 = (ino >> IRTREE_BITS) & IRTREE_MASK;
    uint32_t l1 = ino & IRTREE_MASK;
    if (!s_irtree[l0]) return 0;
    irtree_leaf_t* leaf = s_irtree[l0]->leaves[l1];
    if (!leaf || !leaf->valid || leaf->ino != ino) return 0;
    *out = leaf->inode;
    return 1;
}

static void irtree_put(uint32_t ino, const ext2_inode_t* inode) {
    uint32_t l0 = (ino >> IRTREE_BITS) & IRTREE_MASK;
    uint32_t l1 = ino & IRTREE_MASK;

    if (!s_irtree[l0]) {
        s_irtree[l0] = kmalloc(sizeof(irtree_l1_t));
        if (!s_irtree[l0]) return;
        for (uint32_t i = 0; i < IRTREE_SIZE; i++) s_irtree[l0]->leaves[i] = NULL;
    }

    irtree_leaf_t* leaf = s_irtree[l0]->leaves[l1];
    if (!leaf) {
        leaf = kmalloc(sizeof(irtree_leaf_t));
        if (!leaf) return;
        s_irtree[l0]->leaves[l1] = leaf;
    }

    leaf->inode = *inode;
    leaf->ino   = ino;
    leaf->valid = 1;
}

// ── Low-level block I/O ────────────────────────────────────────────────────

// Read one filesystem block into `buf` (must be at least s_block_size bytes).
static uint8_t read_block(uint32_t blk, uint8_t* buf) {
    // LBA = partition_start + block_number * (block_size / 512)
    uint32_t lba = s_part_lba + blk * s_sectors_per_blk;
    return ahci_read(lba, buf, s_sectors_per_blk);
}

// Write one filesystem block from `buf`.
static uint8_t write_block(uint32_t blk, const uint8_t* buf) {
    uint32_t lba = s_part_lba + blk * s_sectors_per_blk;
    return ahci_write(lba, buf, s_sectors_per_blk);
}

// ── Bitmap helpers ─────────────────────────────────────────────────────────

// Find first zero bit in `bitmap` (up to max_bits bits wide). Returns bit
// index, or UINT32_MAX if none free.
static uint32_t bitmap_find_free(const uint8_t* bitmap, uint32_t max_bits) {
    for (uint32_t i = 0; i < max_bits; i++) {
        uint32_t byte = i >> 3;
        uint32_t bit  = i & 7;
        if (!(bitmap[byte] & (1u << bit)))
            return i;
    }
    return UINT32_MAX;
}

static void bitmap_set(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void bitmap_clear(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static uint8_t bitmap_test(const uint8_t* bitmap, uint32_t bit) {
    return (bitmap[bit >> 3] >> (bit & 7)) & 1u;
}

// ── BGD helpers ────────────────────────────────────────────────────────────

// BGD table lives at block (s_first_data_blk + 1).
static uint32_t bgd_table_block(void) {
    return s_first_data_blk + 1;
}

// Read BGD for group `g` into `out`.
static uint8_t read_bgd(uint32_t g, ext2_bgd_t* out) {
    // Each BGD is 32 bytes.
    uint32_t offset_in_table = g * sizeof(ext2_bgd_t);
    uint32_t blk_idx = bgd_table_block() + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    if (!read_block(blk_idx, s_blk_buf2)) return 0;

    const uint8_t* src = s_blk_buf2 + off;
    uint8_t* dst = (uint8_t*)out;
    for (uint32_t i = 0; i < sizeof(ext2_bgd_t); i++) dst[i] = src[i];
    return 1;
}

// Write BGD for group `g` from `in`.
static uint8_t write_bgd(uint32_t g, const ext2_bgd_t* in) {
    uint32_t offset_in_table = g * sizeof(ext2_bgd_t);
    uint32_t blk_idx = bgd_table_block() + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    if (!read_block(blk_idx, s_blk_buf2)) return 0;

    uint8_t* dst = s_blk_buf2 + off;
    const uint8_t* src = (const uint8_t*)in;
    for (uint32_t i = 0; i < sizeof(ext2_bgd_t); i++) dst[i] = src[i];
    return write_block(blk_idx, s_blk_buf2);
}

// ── Inode I/O ──────────────────────────────────────────────────────────────

static uint8_t read_inode(uint32_t ino, ext2_inode_t* out) {
    if (ino == 0) return 0;
    if (irtree_get(ino, out)) return 1;  // cache hit

    uint32_t g     = (ino - 1) / s_inodes_per_grp;
    uint32_t local = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return 0;

    uint32_t offset_in_table = local * s_inode_size;
    uint32_t blk_idx = bgd.bg_inode_table + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    if (!read_block(blk_idx, s_blk_buf)) return 0;

    const uint8_t* src = s_blk_buf + off;
    uint8_t* dst = (uint8_t*)out;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) dst[i] = src[i];

    irtree_put(ino, out);  // populate cache
    return 1;
}

static uint8_t write_inode(uint32_t ino, const ext2_inode_t* in) {
    if (ino == 0) return 0;
    uint32_t g     = (ino - 1) / s_inodes_per_grp;
    uint32_t local = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return 0;

    uint32_t offset_in_table = local * s_inode_size;
    uint32_t blk_idx = bgd.bg_inode_table + offset_in_table / s_block_size;
    uint32_t off     = offset_in_table % s_block_size;

    if (!read_block(blk_idx, s_blk_buf)) return 0;

    uint8_t* dst = s_blk_buf + off;
    const uint8_t* src = (const uint8_t*)in;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) dst[i] = src[i];
    if (!write_block(blk_idx, s_blk_buf)) return 0;

    irtree_put(ino, in);  // keep cache coherent
    return 1;
}

// ── ext2_init ──────────────────────────────────────────────────────────────

uint8_t ext2_init(uint32_t part_lba) {
    s_part_lba = part_lba;
    s_mounted  = 0;

    // Superblock is at byte offset 1024 from partition start = LBA part_lba + 2
    // (each sector is 512 bytes, so 1024 = 2 sectors).
    static uint8_t sb_buf[1024];
    if (!ahci_read(part_lba + 2, sb_buf, 2)) return 0;

    ext2_superblock_t* sb = (ext2_superblock_t*)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) return 0;

    s_block_size      = 1024u << sb->s_log_block_size;
    s_sectors_per_blk = s_block_size / 512;
    s_inodes_per_grp  = sb->s_inodes_per_group;
    s_blocks_per_grp  = sb->s_blocks_per_group;
    s_first_data_blk  = sb->s_first_data_block;
    s_inode_size      = (sb->s_rev_level >= 1 && sb->s_inode_size > 0)
                        ? sb->s_inode_size : 128;

    // Number of block groups.
    s_num_groups = (sb->s_blocks_count + s_blocks_per_grp - 1) / s_blocks_per_grp;

    s_mounted = 1;
    return 1;
}

// ── String helpers ─────────────────────────────────────────────────────────

static uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int str_cmp_n(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

// ── Block address resolution (direct + single indirect) ───────────────────

// Return the block number for the Nth logical block of an inode.
// Handles direct blocks (0-11) and single-indirect block (12-267) only.
// Returns 0 on error or if block is sparse/unallocated.
static uint32_t inode_get_block(const ext2_inode_t* ino, uint32_t idx) {
    if (idx < 12) {
        return ino->i_block[idx];
    }

    // Single indirect.
    idx -= 12;
    uint32_t addrs_per_blk = s_block_size / 4;
    if (idx < addrs_per_blk) {
        uint32_t indirect_blk = ino->i_block[12];
        if (!indirect_blk) return 0;

        static uint8_t ind_buf[1024];
        if (!read_block(indirect_blk, ind_buf)) return 0;
        uint32_t* addrs = (uint32_t*)ind_buf;
        return addrs[idx];
    }

    // Double indirect.
    idx -= addrs_per_blk;
    if (idx < addrs_per_blk * addrs_per_blk) {
        uint32_t dblind_blk = ino->i_block[13];
        if (!dblind_blk) return 0;

        static uint8_t dbl_buf[1024];
        if (!read_block(dblind_blk, dbl_buf)) return 0;
        uint32_t* l1 = (uint32_t*)dbl_buf;
        uint32_t l1_idx = idx / addrs_per_blk;
        uint32_t l2_idx = idx % addrs_per_blk;
        if (!l1[l1_idx]) return 0;

        static uint8_t dbl_buf2[1024];
        if (!read_block(l1[l1_idx], dbl_buf2)) return 0;
        uint32_t* l2 = (uint32_t*)dbl_buf2;
        return l2[l2_idx];
    }

    // Triple indirect not implemented — too large for our use case.
    return 0;
}

// ── Directory lookup ───────────────────────────────────────────────────────

// Find a directory entry named `name` (length `name_len`) inside the directory
// inode `dir_ino`. Returns the inode number or 0 if not found.
static uint32_t dir_lookup(const ext2_inode_t* dir_ino, const char* name, uint32_t name_len) {
    uint32_t file_size = dir_ino->i_size;
    uint32_t bytes_left = file_size;
    uint32_t blk_idx = 0;

    while (bytes_left > 0) {
        uint32_t blk = inode_get_block(dir_ino, blk_idx);
        blk_idx++;
        if (!blk) break;

        static uint8_t dir_buf[1024];
        if (!read_block(blk, dir_buf)) break;

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;

        while (off + 8 <= blk_bytes) {
            ext2_dirent_t* de = (ext2_dirent_t*)(dir_buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->name_len == (uint8_t)name_len &&
                str_cmp_n(de->name, name, name_len) == 0) {
                return de->inode;
            }

            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;
    }

    return 0;
}

// ── Path resolution ────────────────────────────────────────────────────────

// Resolve an absolute path to an inode number.
// Returns 0 on failure.
static uint32_t path_to_inode(const char* path) {
    if (!s_mounted) return 0;
    if (!path || path[0] == '\0') return 0;
    if (path[0] != '/') return 0; // must be absolute

    uint32_t cur_ino = EXT2_ROOT_INO;

    uint32_t i = 1; // skip leading '/'

    while (path[i] != '\0') {
        // Extract next component.
        uint32_t start = i;
        while (path[i] != '\0' && path[i] != '/') i++;
        uint32_t comp_len = i - start;
        if (comp_len == 0) {
            // trailing slash or double slash — skip
            if (path[i] == '/') { i++; continue; }
            break;
        }

        // Read current inode.
        ext2_inode_t cur_inode;
        if (!read_inode(cur_ino, &cur_inode)) return 0;
        if (!(cur_inode.i_mode & EXT2_S_IFDIR)) return 0; // not a dir

        // Find component in directory.
        cur_ino = dir_lookup(&cur_inode, path + start, comp_len);
        if (!cur_ino) return 0;

        // Skip slash.
        if (path[i] == '/') i++;
    }

    return cur_ino;
}

// Forward declarations for helpers used by ext2_vfs_write.
static uint32_t alloc_block(void);
static void     free_block(uint32_t blk);
static uint8_t  inode_set_block(ext2_inode_t* inode, uint32_t idx, uint32_t blk_num);

// ── File read VFS callbacks ────────────────────────────────────────────────

typedef struct {
    uint32_t ino;
    uint32_t cur_pos;
    uint32_t file_size;
    ext2_inode_t inode;
} ext2_fd_t;

static int64_t ext2_vfs_read(vfs_file_t* self, void* buf, uint64_t len) {
    ext2_fd_t* fd = (ext2_fd_t*)self->ctx;
    if (!fd) return -1;

    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;

    while (total < len) {
        if (fd->cur_pos >= fd->file_size) break;

        uint32_t blk_idx     = fd->cur_pos / s_block_size;
        uint32_t off_in_blk  = fd->cur_pos % s_block_size;
        uint32_t remain_file = fd->file_size - fd->cur_pos;
        uint32_t remain_blk  = s_block_size - off_in_blk;

        uint32_t to_copy = (uint32_t)(len - total);
        if (to_copy > remain_blk)  to_copy = remain_blk;
        if (to_copy > remain_file) to_copy = remain_file;

        uint32_t blk = inode_get_block(&fd->inode, blk_idx);
        if (!blk) {
            // Sparse hole: block not allocated, return zeros.
            for (uint32_t i = 0; i < to_copy; i++) dst[total + i] = 0;
        } else {
            static uint8_t rd_buf[1024];
            if (!read_block(blk, rd_buf)) return -1;
            const uint8_t* src = rd_buf + off_in_blk;
            for (uint32_t i = 0; i < to_copy; i++) dst[total + i] = src[i];
        }

        total        += to_copy;
        fd->cur_pos  += to_copy;
    }

    return (int64_t)total;
}

// Write `len` bytes from `buf` at fd->cur_pos, growing the file as needed.
static int64_t ext2_vfs_write(vfs_file_t* self, const void* buf, uint64_t len) {
    ext2_fd_t* fd = (ext2_fd_t*)self->ctx;
    if (!fd || !len) return 0;

    // O_APPEND: move to end before writing.
    if (self->flags & 0x400 /*O_APPEND*/) fd->cur_pos = fd->file_size;

    const uint8_t* src = (const uint8_t*)buf;
    uint64_t total = 0;

    while (total < len) {
        uint32_t blk_idx    = fd->cur_pos / s_block_size;
        uint32_t off_in_blk = fd->cur_pos % s_block_size;
        uint32_t to_write   = s_block_size - off_in_blk;
        if (to_write > (uint32_t)(len - total))
            to_write = (uint32_t)(len - total);

        uint32_t blk = inode_get_block(&fd->inode, blk_idx);
        if (!blk) {
            blk = alloc_block();
            if (!blk) break;
            // Zero-initialize new block.
            static uint8_t zb[1024];
            for (uint32_t i = 0; i < s_block_size; i++) zb[i] = 0;
            if (!write_block(blk, zb)) { free_block(blk); break; }
            if (!inode_set_block(&fd->inode, blk_idx, blk)) { free_block(blk); break; }
        }

        // Read-modify-write if partial block.
        static uint8_t wr_buf[1024];
        if (off_in_blk != 0 || to_write != s_block_size) {
            if (!read_block(blk, wr_buf)) break;
        }
        for (uint32_t i = 0; i < to_write; i++)
            wr_buf[off_in_blk + i] = src[total + i];
        if (!write_block(blk, wr_buf)) break;

        total        += to_write;
        fd->cur_pos  += to_write;
        if (fd->cur_pos > fd->file_size) fd->file_size = fd->cur_pos;
    }

    // Persist updated inode (size + block pointers).
    fd->inode.i_size   = fd->file_size;
    fd->inode.i_blocks = ((fd->file_size + s_block_size - 1) / s_block_size) * (s_block_size / 512);
    write_inode(fd->ino, &fd->inode);

    return (int64_t)total;
}

static int64_t ext2_vfs_seek(vfs_file_t* self, int64_t offset, int whence) {
    ext2_fd_t* fd = (ext2_fd_t*)self->ctx;
    if (!fd) return -1;
    int64_t new_pos;
    if (whence == 0 /*SEEK_SET*/) new_pos = offset;
    else if (whence == 1 /*SEEK_CUR*/) new_pos = (int64_t)fd->cur_pos + offset;
    else if (whence == 2 /*SEEK_END*/) new_pos = (int64_t)fd->file_size + offset;
    else return -1;
    if (new_pos < 0) return -1;
    fd->cur_pos = (uint32_t)new_pos;
    return new_pos;
}

static void ext2_vfs_close(vfs_file_t* self) {
    if (self->ctx) kfree(self->ctx);
    kfree(self);
}

// ── ext2_open ──────────────────────────────────────────────────────────────

vfs_file_t* ext2_open(const char* path) {
    if (!s_mounted) return NULL;

    uint32_t ino = path_to_inode(path);
    if (!ino) return NULL;

    ext2_inode_t inode;
    if (!read_inode(ino, &inode)) return NULL;

    // Must be a regular file.
    if (!(inode.i_mode & EXT2_S_IFREG)) return NULL;

    ext2_fd_t* fd = kmalloc(sizeof(ext2_fd_t));
    if (!fd) return NULL;

    fd->ino       = ino;
    fd->cur_pos   = 0;
    fd->file_size = inode.i_size;
    // Copy inode.
    uint8_t* dst = (uint8_t*)&fd->inode;
    const uint8_t* src = (const uint8_t*)&inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) dst[i] = src[i];

    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) { kfree(fd); return NULL; }

    f->read     = ext2_vfs_read;
    f->write    = ext2_vfs_write;   // caller enforces O_RDONLY by clearing this
    f->seek     = ext2_vfs_seek;
    f->close    = ext2_vfs_close;
    f->poll     = NULL;             // ext2 files are always ready
    f->ctx      = fd;
    f->flags    = 0;
    f->refcount = 1;
    f->rights   = 0;   // stamped by sys_open after open; zero for internal opens
    // Store absolute path for fstat/ftruncate.
    uint32_t pi = 0;
    if (path) {
        while (pi < 255 && path[pi]) { f->path[pi] = path[pi]; pi++; }
    }
    f->path[pi] = '\0';
    return f;
}

// ── ext2_readdir ───────────────────────────────────────────────────────────

int ext2_readdir(const char* path, ext2_entry_t* entries, int max) {
    if (!s_mounted || !entries || max <= 0) return -1;

    uint32_t dir_ino = path_to_inode(path);
    if (!dir_ino) return -1;

    ext2_inode_t dir_inode;
    if (!read_inode(dir_ino, &dir_inode)) return -1;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return -1;

    int count = 0;
    uint32_t bytes_left = dir_inode.i_size;
    uint32_t blk_idx = 0;

    while (bytes_left > 0 && count < max) {
        uint32_t blk = inode_get_block(&dir_inode, blk_idx);
        blk_idx++;
        if (!blk) break;

        static uint8_t rdd_buf[1024];
        if (!read_block(blk, rdd_buf)) break;

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;

        while (off + 8 <= blk_bytes && count < max) {
            ext2_dirent_t* de = (ext2_dirent_t*)(rdd_buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                uint32_t nlen = de->name_len;
                // Skip "." and ".."
                int is_dot = (nlen == 1 && de->name[0] == '.');
                int is_dotdot = (nlen == 2 && de->name[0] == '.' && de->name[1] == '.');

                if (!is_dot && !is_dotdot) {
                    ext2_entry_t* out = &entries[count];
                    // Copy name.
                    for (uint32_t j = 0; j < nlen && j < 255; j++)
                        out->name[j] = de->name[j];
                    out->name[nlen < 255 ? nlen : 255] = '\0';
                    out->inode_num = de->inode;

                    // Read inode to get size and type.
                    ext2_inode_t child_ino;
                    if (read_inode(de->inode, &child_ino)) {
                        out->size   = child_ino.i_size;
                        out->is_dir = (child_ino.i_mode & EXT2_S_IFDIR) ? 1 : 0;
                    } else {
                        out->size   = 0;
                        out->is_dir = (de->file_type == EXT2_FT_DIR) ? 1 : 0;
                    }
                    count++;
                }
            }

            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;
    }

    return count;
}

// ── Allocation helpers ─────────────────────────────────────────────────────

// Allocate a free inode from any group. Returns inode number or 0.
static uint32_t alloc_inode(void) {
    static uint8_t ibm_buf[1024];

    for (uint32_t g = 0; g < s_num_groups; g++) {
        ext2_bgd_t bgd;
        if (!read_bgd(g, &bgd)) continue;
        if (bgd.bg_free_inodes_count == 0) continue;

        if (!read_block(bgd.bg_inode_bitmap, ibm_buf)) continue;

        uint32_t bit = bitmap_find_free(ibm_buf, s_inodes_per_grp);
        if (bit == UINT32_MAX) continue;

        bitmap_set(ibm_buf, bit);
        if (!write_block(bgd.bg_inode_bitmap, ibm_buf)) return 0;

        bgd.bg_free_inodes_count--;
        write_bgd(g, &bgd);

        uint32_t ino = g * s_inodes_per_grp + bit + 1;
        return ino;
    }
    return 0; // no free inode
}

// Allocate a free block from any group. Returns block number or 0.
static uint32_t alloc_block(void) {
    static uint8_t bbm_buf[1024];

    for (uint32_t g = 0; g < s_num_groups; g++) {
        ext2_bgd_t bgd;
        if (!read_bgd(g, &bgd)) continue;
        if (bgd.bg_free_blocks_count == 0) continue;

        if (!read_block(bgd.bg_block_bitmap, bbm_buf)) continue;

        uint32_t bit = bitmap_find_free(bbm_buf, s_blocks_per_grp);
        if (bit == UINT32_MAX) continue;

        bitmap_set(bbm_buf, bit);
        if (!write_block(bgd.bg_block_bitmap, bbm_buf)) return 0;

        bgd.bg_free_blocks_count--;
        write_bgd(g, &bgd);

        uint32_t blk = g * s_blocks_per_grp + bit + s_first_data_blk;
        return blk;
    }
    return 0;
}

// Free a block.
static void free_block(uint32_t blk) {
    if (!blk) return;
    static uint8_t fbb_buf[1024];

    // Determine which group this block belongs to.
    uint32_t rel = (blk >= s_first_data_blk) ? (blk - s_first_data_blk) : blk;
    uint32_t g   = rel / s_blocks_per_grp;
    uint32_t bit = rel % s_blocks_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return;
    if (!read_block(bgd.bg_block_bitmap, fbb_buf)) return;

    if (bitmap_test(fbb_buf, bit)) {
        bitmap_clear(fbb_buf, bit);
        write_block(bgd.bg_block_bitmap, fbb_buf);
        bgd.bg_free_blocks_count++;
        write_bgd(g, &bgd);
    }
}

// Free an inode.
static void free_inode_num(uint32_t ino) {
    if (!ino) return;
    static uint8_t fib_buf[1024];

    uint32_t g   = (ino - 1) / s_inodes_per_grp;
    uint32_t bit = (ino - 1) % s_inodes_per_grp;

    ext2_bgd_t bgd;
    if (!read_bgd(g, &bgd)) return;
    if (!read_block(bgd.bg_inode_bitmap, fib_buf)) return;

    if (bitmap_test(fib_buf, bit)) {
        bitmap_clear(fib_buf, bit);
        write_block(bgd.bg_inode_bitmap, fib_buf);
        bgd.bg_free_inodes_count++;
        write_bgd(g, &bgd);
    }
}

// Zero a block on disk.
static void zero_block(uint32_t blk) {
    static uint8_t zb_buf[1024];
    for (uint32_t i = 0; i < s_block_size; i++) zb_buf[i] = 0;
    write_block(blk, zb_buf);
}

// Free all data blocks of an inode (direct + single indirect only).
static void free_inode_blocks(ext2_inode_t* inode) {
    uint32_t addrs_per_blk = s_block_size / 4;

    for (uint32_t i = 0; i < 12; i++) {
        if (inode->i_block[i]) {
            free_block(inode->i_block[i]);
            inode->i_block[i] = 0;
        }
    }

    if (inode->i_block[12]) {
        static uint8_t ind_free_buf[1024];
        if (read_block(inode->i_block[12], ind_free_buf)) {
            uint32_t* addrs = (uint32_t*)ind_free_buf;
            for (uint32_t i = 0; i < addrs_per_blk; i++) {
                if (addrs[i]) free_block(addrs[i]);
            }
        }
        free_block(inode->i_block[12]);
        inode->i_block[12] = 0;
    }

    // Double indirect.
    if (inode->i_block[13]) {
        static uint8_t dind_free_buf[1024];
        if (read_block(inode->i_block[13], dind_free_buf)) {
            uint32_t* l1 = (uint32_t*)dind_free_buf;
            for (uint32_t i = 0; i < addrs_per_blk; i++) {
                if (!l1[i]) continue;
                static uint8_t dind_free_buf2[1024];
                if (read_block(l1[i], dind_free_buf2)) {
                    uint32_t* l2 = (uint32_t*)dind_free_buf2;
                    for (uint32_t j = 0; j < addrs_per_blk; j++) {
                        if (l2[j]) free_block(l2[j]);
                    }
                }
                free_block(l1[i]);
            }
        }
        free_block(inode->i_block[13]);
        inode->i_block[13] = 0;
    }

    inode->i_size   = 0;
    inode->i_blocks = 0;
}

// Set block pointer N in inode (allocates indirect block if needed).
// Returns 1 on success, 0 on failure.
static uint8_t inode_set_block(ext2_inode_t* inode, uint32_t idx, uint32_t blk_num) {
    if (idx < 12) {
        inode->i_block[idx] = blk_num;
        return 1;
    }

    uint32_t addrs_per_blk = s_block_size / 4;
    idx -= 12;

    if (idx < addrs_per_blk) {
        // Single indirect.
        static uint8_t si_buf[1024];
        if (!inode->i_block[12]) {
            // Allocate indirect block.
            uint32_t ind_blk = alloc_block();
            if (!ind_blk) return 0;
            inode->i_block[12] = ind_blk;
            // Zero it.
            for (uint32_t i = 0; i < s_block_size; i++) si_buf[i] = 0;
            if (!write_block(ind_blk, si_buf)) return 0;
        }
        if (!read_block(inode->i_block[12], si_buf)) return 0;
        uint32_t* addrs = (uint32_t*)si_buf;
        addrs[idx] = blk_num;
        return write_block(inode->i_block[12], si_buf);
    }

    idx -= addrs_per_blk;

    if (idx < addrs_per_blk * addrs_per_blk) {
        // Double indirect.
        static uint8_t di_l1_buf[1024];
        if (!inode->i_block[13]) {
            uint32_t dind_blk = alloc_block();
            if (!dind_blk) return 0;
            inode->i_block[13] = dind_blk;
            for (uint32_t i = 0; i < s_block_size; i++) di_l1_buf[i] = 0;
            if (!write_block(dind_blk, di_l1_buf)) return 0;
        }
        if (!read_block(inode->i_block[13], di_l1_buf)) return 0;
        uint32_t* l1 = (uint32_t*)di_l1_buf;
        uint32_t l1_idx = idx / addrs_per_blk;
        uint32_t l2_idx = idx % addrs_per_blk;

        static uint8_t di_l2_buf[1024];
        if (!l1[l1_idx]) {
            uint32_t l2_blk = alloc_block();
            if (!l2_blk) return 0;
            l1[l1_idx] = l2_blk;
            if (!write_block(inode->i_block[13], di_l1_buf)) return 0;
            for (uint32_t i = 0; i < s_block_size; i++) di_l2_buf[i] = 0;
            if (!write_block(l2_blk, di_l2_buf)) return 0;
        }
        if (!read_block(l1[l1_idx], di_l2_buf)) return 0;
        uint32_t* l2 = (uint32_t*)di_l2_buf;
        l2[l2_idx] = blk_num;
        return write_block(l1[l1_idx], di_l2_buf);
    }

    return 0; // triple indirect not supported
}

// ── Directory entry manipulation ───────────────────────────────────────────

// Append a directory entry `name` → `child_ino` to directory `dir_ino_num`.
// Returns 1 on success, 0 on failure.
static uint8_t dir_add_entry(uint32_t dir_ino_num, const char* name,
                              uint32_t child_ino, uint8_t file_type) {
    ext2_inode_t dir_inode;
    if (!read_inode(dir_ino_num, &dir_inode)) return 0;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return 0;

    uint32_t name_len = str_len(name);
    // Aligned rec_len for new entry.
    uint32_t new_rec_len = 8 + name_len;
    if (new_rec_len & 3) new_rec_len = (new_rec_len | 3) + 1; // align to 4

    uint32_t blk_idx = 0;
    uint32_t bytes_left = dir_inode.i_size;

    while (1) {
        uint32_t blk;
        uint8_t new_blk = 0;

        if (bytes_left == 0) {
            // Need to allocate a new block for the directory.
            blk = alloc_block();
            if (!blk) return 0;
            zero_block(blk);
            if (!inode_set_block(&dir_inode, blk_idx, blk)) {
                free_block(blk);
                return 0;
            }
            dir_inode.i_size   += s_block_size;
            dir_inode.i_blocks += s_block_size / 512;
            new_blk = 1;
        } else {
            blk = inode_get_block(&dir_inode, blk_idx);
            if (!blk) break;
        }

        static uint8_t dae_buf[1024];
        if (!read_block(blk, dae_buf)) return 0;

        if (new_blk) {
            // Write entry as the only entry in this fresh block.
            ext2_dirent_t* de = (ext2_dirent_t*)dae_buf;
            de->inode     = child_ino;
            de->rec_len   = (uint16_t)s_block_size;
            de->name_len  = (uint8_t)name_len;
            de->file_type = file_type;
            for (uint32_t i = 0; i < name_len; i++) de->name[i] = name[i];
            if (!write_block(blk, dae_buf)) return 0;
            write_inode(dir_ino_num, &dir_inode);
            return 1;
        }

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;

        while (off + 8 <= blk_bytes) {
            ext2_dirent_t* de = (ext2_dirent_t*)(dae_buf + off);
            if (de->rec_len == 0) break;

            // Check if this entry has slack space we can use.
            uint32_t actual_len = 8 + de->name_len;
            if (actual_len & 3) actual_len = (actual_len | 3) + 1;
            uint32_t slack = de->rec_len - actual_len;

            if (de->inode == 0 && de->rec_len >= new_rec_len) {
                // Reuse deleted entry.
                de->inode     = child_ino;
                de->name_len  = (uint8_t)name_len;
                de->file_type = file_type;
                for (uint32_t i = 0; i < name_len; i++) de->name[i] = name[i];
                if (!write_block(blk, dae_buf)) return 0;
                write_inode(dir_ino_num, &dir_inode);
                return 1;
            }

            if (slack >= new_rec_len) {
                // Split: shrink current entry, add new one after it.
                uint32_t old_rec = de->rec_len;
                de->rec_len = (uint16_t)actual_len;
                ext2_dirent_t* new_de = (ext2_dirent_t*)(dae_buf + off + actual_len);
                new_de->inode     = child_ino;
                new_de->rec_len   = (uint16_t)(old_rec - actual_len);
                new_de->name_len  = (uint8_t)name_len;
                new_de->file_type = file_type;
                for (uint32_t i = 0; i < name_len; i++) new_de->name[i] = name[i];
                if (!write_block(blk, dae_buf)) return 0;
                write_inode(dir_ino_num, &dir_inode);
                return 1;
            }

            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;

        blk_idx++;
    }

    return 0;
}

// Remove a directory entry by name from the directory `dir_ino_num`.
// Marks the entry's inode field as 0 (deleted). Returns 1 on success, 0 if not found.
static uint8_t dir_remove_entry(uint32_t dir_ino_num, const char* name) {
    ext2_inode_t dir_inode;
    if (!read_inode(dir_ino_num, &dir_inode)) return 0;

    uint32_t name_len = str_len(name);
    uint32_t bytes_left = dir_inode.i_size;
    uint32_t blk_idx = 0;

    while (bytes_left > 0) {
        uint32_t blk = inode_get_block(&dir_inode, blk_idx);
        blk_idx++;
        if (!blk) break;

        static uint8_t dre_buf[1024];
        if (!read_block(blk, dre_buf)) break;

        uint32_t blk_bytes = (bytes_left < s_block_size) ? bytes_left : s_block_size;
        uint32_t off = 0;

        while (off + 8 <= blk_bytes) {
            ext2_dirent_t* de = (ext2_dirent_t*)(dre_buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->name_len == (uint8_t)name_len &&
                str_cmp_n(de->name, name, name_len) == 0) {
                de->inode = 0;
                write_block(blk, dre_buf);
                return 1;
            }
            off += de->rec_len;
        }

        if (blk_bytes >= s_block_size)
            bytes_left -= s_block_size;
        else
            bytes_left = 0;
    }
    return 0;
}

// ── Path utilities ─────────────────────────────────────────────────────────

// Split path into parent path and basename.
// parent_out must be >= str_len(path)+1 bytes.
// Returns the basename pointer within `path`.
static const char* path_split(const char* path, char* parent_out) {
    uint32_t len = str_len(path);
    // Find last '/'.
    int last_slash = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    if (last_slash <= 0) {
        // Parent is "/".
        parent_out[0] = '/';
        parent_out[1] = '\0';
        // Basename starts after '/' at position 0, or at 0 if no slash.
        return (last_slash == 0) ? path + 1 : path;
    }

    // Copy up to last_slash.
    for (int i = 0; i < last_slash; i++) parent_out[i] = path[i];
    parent_out[last_slash] = '\0';
    return path + last_slash + 1;
}

// ── ext2_write_file ────────────────────────────────────────────────────────

int ext2_write_file(const char* path, const uint8_t* data, uint32_t size) {
    if (!s_mounted) return 0;

    // Split into parent dir and basename.
    static char parent_path[256];
    const char* basename = path_split(path, parent_path);
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(parent_path);
    if (!parent_ino) return 0;

    // Check if file already exists.
    ext2_inode_t parent_inode;
    if (!read_inode(parent_ino, &parent_inode)) return 0;

    uint32_t existing_ino = dir_lookup(&parent_inode, basename, str_len(basename));

    if (existing_ino) {
        // Overwrite: free all old blocks.
        ext2_inode_t old_inode;
        if (!read_inode(existing_ino, &old_inode)) return 0;

        // Free all data blocks.
        free_inode_blocks(&old_inode);

        // Now write new data into existing inode.
        uint32_t n_blocks = (size + s_block_size - 1) / s_block_size;
        uint32_t written = 0;

        for (uint32_t bi = 0; bi < n_blocks; bi++) {
            uint32_t blk = alloc_block();
            if (!blk) return 0;

            static uint8_t wr_buf[1024];
            uint32_t to_write = size - written;
            if (to_write > s_block_size) to_write = s_block_size;

            for (uint32_t i = 0; i < to_write; i++) wr_buf[i] = data[written + i];
            for (uint32_t i = to_write; i < s_block_size; i++) wr_buf[i] = 0;

            if (!write_block(blk, wr_buf)) { free_block(blk); return 0; }
            if (!inode_set_block(&old_inode, bi, blk)) { free_block(blk); return 0; }

            written += to_write;
        }

        old_inode.i_size   = size;
        old_inode.i_blocks = n_blocks * (s_block_size / 512);
        old_inode.i_mode   = EXT2_S_IFREG | 0644;
        old_inode.i_links_count = 1;
        write_inode(existing_ino, &old_inode);
        return 1;
    }

    // File doesn't exist — allocate a new inode.
    uint32_t new_ino = alloc_inode();
    if (!new_ino) return 0;

    ext2_inode_t new_inode;
    // Zero it.
    uint8_t* np = (uint8_t*)&new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) np[i] = 0;

    new_inode.i_mode        = EXT2_S_IFREG | 0644;
    new_inode.i_links_count = 1;
    new_inode.i_size        = size;

    uint32_t n_blocks = (size + s_block_size - 1) / s_block_size;
    uint32_t written  = 0;

    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t blk = alloc_block();
        if (!blk) { free_inode_num(new_ino); return 0; }

        static uint8_t nwr_buf[1024];
        uint32_t to_write = size - written;
        if (to_write > s_block_size) to_write = s_block_size;

        for (uint32_t i = 0; i < to_write; i++) nwr_buf[i] = data[written + i];
        for (uint32_t i = to_write; i < s_block_size; i++) nwr_buf[i] = 0;

        if (!write_block(blk, nwr_buf)) { free_block(blk); free_inode_num(new_ino); return 0; }
        if (!inode_set_block(&new_inode, bi, blk)) {
            free_block(blk);
            free_inode_num(new_ino);
            return 0;
        }

        written += to_write;
    }

    new_inode.i_blocks = n_blocks * (s_block_size / 512);
    write_inode(new_ino, &new_inode);

    // Add directory entry in parent.
    if (!dir_add_entry(parent_ino, basename, new_ino, EXT2_FT_REG_FILE)) {
        free_inode_num(new_ino);
        return 0;
    }

    return 1;
}

// ── ext2_create ───────────────────────────────────────────────────────────
// Create an empty file.  Returns 0 if the file already exists.
int ext2_create(const char* path) {
    if (!s_mounted) return 0;
    static char parent_path[256];
    const char* basename = path_split(path, parent_path);
    if (!basename || basename[0] == '\0') return 0;
    uint32_t parent_ino = path_to_inode(parent_path);
    if (!parent_ino) return 0;
    ext2_inode_t parent_inode;
    if (!read_inode(parent_ino, &parent_inode)) return 0;
    // Fail if already exists.
    if (dir_lookup(&parent_inode, basename, str_len(basename))) return 0;
    // Create empty file via ext2_write_file.
    return ext2_write_file(path, (const uint8_t*)"", 0);
}

// ── ext2_truncate ─────────────────────────────────────────────────────────
// Truncate file to zero bytes.
int ext2_truncate(const char* path) {
    return ext2_write_file(path, (const uint8_t*)"", 0);
}

// ── ext2_truncate_to ──────────────────────────────────────────────────────
// Truncate (or extend) a file to exactly `length` bytes.
// If length > current size, the file is extended with zeros.
// If length == 0, equivalent to ext2_truncate.
// Returns 1 on success, 0 on failure.
int ext2_truncate_to(const char* path, uint64_t length) {
    if (!s_mounted) return 0;
    if (length == 0) return ext2_truncate(path);

    // For simplicity: read the current content, resize to `length`, write back.
    // This is O(file_size) but correct and consistent with our write model.
    uint32_t ino = path_to_inode(path);
    if (!ino) return 0;
    ext2_inode_t inode;
    if (!read_inode(ino, &inode)) return 0;
    uint32_t cur_size = inode.i_size;

    if (length == cur_size) return 1;

    // Clamp to 32-bit (we don't support >4GiB files).
    if (length > 0xFFFFFFFFULL) return 0;
    uint32_t new_size = (uint32_t)length;

    if (new_size < cur_size) {
        // Read current content, write back shorter version.
        uint8_t* buf = kmalloc(new_size);
        if (!buf) return 0;
        vfs_file_t* f = ext2_open(path);
        if (!f) { kfree(buf); return 0; }
        int64_t n = vfs_read(f, buf, new_size);
        vfs_close(f);
        if (n < 0) { kfree(buf); return 0; }
        int r = ext2_write_file(path, buf, new_size);
        kfree(buf);
        return r;
    } else {
        // Extend: read, zero-pad, write.
        uint8_t* buf = kmalloc(new_size);
        if (!buf) return 0;
        vfs_file_t* f = ext2_open(path);
        if (!f) { kfree(buf); return 0; }
        int64_t n = vfs_read(f, buf, cur_size);
        vfs_close(f);
        if (n < 0) { kfree(buf); return 0; }
        // Zero the extended region.
        for (uint32_t i = cur_size; i < new_size; i++) buf[i] = 0;
        int r = ext2_write_file(path, buf, new_size);
        kfree(buf);
        return r;
    }
}

// ── ext2_lookup_path / ext2_read_inode ───────────────────────────────────
// Public wrappers for internal path resolution and inode reading.
// Used by fstat to get file metadata without opening a new fd.
uint32_t ext2_lookup_path(const char* path) {
    if (!s_mounted || !path) return 0;
    return path_to_inode(path);
}

uint8_t ext2_read_inode(uint32_t ino, ext2_inode_t* out) {
    return read_inode(ino, out);
}

// ── ext2_mkdir ────────────────────────────────────────────────────────────

int ext2_mkdir(const char* path) {
    if (!s_mounted) return 0;

    // Check it doesn't already exist.
    if (path_to_inode(path)) return 0; // already exists

    static char md_parent[256];
    const char* basename = path_split(path, md_parent);
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(md_parent);
    if (!parent_ino) return 0;

    // Allocate inode.
    uint32_t new_ino = alloc_inode();
    if (!new_ino) return 0;

    // Allocate one data block for "." and "..".
    uint32_t data_blk = alloc_block();
    if (!data_blk) { free_inode_num(new_ino); return 0; }

    // Build the data block with "." and "..".
    static uint8_t mkdir_buf[1024];
    for (uint32_t i = 0; i < s_block_size; i++) mkdir_buf[i] = 0;

    // "." entry.
    ext2_dirent_t* dot = (ext2_dirent_t*)mkdir_buf;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';

    // ".." entry.
    ext2_dirent_t* dotdot = (ext2_dirent_t*)(mkdir_buf + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(s_block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    if (!write_block(data_blk, mkdir_buf)) {
        free_block(data_blk);
        free_inode_num(new_ino);
        return 0;
    }

    // Create inode.
    ext2_inode_t new_inode;
    uint8_t* np2 = (uint8_t*)&new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++) np2[i] = 0;

    new_inode.i_mode        = EXT2_S_IFDIR | 0755;
    new_inode.i_links_count = 2; // "." and parent's entry
    new_inode.i_size        = s_block_size;
    new_inode.i_blocks      = s_block_size / 512;
    new_inode.i_block[0]    = data_blk;
    write_inode(new_ino, &new_inode);

    // Add to parent directory.
    if (!dir_add_entry(parent_ino, basename, new_ino, EXT2_FT_DIR)) {
        free_block(data_blk);
        free_inode_num(new_ino);
        return 0;
    }

    // Increment parent's links_count for ".."
    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode)) {
        parent_inode.i_links_count++;
        write_inode(parent_ino, &parent_inode);
    }

    // Increment used_dirs_count in parent's BGD.
    uint32_t g = (new_ino - 1) / s_inodes_per_grp;
    ext2_bgd_t bgd;
    if (read_bgd(g, &bgd)) {
        bgd.bg_used_dirs_count++;
        write_bgd(g, &bgd);
    }

    return 1;
}

// ── ext2_unlink ───────────────────────────────────────────────────────────
// Remove a regular file at `path`.  Returns 1 on success, 0 on failure.
int ext2_unlink(const char* path) {
    if (!s_mounted || !path) return 0;

    uint32_t ino = path_to_inode(path);
    if (!ino) return 0;

    ext2_inode_t inode;
    if (!read_inode(ino, &inode)) return 0;

    // Refuse to unlink directories.
    if ((inode.i_mode & 0xF000) == EXT2_S_IFDIR) return 0;

    static char ul_parent[256];
    const char* basename = path_split(path, ul_parent);
    if (!basename || basename[0] == '\0') return 0;

    uint32_t parent_ino = path_to_inode(ul_parent);
    if (!parent_ino) return 0;

    // Remove directory entry first.
    if (!dir_remove_entry(parent_ino, basename)) return 0;

    // Decrement link count; only free inode/data when it reaches 0.
    if (inode.i_links_count > 0) inode.i_links_count--;
    if (inode.i_links_count == 0) {
        free_inode_blocks(&inode);
        inode.i_dtime = 1;
        write_inode(ino, &inode);
        free_inode_num(ino);
    } else {
        write_inode(ino, &inode);
    }

    return 1;
}

// ── ext2_rename ───────────────────────────────────────────────────────────
// Move/rename `src` to `dst`.  Returns 1 on success, 0 on failure.
// If `dst` already exists as a regular file it is removed first.
int ext2_rename(const char* src, const char* dst) {
    if (!s_mounted || !src || !dst) return 0;

    uint32_t src_ino = path_to_inode(src);
    if (!src_ino) return 0;

    ext2_inode_t src_inode;
    if (!read_inode(src_ino, &src_inode)) return 0;

    uint8_t is_dir = ((src_inode.i_mode & 0xF000) == EXT2_S_IFDIR);

    // Resolve src parent/basename.
    static char rn_src_parent[256];
    const char* src_base = path_split(src, rn_src_parent);
    if (!src_base || src_base[0] == '\0') return 0;
    uint32_t src_parent_ino = path_to_inode(rn_src_parent);
    if (!src_parent_ino) return 0;

    // Resolve dst parent/basename.
    static char rn_dst_parent[256];
    const char* dst_base = path_split(dst, rn_dst_parent);
    if (!dst_base || dst_base[0] == '\0') return 0;
    uint32_t dst_parent_ino = path_to_inode(rn_dst_parent);
    if (!dst_parent_ino) return 0;

    // If dst exists as a regular file, unlink it.
    uint32_t dst_ino = path_to_inode(dst);
    if (dst_ino) {
        ext2_inode_t dst_inode;
        if (!read_inode(dst_ino, &dst_inode)) return 0;
        if ((dst_inode.i_mode & 0xF000) == EXT2_S_IFDIR) return 0; // refuse dir collision
        dir_remove_entry(dst_parent_ino, dst_base);
        free_inode_blocks(&dst_inode);
        dst_inode.i_links_count = 0;
        write_inode(dst_ino, &dst_inode);
        free_inode_num(dst_ino);
    }

    // Add new directory entry pointing at the same inode.
    uint8_t ft = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (!dir_add_entry(dst_parent_ino, dst_base, src_ino, ft)) return 0;

    // Remove old directory entry.
    dir_remove_entry(src_parent_ino, src_base);

    return 1;
}
