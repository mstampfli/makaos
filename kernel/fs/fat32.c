#include "fat32.h"
#include "ata_poll.h"
#include "kheap.h"
#include "common.h"

static uint8_t ata_read_sync(uint32_t lba, void* buf, uint32_t count) {
    return ata_poll_read28(lba, buf, count);
}

static uint8_t ata_write_sync(uint32_t lba, const void* buf, uint32_t count) {
    return ata_poll_write28(lba, buf, count);
}

// ── FAT32 on-disk structures (packed, little-endian) ─────────────────────

typedef struct __attribute__((packed)) {
    uint8_t  jump[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    // 0 for FAT32
    uint16_t total_sectors_16;    // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;         // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended BPB
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        // first cluster of root directory
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];          // "FAT32   "
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    uint8_t  name[11];       // 8.3 name, space-padded, uppercase
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} fat32_dirent_t;

#define FAT32_ATTR_DIR      0x10
#define FAT32_ATTR_VOLUME   0x08
#define FAT32_ATTR_LFN      0x0F  // long file name entry — skip
#define FAT32_EOC           0x0FFFFFF8U  // end-of-chain marker

// ── Mount state ───────────────────────────────────────────────────────────

uint32_t g_fat32_part_lba = 0;

static uint32_t s_bytes_per_sector;
static uint32_t s_sectors_per_cluster;
static uint32_t s_fat_lba;        // LBA of FAT (first copy)
static uint32_t s_data_lba;       // LBA of data region (cluster 2)
static uint32_t s_root_cluster;   // first cluster of root directory
static uint32_t s_num_fats    = 2;
static uint32_t s_fat_size_32 = 0;  // sectors per FAT
static uint8_t  s_mounted = 0;

// One-sector scratch buffer.  Not re-entrant — single CPU cooperative only.
static uint8_t s_sector_buf[512];

// ── Cluster/LBA helpers ───────────────────────────────────────────────────

static uint32_t cluster_to_lba(uint32_t cluster) {
    return s_data_lba + (cluster - 2) * s_sectors_per_cluster;
}

// Read the FAT entry for `cluster`.  Returns next cluster number.
static uint32_t fat_next(uint32_t cluster) {
    // Each FAT32 entry is 4 bytes.
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = s_fat_lba + fat_offset / s_bytes_per_sector;
    uint32_t byte_offset = fat_offset % s_bytes_per_sector;

    if (!ata_read_sync(fat_sector, s_sector_buf, 1))
        return 0x0FFFFFFF;  // treat I/O error as EOC

    uint32_t val;
    // Read 4 bytes little-endian from the buffer.
    val  = (uint32_t)s_sector_buf[byte_offset + 0];
    val |= (uint32_t)s_sector_buf[byte_offset + 1] << 8;
    val |= (uint32_t)s_sector_buf[byte_offset + 2] << 16;
    val |= (uint32_t)s_sector_buf[byte_offset + 3] << 24;
    return val & 0x0FFFFFFF;
}

// ── fat32_init ────────────────────────────────────────────────────────────
uint8_t fat32_init(uint32_t part_lba) {
    g_fat32_part_lba = part_lba;

    // Read the BPB (sector 0 of the partition).
    if (!ata_read_sync(part_lba, s_sector_buf, 1))
        return 0;

    fat32_bpb_t* bpb = (fat32_bpb_t*)s_sector_buf;

    // Basic sanity checks.
    if (bpb->bytes_per_sector != 512)  return 0;
    if (bpb->sectors_per_cluster == 0) return 0;
    if (bpb->num_fats == 0)            return 0;
    if (bpb->fat_size_32 == 0)         return 0;

    s_bytes_per_sector    = bpb->bytes_per_sector;
    s_sectors_per_cluster = bpb->sectors_per_cluster;
    s_root_cluster        = bpb->root_cluster;
    s_num_fats            = bpb->num_fats;
    s_fat_size_32         = bpb->fat_size_32;

    // FAT starts after the reserved sectors.
    s_fat_lba = part_lba + bpb->reserved_sectors;

    // Data region starts after reserved sectors + all FATs.
    s_data_lba = s_fat_lba + (uint32_t)bpb->num_fats * bpb->fat_size_32;

    s_mounted = 1;
    return 1;
}

