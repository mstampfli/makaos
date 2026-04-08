/*
 * MakaOS UEFI Bootloader
 *
 * Replaces boot1.asm + boot2.asm + kernelLoader entirely.
 * Compiled as a freestanding PE32+ executable for x86_64 UEFI.
 *
 * Flow:
 *   1. Locate GOP framebuffer
 *   2. Load kernel.bin from \EFI\MAKA\kernel.bin on the ESP
 *   3. Get UEFI memory map
 *   4. Build page tables (identity 1GiB, HHDM, kernel at 0xFFFFFFFF80000000)
 *   5. ExitBootServices
 *   6. Translate UEFI memory map to e820, fill boot_info_t
 *   7. Load CR3, jump to kernel
 */

/* ── Minimal EFI type definitions (no gnu-efi dependency) ─────────────────── */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef uint64_t           uintptr_t;
typedef uint64_t           size_t;
typedef uint64_t           EFI_STATUS;
typedef void*              EFI_HANDLE;

#define NULL ((void*)0)
#define EFI_SUCCESS             0ULL
#define EFI_ERROR(s)            ((s) & (1ULL << 63))
#define EFI_BUFFER_TOO_SMALL    (5ULL  | (1ULL << 63))

/* UEFI calling convention on x86_64 is MS ABI */
#define EFIAPI __attribute__((ms_abi))

/* ── EFI GUID ───────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* ── Memory types ───────────────────────────────────────────────────────── */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

/* ── Simple Text Output ─────────────────────────────────────────────────── */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void* Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, uint16_t*);
    void* TestString;
    void* QueryMode;
    void* SetMode;
    void* SetAttribute;
    void* ClearScreen;
    void* SetCursorPosition;
    void* EnableCursor;
    void* Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ── Boot Services (only the ones we need) ───────────────────────────────── */
typedef struct {
    uint8_t  Hdr[24];  /* EFI_TABLE_HEADER — 24 bytes */
    void*    RaiseTPL;
    void*    RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE,
                                        size_t, EFI_PHYSICAL_ADDRESS*);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, size_t);
    EFI_STATUS (EFIAPI *GetMemoryMap)(size_t*, EFI_MEMORY_DESCRIPTOR*,
                                      size_t*, size_t*, uint32_t*);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, size_t, void**);
    EFI_STATUS (EFIAPI *FreePool)(void*);
    void*    CreateEvent;
    void*    SetTimer;
    void*    WaitForEvent;
    void*    SignalEvent;
    void*    CloseEvent;
    void*    CheckEvent;
    void*    InstallProtocolInterface;
    void*    ReinstallProtocolInterface;
    void*    UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID*, void**);
    void*    Reserved;
    void*    RegisterProtocolNotify;
    EFI_STATUS (EFIAPI *LocateHandle)(uint32_t, EFI_GUID*, void*, size_t*, EFI_HANDLE*);
    void*    LocateDevicePath;
    void*    InstallConfigurationTable;
    EFI_STATUS (EFIAPI *LoadImage)(uint8_t, EFI_HANDLE, void*, void*, size_t, EFI_HANDLE*);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, size_t*, uint16_t**);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE, EFI_STATUS, size_t, uint16_t*);
    void*    UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, size_t);
    void*    GetNextMonotonicCount;
    void*    Stall;
    void*    SetWatchdogTimer;
    void*    ConnectController;
    void*    DisconnectController;
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE, EFI_GUID*, void**, EFI_HANDLE, EFI_HANDLE, uint32_t);
    void*    CloseProtocol;
    void*    OpenProtocolInformation;
    void*    ProtocolsPerHandle;
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(uint32_t, EFI_GUID*, void*, size_t*, EFI_HANDLE**);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID*, void*, void**);
    void*    InstallMultipleProtocolInterfaces;
    void*    UninstallMultipleProtocolInterfaces;
    void*    CalculateCrc32;
    void*    CopyMem;
    void*    SetMem;
    void*    CreateEventEx;
} EFI_BOOT_SERVICES;

/* ── System Table ───────────────────────────────────────────────────────── */
/* EFI_TABLE_HEADER: Signature(8) + Revision(4) + HeaderSize(4) + CRC32(4) + Reserved(4) = 24 bytes */
typedef struct {
    uint8_t                        Hdr[24];  /* EFI_TABLE_HEADER — 24 bytes exactly */
    uint16_t*                      FirmwareVendor;
    uint32_t                       FirmwareRevision;
    uint32_t                       Pad1;
    EFI_HANDLE                     ConsoleInHandle;
    void*                          ConIn;
    EFI_HANDLE                     ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE                     StandardErrorHandle;
    void*                          StdErr;
    void*                          RuntimeServices;
    EFI_BOOT_SERVICES*             BootServices;
} EFI_SYSTEM_TABLE;

