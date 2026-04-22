#!/usr/bin/env bash
# ── debug-pf.sh — attach GDB to live QEMU, catch user-mode PF-KILLs ──
#
# Usage:  ./scripts/debug-pf.sh &   (then trigger the reproducer)
#
# Waits for QEMU's gdb stub on :1234, attaches, arms a breakpoint at
# the `kill:` label in isr14_page_fault (so CoW and demand-paging
# resolve quietly without triggering us).  When a fatal PF fires,
# dumps CR2, error code, raw interrupt-frame contents, extracted
# user GPRs (using the layout from isr_stubs.asm PUSH_GPRS — frame
# +40 rax, +48 rbx, +56 rcx, +64 rdx, +72 rbp, +80 rsi, +88 rdi),
# user-RIP raw bytes + disassembly, CR2-neighbourhood bytes, and
# then continues so the kernel's real handler runs.
#
# Output is APPENDED to /tmp/gdb-pf.log.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LOG=/tmp/gdb-pf.log
CMD=/tmp/gdb-pf.cmd

cat > "$CMD" <<'GDB'
set pagination off
set confirm off
set print pretty on
set logging overwrite off
set logging file /tmp/gdb-pf.log
set logging enabled on
set remotetimeout 30

file build/kernel.elf
target remote localhost:1234

add-symbol-file build/sysroot/usr/bin/dwl 0x400000

# Break at the `kill:` label (first statement inside the kill block).
break kernel/mm/vmm.c:867
commands
  set $ff = f
  printf "\n########## PF-KILL CAUGHT ##########\n"
  printf "CR2        = 0x%016lx\n", fault_addr
  printf "error code = 0x%lx (P=%d W=%d U=%d RSVD=%d I/D=%d)\n", \
    ec, !!(ec & 1), !!(ec & 2), !!(ec & 4), !!(ec & 8), !!(ec & 16)
  printf "frame @ 0x%lx\n\n", (unsigned long)$ff
  printf "---- RAW FRAME DUMP (24 qwords from $ff):\n"
  x/24gx $ff
  printf "\n---- KERNEL REGS AT KILL-POINT:\n"
  info registers rax rbx rcx rdx rdi rsi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15
  printf "\n---- QEMU LIVE REGISTERS (monitor):\n"
  monitor info registers
  printf "\n---- KERNEL BACKTRACE:\n"
  bt 20
  set $urip = *(unsigned long*)($ff + 0)
  set $ucs  = *(unsigned long*)($ff + 8)
  set $ufl  = *(unsigned long*)($ff + 16)
  set $ursp = *(unsigned long*)($ff + 24)
  set $uss  = *(unsigned long*)($ff + 32)
  set $urax = *(unsigned long*)($ff + 40)
  set $urbx = *(unsigned long*)($ff + 48)
  set $urcx = *(unsigned long*)($ff + 56)
  set $urdx = *(unsigned long*)($ff + 64)
  set $urbp = *(unsigned long*)($ff + 72)
  set $ursi = *(unsigned long*)($ff + 80)
  set $urdi = *(unsigned long*)($ff + 88)
  set $ur8  = *(unsigned long*)($ff + 96)
  set $ur9  = *(unsigned long*)($ff + 104)
  set $ur10 = *(unsigned long*)($ff + 112)
  set $ur11 = *(unsigned long*)($ff + 120)
  set $ur12 = *(unsigned long*)($ff + 128)
  set $ur13 = *(unsigned long*)($ff + 136)
  set $ur14 = *(unsigned long*)($ff + 144)
  set $ur15 = *(unsigned long*)($ff + 152)
  printf "\n---- USER REGS:\n"
  printf "RIP = 0x%lx  CS=0x%lx  RFLAGS=0x%lx\n", $urip, $ucs, $ufl
  printf "RSP = 0x%lx  SS=0x%lx\n", $ursp, $uss
  printf "RAX = 0x%-18lx RBX = 0x%-18lx RCX = 0x%lx\n", $urax, $urbx, $urcx
  printf "RDX = 0x%-18lx RBP = 0x%-18lx RSI = 0x%lx\n", $urdx, $urbp, $ursi
  printf "RDI = 0x%-18lx R8  = 0x%-18lx R9  = 0x%lx\n", $urdi, $ur8, $ur9
  printf "R10 = 0x%-18lx R11 = 0x%-18lx R12 = 0x%lx\n", $ur10, $ur11, $ur12
  printf "R13 = 0x%-18lx R14 = 0x%-18lx R15 = 0x%lx\n", $ur13, $ur14, $ur15
  printf "\n---- USER STACK (32 qwords from RSP=0x%lx):\n", $ursp
  x/32gx $ursp
  printf "\n---- USER RIP raw bytes (32 B):\n"
  x/32bx $urip
  printf "\n---- USER RIP disassembly (12 insns):\n"
  x/12i $urip
  printf "\n---- CR2 context (32 B around 0x%lx):\n", (unsigned long)fault_addr
  x/32bx ((unsigned long)fault_addr - 16)
  printf "########## END ##########\n\n"
  continue
end

continue
GDB

echo "[debug-pf] waiting for QEMU gdb stub on :1234 ..."
until nc -z localhost 1234 2>/dev/null; do sleep 0.5; done
echo "[debug-pf] attaching; breakpoint armed.  Log: $LOG"

exec gdb --batch -nx -x "$CMD"