// ── File descriptor state ─────────────────────────────────────────────────

typedef struct {
    uint32_t start_cluster;   // first cluster of the file
    uint32_t cur_cluster;     // cluster currently being read
    uint32_t cur_sector;      // sector index within cur_cluster (0-based)
    uint32_t sector_offset;   // byte offset within cur_sector
    uint32_t file_size;       // total bytes in file
    uint32_t bytes_read;      // bytes consumed so far
} fat32_fd_t;

static int64_t fat32_read(vfs_file_t* self, void* buf, uint64_t len) {
    fat32_fd_t* fd = (fat32_fd_t*)self->ctx;
    if (!fd) return -1;

    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;

    while (total < len) {
        // Check EOF.
        if (fd->bytes_read >= fd->file_size) break;
        if (fd->cur_cluster >= FAT32_EOC)    break;

        // How many bytes remain in the current sector.
        uint32_t remaining_in_sector = s_bytes_per_sector - fd->sector_offset;
        uint32_t remaining_in_file   = fd->file_size - fd->bytes_read;
        uint32_t to_copy = (uint32_t)(len - total);
        if (to_copy > remaining_in_sector) to_copy = remaining_in_sector;
        if (to_copy > remaining_in_file)   to_copy = remaining_in_file;

        // Read the current sector from disk.
        uint32_t lba = cluster_to_lba(fd->cur_cluster) + fd->cur_sector;
        if (!ata_read_sync(lba, s_sector_buf, 1)) return -1;

        // Copy bytes from sector buffer.
        uint8_t* src = s_sector_buf + fd->sector_offset;
        for (uint32_t i = 0; i < to_copy; i++)
            dst[total + i] = src[i];

        total             += to_copy;
        fd->bytes_read    += to_copy;
        fd->sector_offset += to_copy;

        // Advance sector/cluster pointers if we exhausted this sector.
        if (fd->sector_offset >= s_bytes_per_sector) {
            fd->sector_offset = 0;
            fd->cur_sector++;
            if (fd->cur_sector >= s_sectors_per_cluster) {
                fd->cur_sector = 0;
                fd->cur_cluster = fat_next(fd->cur_cluster);
            }
        }
    }

    return (int64_t)total;
}

static void fat32_close(vfs_file_t* self) {
    if (self->ctx) kfree(self->ctx);
    kfree(self);
}

// ── fat32_open ────────────────────────────────────────────────────────────
// Walks the root directory looking for an entry whose 8.3 name matches
// `name83` (exactly 11 bytes, uppercase, space-padded).
vfs_file_t* fat32_open(const char* name83) {
    if (!s_mounted) return NULL;

    uint32_t cluster = s_root_cluster;

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t sec = 0; sec < s_sectors_per_cluster; sec++) {
            if (!ata_read_sync(lba + sec, s_sector_buf, 1)) return NULL;

            fat32_dirent_t* dir = (fat32_dirent_t*)s_sector_buf;
            uint32_t entries = s_bytes_per_sector / sizeof(fat32_dirent_t);

            for (uint32_t i = 0; i < entries; i++) {
                fat32_dirent_t* e = &dir[i];

                // End of directory.
                if (e->name[0] == 0x00) return NULL;
                // Deleted entry.
                if (e->name[0] == 0xE5) continue;
                // Skip LFN, volume label, directory entries.
                if (e->attr == FAT32_ATTR_LFN)    continue;
                if (e->attr & FAT32_ATTR_VOLUME)   continue;
                if (e->attr & FAT32_ATTR_DIR)      continue;

                // Compare 8.3 name (11 bytes).
                uint8_t match = 1;
                for (int j = 0; j < 11; j++) {
                    if (e->name[j] != (uint8_t)name83[j]) { match = 0; break; }
                }
                if (!match) continue;

                // Found — allocate fd and vfs_file_t from the heap.
                fat32_fd_t* fd = kmalloc(sizeof(fat32_fd_t));
                fd->start_cluster = ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
                fd->cur_cluster   = fd->start_cluster;
                fd->cur_sector    = 0;
                fd->sector_offset = 0;
                fd->file_size     = e->file_size;
                fd->bytes_read    = 0;

                vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
                f->read     = fat32_read;
                f->write    = NULL;
                f->close    = fat32_close;
                f->seek     = NULL;
                f->poll     = NULL;
                f->ctx      = fd;
                f->flags    = 0;
                f->refcount = 1;
                f->rights   = 0;
                f->path[0]  = '\0';
                return f;
            }
        }

        cluster = fat_next(cluster);
    }

    return NULL;
}