/* ── Loaded Image Protocol ───────────────────────────────────────────────── */
typedef struct {
    uint32_t    Revision;
    uint32_t    Pad;
    EFI_HANDLE  ParentHandle;
    EFI_SYSTEM_TABLE* SystemTable;
    EFI_HANDLE  DeviceHandle;
    void*       FilePath;
    void*       Reserved;
    uint32_t    LoadOptionsSize;
    void*       LoadOptions;
    void*       ImageBase;
    uint64_t    ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    void*       Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ── Simple File System Protocol ─────────────────────────────────────────── */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**,
                               uint16_t*, uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL*);
    void*      Delete;
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL*, size_t*, void*);
    void*      Write;
    void*      GetPosition;
    void*      SetPosition;
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL*, EFI_GUID*, size_t*, void*);
    void*      SetInfo;
    void*      Flush;
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*,
                                     EFI_FILE_PROTOCOL**);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* EFI_FILE_INFO layout (only fields we need) */
typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint8_t  CreateTime[16];
    uint8_t  LastAccessTime[16];
    uint8_t  ModificationTime[16];
    uint64_t Attribute;
    uint16_t FileName[1]; /* variable length */
} EFI_FILE_INFO;

/* ── Graphics Output Protocol ────────────────────────────────────────────── */
typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t         Version;
    uint32_t         HorizontalResolution;
    uint32_t         VerticalResolution;
    uint32_t         PixelFormat; /* 0=RGB, 1=BGR, 2=bitmask, 3=BltOnly */
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t         PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                           MaxMode;
    uint32_t                           Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    size_t                             SizeOfInfo;
    EFI_PHYSICAL_ADDRESS               FrameBufferBase;
    size_t                             FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void*                               QueryMode;
    void*                               SetMode;
    void*                               Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE*  Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ── GUIDs ──────────────────────────────────────────────────────────────── */
static EFI_GUID g_gop_guid = {
    0x9042a9de, 0x23dc, 0x4a38,
    {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}
};
static EFI_GUID g_sfsp_guid = {
    0x0964e5b22, 0x6459, 0x11d2,
    {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}
};
static EFI_GUID g_lip_guid = {
    0x5b1b31a1, 0x9562, 0x11d2,
    {0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}
};
static EFI_GUID g_file_info_guid = {
    0x09576e92, 0x6d3f, 0x11d2,
    {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}
};

/* ── boot_info_t (must match kernel/common.h exactly) ───────────────────── */
#define E820_MAX 128

typedef struct __attribute__((packed)) {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
} e820_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t sig0;
    uint32_t sig1;
    uint16_t      e820_count;
    e820_entry_t  e820_map[E820_MAX];
    uint64_t fb_phys;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint64_t kernel_phys_base;
    uint64_t phys_ceiling;
    uint64_t pml4_phys;
    uint64_t hhdm_offset;
} boot_info_t;

/* ── Constants ───────────────────────────────────────────────────────────── */
#define HHDM_OFFSET     0xFFFF800000000000ULL
#define KERNEL_HH_BASE  0xFFFFFFFF80000000ULL
#define PAGE_SIZE       4096ULL
#define GIB             (1ULL << 30)

/* Reserved 32 MiB window at kernel_phys_base (page tables live at the top) */
#define KERNEL_WINDOW   (32ULL * 1024 * 1024)
#define PT_PAGES        6   /* PML4, pdpt_id, pd_id, pdpt_hh, pd_kern, pdpt_hhdm */

