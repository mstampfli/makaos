; ── AP real-mode → long-mode trampoline ────────────────────────────────
;
; Assembled as a flat binary (`nasm -f bin`) and copied at runtime into a
; reserved physical page below 1 MiB.  The INIT/SIPI sequence from the BSP
; parks an AP at CS:IP = (page_phys >> 12):0000, so execution always begins
; at byte 0 of this blob — everything here is position-independent.
;
; Control flow (three mode changes, all in one page):
;
;      AP reset → real mode → protected mode → long mode → kernel C
;      ─────────────────────────────────────────────────────────────
;      rm_entry            16-bit, CS base = page_phys
;         │
;         │ build prot-mode GDTR using (cs<<4)+gdt_off
;         │ set CR0.PE
;         ▼
;      pm_entry            32-bit, flat 4 GiB
;         │
;         │ enable PAE, load CR3 from startup_state, set EFER.LME,
;         │ set CR0.PG → long mode compatibility sub-mode
;         ▼
;      lm_entry            64-bit, still at low phys (identity map)
;         │
;         │ load final RSP, rdi = cpu_id, absolute jmp to kernel entry
;         ▼
;      cpu_init_ap         higher-half kernel C, never returns
;
; Startup state is at a fixed offset (STARTUP_OFF) so C code can poke it
; without parsing symbol tables.  One AP at a time reuses this page — the
; BSP sends INIT/SIPI to AP N, waits for it to bump g_num_cpus, then moves
; on — so there is no concurrency on the startup_state struct.
;
; Page layout:
;   0x0000  rm_entry      16-bit real-mode code
;   0x00??  pm_entry      32-bit protected-mode code
;   0x00??  lm_entry      64-bit long-mode code
;   0x00??  trampoline GDT (null / 32-bit code / 32-bit data / 64-bit code / 64-bit data)
;   0x00??  GDTR pointer (limit + base)
;   0x0F00  startup_state (cr3, stack, entry, cpu_id) — fixed offset
;
; Keep code below STARTUP_OFF (0x0F00) at all times.

bits 16
org  0

; ── Layout constants ─────────────────────────────────────────────────────
%define STARTUP_OFF      0x0F00          ; byte offset of startup_state inside the page
%define SS_CR3           (STARTUP_OFF + 0x00)  ; u64: kernel PML4 physical
%define SS_STACK         (STARTUP_OFF + 0x08)  ; u64: 64-bit stack top for this AP
%define SS_ENTRY         (STARTUP_OFF + 0x10)  ; u64: address of cpu_init_ap (higher-half)
%define SS_CPU_ID        (STARTUP_OFF + 0x18)  ; u32: logical CPU id (g_cpus index)

; CR0 / CR4 / EFER bits we touch
%define CR0_PE           (1 << 0)
%define CR0_MP           (1 << 1)
%define CR0_NE           (1 << 5)
%define CR0_WP           (1 << 16)
%define CR0_PG           (1 << 31)
%define CR4_PAE          (1 << 5)
%define CR4_PGE          (1 << 7)
%define MSR_EFER         0xC0000080
%define EFER_LME         (1 << 8)
%define EFER_NXE         (1 << 11)

; Trampoline GDT selectors
%define TR_CS32          0x08
%define TR_DS32          0x10
%define TR_CS64          0x18
%define TR_DS64          0x20


global ap_trampoline_start
ap_trampoline_start:

; ── Real-mode entry ──────────────────────────────────────────────────────
; CS = page_phys >> 4, IP = 0.  Interrupts are already off on an AP after
; SIPI, but make it explicit.  We must keep every memory access relative
; to CS because DS/ES/SS are uninitialised.
rm_entry:
    cli
    cld

    ; Align DS, ES, SS with CS so "[ds:label]" reads data at the same page.
    ; Use a temporary stack at the top of the trampoline page's first
    ; 512 bytes — we only need a few words for lgdt scratch.
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STARTUP_OFF        ; temp stack (grows down from startup_state; we never actually push)

    ; Compute page_phys = (cs << 4) into a 32-bit scratch so we can patch
    ; the GDTR base and the far-jump target on the fly.  NASM needs a32/o32
    ; prefixes to emit 32-bit operands in a 16-bit segment.
    xor eax, eax
    mov ax, cs
    shl eax, 4                 ; eax = page physical base

    ; Patch gdt_ptr.base = page_phys + gdt - ap_trampoline_start
    mov ebx, eax
    add ebx, gdt
    mov dword [ds:gdt_ptr + 2], ebx

    ; Patch the far-jump target (pm_entry) with the absolute phys address.
    ; Byte layout at far_jmp_pm is: 0x66 0xEA [imm32 offset] [imm16 selector]
    ; so the imm32 field starts at far_jmp_pm + 2.
    mov ebx, eax
    add ebx, pm_entry
    mov dword [ds:far_jmp_pm + 2], ebx

    ; Load the 32-bit GDT and flip CR0.PE.
    o32 lgdt [ds:gdt_ptr]

    mov eax, cr0
    or  eax, CR0_PE
    mov cr0, eax

    ; Far jump into 32-bit protected mode.  The target was patched above.
far_jmp_pm:
    db 0x66, 0xEA              ; o32 jmp far imm16:imm32
    dd 0x00000000              ; imm32 offset (patched)
    dw TR_CS32                 ; imm16 selector