// ── fat32_readdir ─────────────────────────────────────────────────────────
// Walk the root cluster chain and fill `entries` (up to max_entries).
// Returns count of entries written.
int fat32_readdir(fat32_entry_t* entries, int max_entries) {
    if (!s_mounted || !entries || max_entries <= 0) return 0;

    // Use a separate sector buffer on the stack to avoid clobbering s_sector_buf
    // which fat_next() uses.
    uint8_t dir_buf[512];

    int count = 0;
    uint32_t cluster = s_root_cluster;

    while (cluster < FAT32_EOC && count < max_entries) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t sec = 0; sec < s_sectors_per_cluster && count < max_entries; sec++) {
            if (!ata_read_sync(lba + sec, dir_buf, 1)) return count;

            fat32_dirent_t* dir = (fat32_dirent_t*)dir_buf;
            uint32_t entries_per_sector = s_bytes_per_sector / sizeof(fat32_dirent_t);

            for (uint32_t i = 0; i < entries_per_sector && count < max_entries; i++) {
                fat32_dirent_t* e = &dir[i];

                // End of directory.
                if (e->name[0] == 0x00) return count;
                // Deleted entry.
                if ((uint8_t)e->name[0] == 0xE5) continue;
                // Skip LFN entries.
                if (e->attr == FAT32_ATTR_LFN) continue;
                // Skip volume labels.
                if (e->attr & FAT32_ATTR_VOLUME) continue;

                fat32_entry_t* out = &entries[count];

                // Copy raw 8.3 name.
                for (int j = 0; j < 11; j++) out->name83[j] = (char)e->name[j];

                // Build human-readable name.
                // Name part: up to 8 chars, strip trailing spaces.
                int nlen = 8;
                while (nlen > 0 && out->name83[nlen - 1] == ' ') nlen--;

                int pos = 0;
                for (int j = 0; j < nlen; j++) out->name[pos++] = out->name83[j];

                // Extension part: up to 3 chars, strip trailing spaces.
                int elen = 3;
                while (elen > 0 && out->name83[8 + elen - 1] == ' ') elen--;
                if (elen > 0) {
                    out->name[pos++] = '.';
                    for (int j = 0; j < elen; j++) out->name[pos++] = out->name83[8 + j];
                }
                out->name[pos] = '\0';

                out->size   = (e->attr & FAT32_ATTR_DIR) ? 0 : e->file_size;
                out->is_dir = (e->attr & FAT32_ATTR_DIR) ? 1 : 0;

                count++;
            }
        }

        cluster = fat_next(cluster);
    }

    return count;
}

// ── FAT write helpers ─────────────────────────────────────────────────────

// Write FAT[cluster] = value in all FAT copies.
static uint8_t fat_write(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = fat_offset / s_bytes_per_sector;
    uint32_t byte_offset = fat_offset % s_bytes_per_sector;

    uint32_t abs_sector = s_fat_lba + fat_sector;

    // Read existing sector.
    static uint8_t fat_buf[512];
    if (!ata_read_sync(abs_sector, fat_buf, 1)) return 0;

    // Modify entry (preserve top 4 bits per FAT32 spec).
    uint32_t old_val;
    old_val  = (uint32_t)fat_buf[byte_offset + 0];
    old_val |= (uint32_t)fat_buf[byte_offset + 1] << 8;
    old_val |= (uint32_t)fat_buf[byte_offset + 2] << 16;
    old_val |= (uint32_t)fat_buf[byte_offset + 3] << 24;
    uint32_t new_val = (old_val & 0xF0000000U) | (value & 0x0FFFFFFFU);

    fat_buf[byte_offset + 0] = (uint8_t)(new_val);
    fat_buf[byte_offset + 1] = (uint8_t)(new_val >> 8);
    fat_buf[byte_offset + 2] = (uint8_t)(new_val >> 16);
    fat_buf[byte_offset + 3] = (uint8_t)(new_val >> 24);

    // Write to all FAT copies.
    for (uint32_t f = 0; f < s_num_fats; f++) {
        uint32_t copy_sector = s_fat_lba + f * s_fat_size_32 + fat_sector;
        if (!ata_write_sync(copy_sector, fat_buf, 1)) return 0;
    }
    return 1;
}

