#pragma once
#include "common.h"
#include "process.h"
#include "mm.h"
#include "vmm.h"

// ── ELF64 constants ───────────────────────────────────────────────────────
#define ELF_MAGIC   0x464C457FU  // 0x7F 'E' 'L' 'F' as little-endian uint32

#define ET_EXEC     2            // executable file
#define EM_X86_64   62           // AMD64

#define PT_LOAD     1            // loadable segment

#define PF_X        (1U << 0)   // segment is executable
#define PF_W        (1U << 1)   // segment is writable
#define PF_R        (1U << 2)   // segment is readable

// ── ELF64 header ─────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];   // magic + class + data + version + OS/ABI + padding
    uint16_t e_type;        // ET_EXEC = 2
    uint16_t e_machine;     // EM_X86_64 = 62
    uint32_t e_version;     // 1
    uint64_t e_entry;       // virtual entry point
    uint64_t e_phoff;       // program header table offset
    uint64_t e_shoff;       // section header table offset (unused)
    uint32_t e_flags;       // processor-specific flags
    uint16_t e_ehsize;      // ELF header size (64)
    uint16_t e_phentsize;   // size of one program header entry (56)
    uint16_t e_phnum;       // number of program header entries
    uint16_t e_shentsize;   // size of one section header entry
    uint16_t e_shnum;       // number of section header entries
    uint16_t e_shstrndx;    // section name string table index
} Elf64_Ehdr;

// ── ELF64 program header ──────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t p_type;        // PT_LOAD = 1
    uint32_t p_flags;       // PF_R | PF_W | PF_X
    uint64_t p_offset;      // offset in file
    uint64_t p_vaddr;       // virtual address in memory
    uint64_t p_paddr;       // physical address (ignored)
    uint64_t p_filesz;      // size in file
    uint64_t p_memsz;       // size in memory (>= p_filesz; extra is zero)
    uint64_t p_align;       // alignment
} Elf64_Phdr;

// ── API ───────────────────────────────────────────────────────────────────

// Load ELF segments into a new PML4 and create a task for process `pid`.
// `data` points to the full ELF binary in kernel memory; `size` is its byte length.
// Returns the fully initialised task_t* (ready to sched_add), or NULL on error.
task_t* elf_load(const uint8_t* data, uint64_t size, uint32_t pid);

// Load ELF segments into a new PML4 WITHOUT creating a task.
// Used by exec: caller provides its own task and replaces its address space.
// Out-params: new_pml4, new_mm, entry virtual address.
// Returns 1 on success, 0 on failure.
uint8_t elf_load_into(const uint8_t* data, uint64_t size,
                      phys_addr_t* out_pml4, mm_t** out_mm, uint64_t* out_entry);

// Convenience: read the file at `path` from ext2 (up to 1 MiB), call elf_load.
task_t* elf_load_from_ext2(const char* path, uint32_t pid);