/* ── I/O port helpers ────────────────────────────────────────────────────── */
static uint8_t inb_efi(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void outb_efi(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* ── Serial debug output ─────────────────────────────────────────────────── */
static void serial_putc(char c) {
    while (!(inb_efi(0x3F8 + 5) & 0x20));
    outb_efi(0x3F8, (uint8_t)c);
}
static void serial_puts(const char* s) {
    for (; *s; s++) serial_putc(*s);
}
static void serial_hex(uint64_t v) {
    const char* h = "0123456789ABCDEF";
    serial_putc('0'); serial_putc('x');
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(h[(v >> i) & 0xF]);
    serial_putc('\n');
}

/* ── Utility ─────────────────────────────────────────────────────────────── */
static void efi_memset(void* p, uint8_t v, size_t n) {
    uint8_t* b = (uint8_t*)p;
    for (size_t i = 0; i < n; i++) b[i] = v;
}

static void hang(void) {
    serial_puts("BOOT HANG\n");
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── Page table setup ────────────────────────────────────────────────────── */
/*
 * Builds 6 page tables inside the reserved kernel window:
 *   [kernel_phys + KERNEL_WINDOW - PT_PAGES*PAGE_SIZE]
 *
 * Maps:
 *   VA [0, 1GiB)                 → PA [0, 1GiB)            (identity, 2MiB pages)
 *   VA [KERNEL_HH_BASE, ...]     → PA [kernel_phys, ...]   (2MiB pages, 16 entries)
 *   VA [HHDM_OFFSET, ...]        → PA [0, phys_ceiling)    (1GiB pages)
 */
static uint64_t setup_paging(uint64_t kernel_phys, uint64_t phys_ceiling) {
    uint64_t pt_base = kernel_phys + KERNEL_WINDOW - (uint64_t)PT_PAGES * PAGE_SIZE;

    uint64_t* pml4      = (uint64_t*)(pt_base + 0 * PAGE_SIZE);
    uint64_t* pdpt_id   = (uint64_t*)(pt_base + 1 * PAGE_SIZE);
    uint64_t* pd_id     = (uint64_t*)(pt_base + 2 * PAGE_SIZE);
    uint64_t* pdpt_hh   = (uint64_t*)(pt_base + 3 * PAGE_SIZE);
    uint64_t* pd_kern   = (uint64_t*)(pt_base + 4 * PAGE_SIZE);
    uint64_t* pdpt_hhdm = (uint64_t*)(pt_base + 5 * PAGE_SIZE);

    efi_memset(pml4,      0, PAGE_SIZE);
    efi_memset(pdpt_id,   0, PAGE_SIZE);
    efi_memset(pd_id,     0, PAGE_SIZE);
    efi_memset(pdpt_hh,   0, PAGE_SIZE);
    efi_memset(pd_kern,   0, PAGE_SIZE);
    efi_memset(pdpt_hhdm, 0, PAGE_SIZE);

    /* Identity map: PML4[0] → pdpt_id → pd_id (2MiB pages for first 1GiB) */
    pml4[0]    = (uint64_t)pdpt_id | 0x3;
    pdpt_id[0] = (uint64_t)pd_id   | 0x3;
    for (uint64_t i = 0; i < 512; i++)
        pd_id[i] = (i * 2 * 1024 * 1024) | 0x83; /* P | RW | PS */

    /* Kernel high-half: PML4[511] → pdpt_hh → pd_kern (2MiB pages, 16 slots) */
    pml4[511]    = (uint64_t)pdpt_hh | 0x3;
    pdpt_hh[510] = (uint64_t)pd_kern | 0x3;
    for (uint64_t i = 0; i < 16; i++)
        pd_kern[i] = (kernel_phys + i * 2 * 1024 * 1024) | 0x83;

    /* HHDM: PML4[256] → pdpt_hhdm (1GiB pages covering physical RAM) */
    pml4[256] = (uint64_t)pdpt_hhdm | 0x3;
    uint64_t gib_count = (phys_ceiling + GIB - 1) / GIB;
    if (gib_count > 512) gib_count = 512;
    for (uint64_t i = 0; i < gib_count; i++)
        pdpt_hhdm[i] = (i * GIB) | 0x83; /* P | RW | PS (1GiB) */

    return (uint64_t)pml4;
}

/* ── Translate UEFI memory map to e820 ───────────────────────────────────── */
static void translate_mmap(const uint8_t* raw, size_t mmap_size,
                            size_t desc_size, uint64_t kernel_phys,
                            boot_info_t* bi) {
    uint16_t count = 0;
    uint64_t ceiling = 0;

    for (size_t off = 0; off + desc_size <= mmap_size; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR* d =
            (const EFI_MEMORY_DESCRIPTOR*)(raw + off);
        uint64_t base = d->PhysicalStart;
        uint64_t len  = d->NumberOfPages * PAGE_SIZE;
        uint64_t end  = base + len;

        /* Determine e820 type */
        uint32_t type;
        switch (d->Type) {
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiLoaderCode:
            case EfiLoaderData:
                type = 1; /* usable RAM */
                break;
            case EfiACPIReclaimMemory:
                type = 3;
                break;
            case EfiACPIMemoryNVS:
                type = 4;
                break;
            default:
                type = 2; /* reserved */
                break;
        }

        /* Mark kernel's 32MiB window as reserved so PMM doesn't reclaim it */
        if (type == 1 &&
            base < kernel_phys + KERNEL_WINDOW &&
            end  > kernel_phys) {
            type = 2;
        }

        if (count < E820_MAX) {
            bi->e820_map[count].base   = base;
            bi->e820_map[count].length = len;
            bi->e820_map[count].type   = type;
            bi->e820_map[count].attr   = 0;
            count++;
        }

        if (end > ceiling) ceiling = end;
    }

    bi->e820_count   = count;
    bi->phys_ceiling = ceiling;
}

/* ── Wchar helpers for EFI file open ─────────────────────────────────────── */
static uint16_t efi_path[] = {
    '\\','E','F','I','\\','M','A','K','A','\\',
    'k','e','r','n','e','l','.','b','i','n', 0
};

/* ── EFI entry point ─────────────────────────────────────────────────────── */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* ST) {
    EFI_BOOT_SERVICES* BS = ST->BootServices;

    serial_puts("MakaOS UEFI bootloader starting\n");

    /* ── 1. Get GOP framebuffer ─────────────────────────────────────────── */
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    BS->LocateProtocol(&g_gop_guid, NULL, (void**)&gop);
    if (!gop) { serial_puts("FATAL: no GOP\n"); hang(); }

    uint64_t fb_phys  = gop->Mode->FrameBufferBase;
    uint32_t fb_w     = gop->Mode->Info->HorizontalResolution;
    uint32_t fb_h     = gop->Mode->Info->VerticalResolution;
    uint32_t fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;

    serial_puts("GOP fb="); serial_hex(fb_phys);
    serial_puts("    w=");  serial_hex(fb_w);
    serial_puts("    h=");  serial_hex(fb_h);

    /* ── 2. Load kernel.bin from ESP ───────────────────────────────────── */
    /* Get the SimpleFileSystem on our boot device */
    EFI_LOADED_IMAGE_PROTOCOL* lip = NULL;
    BS->HandleProtocol(ImageHandle, &g_lip_guid, (void**)&lip);
    if (!lip) { serial_puts("FATAL: no LIP\n"); hang(); }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfsp = NULL;
    BS->HandleProtocol(lip->DeviceHandle, &g_sfsp_guid, (void**)&sfsp);
    if (!sfsp) { serial_puts("FATAL: no SFSP\n"); hang(); }

    EFI_FILE_PROTOCOL* root = NULL;
    if (EFI_ERROR(sfsp->OpenVolume(sfsp, &root))) {
        serial_puts("FATAL: OpenVolume\n"); hang();
    }

    EFI_FILE_PROTOCOL* kfile = NULL;
    if (EFI_ERROR(root->Open(root, &kfile, efi_path, 1 /*READ*/, 0))) {
        serial_puts("FATAL: cannot open kernel.bin\n"); hang();
    }

    /* Get file size via GetInfo */
    uint8_t info_buf[256];
    size_t info_sz = sizeof(info_buf);
    if (EFI_ERROR(kfile->GetInfo(kfile, &g_file_info_guid, &info_sz, info_buf))) {
        serial_puts("FATAL: GetInfo\n"); hang();
    }
    EFI_FILE_INFO* finfo = (EFI_FILE_INFO*)info_buf;
    size_t kernel_size = (size_t)finfo->FileSize;

    serial_puts("kernel.bin size="); serial_hex(kernel_size);

    /* Allocate 32 MiB window (kernel + reserved space for page tables) */
    size_t window_pages = KERNEL_WINDOW / PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS kernel_phys = 0;
    if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                     window_pages, &kernel_phys))) {
        serial_puts("FATAL: AllocatePages\n"); hang();
    }

    /* Align to 2 MiB boundary */
    uint64_t aligned = (kernel_phys + 0x1FFFFULL) & ~0x1FFFFFULL;
    /* If alignment wasted too much, we still have room because we allocated
       32 MiB but the kernel itself is much smaller. Accept the aligned base. */
    kernel_phys = aligned;

    serial_puts("kernel_phys="); serial_hex(kernel_phys);

    /* Zero the window first */
    efi_memset((void*)kernel_phys, 0, KERNEL_WINDOW);

    /* Read kernel into memory */
    size_t read_size = kernel_size;
    if (EFI_ERROR(kfile->Read(kfile, &read_size, (void*)kernel_phys))) {
        serial_puts("FATAL: kernel read\n"); hang();
    }
    kfile->Close(kfile);
    root->Close(root);

    serial_puts("kernel loaded, bytes="); serial_hex(read_size);

    /* ── 3. Allocate memory map buffer ─────────────────────────────────── */
    size_t mmap_size = 0, map_key = 0, desc_size = 0;
    uint32_t desc_version = 0;

    /* First call to determine size */
    BS->GetMemoryMap(&mmap_size, NULL, &map_key, &desc_size, &desc_version);
    mmap_size += 4 * desc_size; /* slack for our AllocatePages calls */

    uint8_t* mmap_buf = NULL;
    BS->AllocatePool(EfiLoaderData, mmap_size, (void**)&mmap_buf);
    if (!mmap_buf) { serial_puts("FATAL: mmap pool\n"); hang(); }

    /* ── 4. Build page tables (before ExitBootServices) ────────────────── */
    /* We need a preliminary memory map scan to find phys_ceiling */
    size_t tmp_sz = mmap_size;
    BS->GetMemoryMap(&tmp_sz, (EFI_MEMORY_DESCRIPTOR*)mmap_buf,
                     &map_key, &desc_size, &desc_version);

    uint64_t phys_ceiling = 0;
    for (size_t off = 0; off + desc_size <= tmp_sz; off += desc_size) {
        const EFI_MEMORY_DESCRIPTOR* d =
            (const EFI_MEMORY_DESCRIPTOR*)(mmap_buf + off);
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (end > phys_ceiling) phys_ceiling = end;
    }
    /* Round up to next GiB boundary */
    phys_ceiling = (phys_ceiling + GIB - 1) & ~(GIB - 1);
    serial_puts("phys_ceiling="); serial_hex(phys_ceiling);

    uint64_t pml4_phys = setup_paging(kernel_phys, phys_ceiling);
    serial_puts("pml4_phys="); serial_hex(pml4_phys);

    /* Place boot_info_t just below the page tables at the top of the 32MiB window.
       Page tables occupy [kernel_phys + KERNEL_WINDOW - PT_PAGES*PAGE_SIZE, top).
       boot_info_t sits one page below that, safely away from the kernel image. */
    boot_info_t* bi = (boot_info_t*)(kernel_phys + KERNEL_WINDOW
                                     - (uint64_t)(PT_PAGES + 1) * PAGE_SIZE);
    efi_memset(bi, 0, sizeof(boot_info_t));
    bi->sig0            = 0xB007EF11U;
    bi->sig1            = 0xCAFEF00DU;
    bi->fb_phys         = fb_phys;
    bi->fb_width        = fb_w;
    bi->fb_height       = fb_h;
    bi->fb_pitch        = fb_pitch;
    bi->fb_bpp          = 32;
    bi->kernel_phys_base = kernel_phys;
    bi->pml4_phys       = pml4_phys;
    bi->hhdm_offset     = HHDM_OFFSET;

    /* ── 5. ExitBootServices (retry loop until key is fresh) ───────────── */
    serial_puts("Calling ExitBootServices...\n");
    EFI_STATUS status;
    for (;;) {
        mmap_size = mmap_size; /* keep size from initial query + slack */
        size_t cur_sz = mmap_size;
        /* Reset to full buffer capacity each iteration */
        cur_sz = 4096 * 16; /* 64 KiB — generous */
        status = BS->GetMemoryMap(&cur_sz,
                                  (EFI_MEMORY_DESCRIPTOR*)mmap_buf,
                                  &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(status)) hang();

        status = BS->ExitBootServices(ImageHandle, map_key);
        if (!EFI_ERROR(status)) {
            /* Translate mmap now (Boot Services gone but mmap_buf still valid) */
            translate_mmap(mmap_buf, cur_sz, desc_size, kernel_phys, bi);
            bi->phys_ceiling = phys_ceiling;
            break;
        }
        /* Key stale — retry; only GetMemoryMap allowed here */
    }

    serial_puts("ExitBootServices OK\n");

    /* ── 6. Switch to new page tables and jump to kernel ──────────────── */
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

    /* Jump to kernel _start at KERNEL_HH_BASE.
       kernel/entry.asm expects boot_info_t* in RDI (System V AMD64 ABI).
       Use inline asm for the jump so we control the calling convention exactly. */
    __asm__ volatile(
        "mov %0, %%rdi\n"
        "jmp *%1\n"
        :
        : "r"((uint64_t)bi), "r"(KERNEL_HH_BASE)
        : "rdi"
    );

    /* Should never reach here */
    hang();
    return EFI_SUCCESS;
}