// Allocate a free FAT cluster (returns 0 on failure).
static uint32_t fat_alloc(void) {
    static uint8_t fat_buf[512];
    uint32_t entries_per_sector = s_bytes_per_sector / 4;
    uint32_t total_fat_sectors  = s_fat_size_32;

    for (uint32_t sec = 0; sec < total_fat_sectors; sec++) {
        if (!ata_read_sync(s_fat_lba + sec, fat_buf, 1)) return 0;

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint32_t cluster = sec * entries_per_sector + i;
            if (cluster < 2) continue;  // clusters 0 and 1 are reserved

            uint32_t val;
            uint32_t off = i * 4;
            val  = (uint32_t)fat_buf[off + 0];
            val |= (uint32_t)fat_buf[off + 1] << 8;
            val |= (uint32_t)fat_buf[off + 2] << 16;
            val |= (uint32_t)fat_buf[off + 3] << 24;
            val &= 0x0FFFFFFF;

            if (val == 0) return cluster;
        }
    }
    return 0;  // disk full
}

// Free entire cluster chain starting at `start` (writes 0 to each entry).
static void fat_free_chain(uint32_t start) {
    uint32_t cluster = start;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t next = fat_next(cluster);
        fat_write(cluster, 0);
        cluster = next;
    }
}

// ── fat32_write_file ──────────────────────────────────────────────────────
// Creates or overwrites a file in the root directory.
// `name83` must be exactly 11 bytes, uppercase, space-padded (8.3 format).
// Returns 1 on success, 0 on failure.
int fat32_write_file(const char* name83, const uint8_t* data, uint32_t size) {
    if (!s_mounted) return 0;

    // Use a separate buffer to avoid corrupting s_sector_buf during dir walk.
    static uint8_t dir_buf[512];

    // ── Phase 1: scan root dir for matching or free entry ──────────────────
    uint32_t dir_cluster   = s_root_cluster;
    uint32_t found_lba     = 0;   // absolute LBA of the sector
    uint32_t found_idx     = 0;   // index within sector
    uint32_t free_lba      = 0;   // first free (deleted/empty) slot LBA
    uint32_t free_idx      = 0;
    uint32_t old_first_cluster = 0;
    uint8_t  file_exists   = 0;

    while (dir_cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(dir_cluster);

        for (uint32_t sec = 0; sec < s_sectors_per_cluster; sec++) {
            uint32_t abs_lba = lba + sec;
            if (!ata_read_sync(abs_lba, dir_buf, 1)) return 0;

            fat32_dirent_t* dir = (fat32_dirent_t*)dir_buf;
            uint32_t entries    = s_bytes_per_sector / sizeof(fat32_dirent_t);

            for (uint32_t i = 0; i < entries; i++) {
                fat32_dirent_t* e = &dir[i];

                if (e->name[0] == 0x00) {
                    // End of directory — record as free slot if we haven't yet.
                    if (!free_lba) { free_lba = abs_lba; free_idx = i; }
                    goto scan_done;
                }

                if ((uint8_t)e->name[0] == 0xE5) {
                    // Deleted entry — usable free slot.
                    if (!free_lba) { free_lba = abs_lba; free_idx = i; }
                    continue;
                }

                if (e->attr == FAT32_ATTR_LFN)   continue;
                if (e->attr & FAT32_ATTR_VOLUME)  continue;
                if (e->attr & FAT32_ATTR_DIR)     continue;

                // Compare 8.3 name.
                uint8_t match = 1;
                for (int j = 0; j < 11; j++) {
                    if (e->name[j] != (uint8_t)name83[j]) { match = 0; break; }
                }
                if (match) {
                    found_lba = abs_lba;
                    found_idx = i;
                    old_first_cluster = ((uint32_t)e->cluster_hi << 16) | e->cluster_lo;
                    file_exists = 1;
                    goto scan_done;
                }
            }
        }

        dir_cluster = fat_next(dir_cluster);
    }

scan_done:;

    // ── Phase 2: free old cluster chain if overwriting ─────────────────────
    if (file_exists && old_first_cluster >= 2) {
        fat_free_chain(old_first_cluster);
    }

    // ── Phase 3: allocate clusters and write data ──────────────────────────
    uint32_t bytes_per_cluster = s_bytes_per_sector * s_sectors_per_cluster;
    uint32_t first_cluster = 0;
    uint32_t prev_cluster  = 0;

    // Handle zero-length file specially.
    if (size == 0) {
        first_cluster = 0;
    } else {
        uint32_t remaining = size;
        const uint8_t* src = data;
        static uint8_t write_buf[512];

        while (remaining > 0) {
            uint32_t cl = fat_alloc();
            if (!cl) return 0;  // disk full

            // Mark allocated (tentatively as EOC; we'll chain below).
            if (!fat_write(cl, 0x0FFFFFFF)) return 0;

            // Chain previous cluster to this one.
            if (prev_cluster) {
                if (!fat_write(prev_cluster, cl)) return 0;
            } else {
                first_cluster = cl;
            }

            // Write data into this cluster sector by sector.
            uint32_t cl_lba = cluster_to_lba(cl);
            uint32_t cl_written = 0;

            for (uint32_t s = 0; s < s_sectors_per_cluster && remaining > 0; s++) {
                uint32_t to_write = remaining < 512 ? remaining : 512;

                // Copy into write_buf, zero-pad rest.
                for (uint32_t b = 0; b < to_write; b++) write_buf[b] = src[b];
                for (uint32_t b = to_write; b < 512; b++) write_buf[b] = 0;

                if (!ata_write_sync(cl_lba + s, write_buf, 1)) return 0;

                src       += to_write;
                remaining -= to_write;
                cl_written += to_write;
            }

            // Zero-pad any remaining sectors in this cluster.
            uint32_t sectors_used = (cl_written + 511) / 512;
            for (uint32_t s = sectors_used; s < s_sectors_per_cluster; s++) {
                for (uint32_t b = 0; b < 512; b++) write_buf[b] = 0;
                ata_write_sync(cl_lba + s, write_buf, 1);
            }

            prev_cluster = cl;
            (void)bytes_per_cluster;
        }
        // last cluster already marked EOC above
    }

    // ── Phase 4: update or create directory entry ──────────────────────────
    uint32_t target_lba = file_exists ? found_lba : free_lba;
    uint32_t target_idx = file_exists ? found_idx : free_idx;

    if (!target_lba) return 0;  // no free slot found

    // Re-read the target directory sector.
    if (!ata_read_sync(target_lba, dir_buf, 1)) return 0;

    fat32_dirent_t* e = (fat32_dirent_t*)dir_buf + target_idx;

    // Populate entry.
    for (int j = 0; j < 11; j++) e->name[j] = (uint8_t)name83[j];
    e->attr          = 0x20;  // ARCHIVE
    e->reserved      = 0;
    e->crt_time_tenth= 0;
    e->crt_time      = 0;
    e->crt_date      = 0x4A21;  // arbitrary date
    e->acc_date      = 0x4A21;
    e->wrt_time      = 0;
    e->wrt_date      = 0x4A21;
    e->cluster_hi    = (uint16_t)(first_cluster >> 16);
    e->cluster_lo    = (uint16_t)(first_cluster & 0xFFFF);
    e->file_size     = size;

    if (!ata_write_sync(target_lba, dir_buf, 1)) return 0;

    return 1;
}
