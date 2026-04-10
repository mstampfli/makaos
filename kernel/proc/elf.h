#pragma once
#include "common.h"
#include "process.h"
#include "mm.h"
#include "vmm.h"

// ── ELF64 constants ───────────────────────────────────────────────────────
#define ELF_MAGIC   0x464C457FU  // 0x7F 'E' 'L' 'F' as little-endian uint32

#define ET_EXEC     2            // executable file
#define ET_DYN      3            // position-independent executable (PIE)
#define EM_X86_64   62           // AMD64

// ELF OS/ABI values (e_ident[7])
#define ELFOSABI_NONE   0        // System V / none
#define ELFOSABI_LINUX  3        // Linux

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

// ── Auxiliary vector types (SysV AMD64 ABI, §3.4.4) ─────────────────────
#define AT_NULL    0   // end of vector
#define AT_IGNORE  1   // ignored
#define AT_EXECFD  2   // file descriptor of program
#define AT_PHDR    3   // program header table address
#define AT_PHENT   4   // size of one program header entry
#define AT_PHNUM   5   // number of program header entries
#define AT_PAGESZ  6   // system page size
#define AT_BASE    7   // interpreter base address (0 = no interp)
#define AT_FLAGS   8   // flags
#define AT_ENTRY   9   // program entry point
#define AT_NOTELF  10  // set if not ELF
#define AT_UID     11  // real user ID
#define AT_EUID    12  // effective user ID
#define AT_GID     13  // real group ID
#define AT_EGID    14  // effective group ID
#define AT_HWCAP   16  // hardware capabilities
#define AT_CLKTCK  17  // Hz of times()
#define AT_RANDOM  25  // address of 16 random bytes

// ── API ───────────────────────────────────────────────────────────────────

// Load ELF segments into a new PML4 and create a task for process `pid`.
// `data` points to the full ELF binary in kernel memory; `size` is its byte length.
// Returns the fully initialised task_t* (ready to sched_add), or NULL on error.
task_t* elf_load(const uint8_t* data, uint64_t size, uint32_t pid);

// Load ELF segments into a new PML4 WITHOUT creating a task.
// Used by exec: caller provides its own task and replaces its address space.
//
// Required out-params: out_pml4, out_mm, out_entry.
// Optional out-params (pass NULL to ignore):
//   out_phdr_vaddr — virtual address of the program-header table (AT_PHDR).
//   out_phnum      — number of program-header entries (AT_PHNUM).
//   out_phent      — size of one program-header entry (AT_PHENT).
// Returns 1 on success, 0 on failure.
uint8_t elf_load_into(const uint8_t* data, uint64_t size,
                      phys_addr_t* out_pml4, mm_t** out_mm, uint64_t* out_entry,
                      uint64_t* out_phdr_vaddr,
                      uint16_t* out_phnum,
                      uint16_t* out_phent);

// Full process launch with argc/argv/envp/auxv stack setup.
// Equivalent to elf_load but calls elf_setup_stack so argv[0] etc. are
// correctly passed to the program.  For Linux ABI binaries (bash, musl apps).
// argv and envp are NULL-terminated kernel-side pointer arrays.
//
// stdio: array of 3 fd specs for the child's stdin/stdout/stderr.
//   stdio[i] == -1  → dup fd i from the calling process (inherit)
//   stdio[i] >= 0   → dup that specific fd from the calling process
//   stdio == NULL   → open /dev/tty0 for all three (safe for init)
//
// Returns fully-initialised task_t* (ready to sched_add), or NULL.
task_t* elf_load_with_argv(const uint8_t* data, uint64_t size, uint32_t pid,
                           const char* const* argv, const char* const* envp,
                           const int stdio[3]);

// Convenience: read the ext2 file at `path`, then elf_load_with_argv.
task_t* elf_exec_from_ext2(const char* path, uint32_t pid,
                            const char* const* argv, const char* const* envp,
                            const int stdio[3]);

// Build the SysV AMD64 initial user stack for a newly exec'd process.
// Maps a fresh stack page into `pml4`, copies argv/envp strings onto it,
// and lays out the [argc, argv[], NULL, envp[], NULL, auxv[], AT_NULL]
// structure exactly as the ABI specifies.
//
// argv and envp are kernel-side pointer arrays (NULL-terminated).
// entry is the ELF entry point (for AT_ENTRY).
// phdr_vaddr/phnum/phent are from the loaded ELF (for AT_PHDR).
//
// Returns the initial user RSP to pass to the new process, or 0 on failure.
uint64_t elf_setup_stack(phys_addr_t pml4,
                          const char* const* argv, const char* const* envp,
                          uint64_t entry,
                          uint64_t phdr_vaddr, uint16_t phnum, uint16_t phent);

// Convenience: read the file at `path` from ext2 (up to 8 MiB), call elf_load.
task_t* elf_load_from_ext2(const char* path, uint32_t pid);