; ── 32-bit protected-mode entry ──────────────────────────────────────────
bits 32
pm_entry:
    ; Load flat data segments.
    mov ax, TR_DS32
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; We are still running out of the trampoline page.  EIP is absolute,
    ; so compute our page base again for any memory access we need.
    ; Easiest: put esi = page_phys once and keep it for the rest of pm.
    ;
    ; We read startup_state for CR3 using absolute addresses (page_phys +
    ; offset).  But we don't know page_phys directly — the far jmp just
    ; landed us at pm_entry_phys.  Recover it via an immediate: the linker
    ; can't help because this is a flat blob, so we use a call/pop idiom.
    call .here
.here:
    pop  esi                   ; esi = phys address of label .here
    sub  esi, (.here - ap_trampoline_start)
    ; esi = page_phys (absolute phys address of the trampoline page)

    ; Enable PAE.
    mov eax, cr4
    or  eax, CR4_PAE | CR4_PGE
    mov cr4, eax

    ; Load CR3 from startup_state.
    mov eax, [esi + SS_CR3]    ; low dword
    mov cr3, eax               ; high dword is unused in 32-bit CR3; the
                               ; full 64-bit PML4 phys fits in 32 bits on
                               ; any machine we care about (PML4s live in
                               ; the low 4 GiB in our kernel).  If we ever
                               ; allow >4 GiB PML4s, patch here.

    ; Set EFER.LME and EFER.NXE (NXE so NX bits in PTEs are honoured).
    mov ecx, MSR_EFER
    rdmsr
    or  eax, EFER_LME | EFER_NXE
    wrmsr

    ; Enable paging + protection + WP + NE — transitions to long mode
    ; compatibility sub-mode.  The very next instruction fetch after mov
    ; cr0 is still 32-bit compat, which is what we want until the far jmp
    ; to CS64 switches to true 64-bit.
    mov eax, cr0
    or  eax, CR0_PG | CR0_PE | CR0_MP | CR0_NE | CR0_WP
    mov cr0, eax

    ; Far jump into 64-bit mode.  Absolute target = page_phys + lm_entry.
    ; Patch the m16:32 operand in-place, then indirect-far-jmp through it.
    lea eax, [esi + lm_entry]
    mov [esi + far_jmp_lm], eax

    jmp far [esi + far_jmp_lm]

; Far-jump operand for the 32→64 transition (offset is patched above).
align 4
far_jmp_lm:
    dd 0x00000000               ; absolute 32-bit offset (patched)
    dw TR_CS64

; ── 64-bit long-mode entry ───────────────────────────────────────────────
bits 64
lm_entry:
    ; Load 64-bit flat data segments.
    mov ax, TR_DS64
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; We are still executing at low physical (identity mapped via kernel
    ; PML4[0]).  Rebuild rsi = page_phys in 64-bit.  RIP-relative works
    ; here because the trampoline is contiguous and our code label is at
    ; a known low address — leaq gives us the absolute RIP-relative form.
    lea rsi, [rel ap_trampoline_start]
    ; rsi is now page_phys (the trampoline was copied to that page, and
    ; the linker wrote offsets relative to ap_trampoline_start=0, so
    ; RIP-relative + label = absolute physical address of the page).

    ; Install the AP's final kernel stack.
    mov rsp, [rsi + SS_STACK]

    ; Arg 1 (rdi) = cpu_id — cpu_init_ap reads it to know which g_cpus slot
    ; to claim.  Zero-extend the u32 field.
    mov edi, [rsi + SS_CPU_ID]

    ; Absolute 64-bit jump into the kernel C entry point (higher-half
    ; virtual address).  After this instruction we never touch the
    ; trampoline page again — the BSP is free to reuse it for the next AP.
    mov rax, [rsi + SS_ENTRY]
    jmp rax

; ── Trampoline GDT (used only while we're in this page) ─────────────────
; Kept trivially small: three flat descriptors so we can reach every
; byte of the first 4 GiB from prot mode, plus two 64-bit descriptors
; for the final long-mode transition.  The kernel's real GDT (loaded by
; cpu_init_ap via the shared g_gdt) takes over once we jump away.
align 8
gdt:
    dq 0x0000000000000000        ; 0x00: null
    dq 0x00CF9A000000FFFF        ; 0x08: 32-bit code, base=0, limit=4G
    dq 0x00CF92000000FFFF        ; 0x10: 32-bit data, base=0, limit=4G
    dq 0x00AF9A000000FFFF        ; 0x18: 64-bit code (L=1, D=0)
    dq 0x00AF92000000FFFF        ; 0x20: 64-bit data
gdt_end:

align 4
gdt_ptr:
    dw gdt_end - gdt - 1         ; limit
    dd 0x00000000                ; base (patched at runtime in rm_entry)

; ── Guard: make sure nothing above spills into the startup_state slot ──
%if ($ - ap_trampoline_start) > STARTUP_OFF
    %error "ap_trampoline code + data exceeds STARTUP_OFF — shrink it or move startup_state"
%endif

times STARTUP_OFF - ($ - ap_trampoline_start) db 0

; ── Startup state (at STARTUP_OFF) ──────────────────────────────────────
global ap_trampoline_startup_off
ap_trampoline_startup_off equ STARTUP_OFF

startup_state:
    dq 0         ; SS_CR3
    dq 0         ; SS_STACK
    dq 0         ; SS_ENTRY
    dd 0         ; SS_CPU_ID
    dd 0         ; padding

; Pad out to exactly one page so the whole thing is 4 KiB.
times 4096 - ($ - ap_trampoline_start) db 0

global ap_trampoline_end
ap_trampoline_end:
