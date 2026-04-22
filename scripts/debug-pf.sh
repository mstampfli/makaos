#!/usr/bin/env bash
# ── debug-pf.sh — attach GDB to a live QEMU, catch user-mode PFs ─────
#
# Usage:  ./scripts/debug-pf.sh &     (background it)
#         ./run.sh                    (start QEMU as usual)
#         [inside QEMU] dwl -s foot   (trigger the PF)
#
# Script waits until QEMU's gdb stub is listening on localhost:1234,
# then attaches with a pre-armed breakpoint at isr14_page_fault.
# When a PF fires, GDB dumps:
#   - CR2 + error code + interrupt frame
#   - kernel backtrace
#   - extracted user RIP/RSP/RBP/RAX/RCX
#   - 16 qwords from user RSP
#   - rbp chain (up to 12 frames) — this walks the user-space stack
#   - 10 insns of disassembly at user RIP
# into /tmp/gdb-pf.log.  Continues afterwards so the kernel's real
# handler runs (PF-KILL or CoW resolution).
#
# Also loads symbols for build/sysroot/usr/bin/dwl at its link base
# 0x400000 so `bt` resolves user frames when we break inside a
# syscall-from-dwl.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LOG=/tmp/gdb-pf.log
CMD=/tmp/gdb-pf.cmd

cat > "$CMD" <<'GDB'
set pagination off
set confirm off
set print pretty on
set logging overwrite on
set logging file /tmp/gdb-pf.log
set logging enabled on

file build/kernel.elf
target remote localhost:1234

add-symbol-file build/sysroot/usr/bin/dwl 0x400000

# interrupt_frame_t layout (kernel/arch/x86_64/idt.h):
#   0: ip    8: cs   16: flags   24: sp   32: ss
# That's the CPU-pushed IRET frame — only 5 qwords.
# PUSH_GPRS ran BEFORE the frame alloc in isr_common_entry, so the
# user GPRs sit above the frame on the kernel stack:
#   frame+40 rax  +48 rbx  +56 rcx  +64 rdx  +72 rbp
#   +80 rsi  +88 rdi  +96 r8  +104 r9  +112 r10  +120 r11
#   +128 r12 +136 r13 +144 r14 +152 r15

break isr14_page_fault
commands
  printf "\n########## PF CAUGHT ##########\n"
  printf "CR2        = 0x%016lx\n", $cr2
  printf "error code = 0x%lx\n",   $rsi
  printf "  P=%d  W/R=%d  U/S=%d  RSVD=%d  I/D=%d\n", \
    !!($rsi & 1), !!($rsi & 2), !!($rsi & 4), !!($rsi & 8), !!($rsi & 16)
  printf "frame @ 0x%lx\n\n", $rdi
  printf "---- KERNEL BACKTRACE:\n"
  bt 20
  set $urip = *(unsigned long*)($rdi + 0)
  set $ucs  = *(unsigned long*)($rdi + 8)
  set $ufl  = *(unsigned long*)($rdi + 16)
  set $ursp = *(unsigned long*)($rdi + 24)
  set $uss  = *(unsigned long*)($rdi + 32)
  set $urax = *(unsigned long*)($rdi + 40)
  set $urbx = *(unsigned long*)($rdi + 48)
  set $urcx = *(unsigned long*)($rdi + 56)
  set $urdx = *(unsigned long*)($rdi + 64)
  set $urbp = *(unsigned long*)($rdi + 72)
  set $ursi = *(unsigned long*)($rdi + 80)
  set $urdi = *(unsigned long*)($rdi + 88)
  printf "\n---- USER REGS (from interrupt_frame + GPR save area):\n"
  printf "RIP = 0x%lx  CS=0x%lx  RFLAGS=0x%lx\n", $urip, $ucs, $ufl
  printf "RSP = 0x%lx  SS=0x%lx\n", $ursp, $uss
  printf "RAX = 0x%-16lx  RBX = 0x%-16lx  RCX = 0x%lx\n", $urax, $urbx, $urcx
  printf "RDX = 0x%-16lx  RBP = 0x%-16lx  RSI = 0x%lx\n", $urdx, $urbp, $ursi
  printf "RDI = 0x%lx\n", $urdi
  printf "\n---- USER STACK (16 qwords from RSP=0x%lx):\n", $ursp
  x/16gx $ursp
  printf "\n---- USER RBP CHAIN:\n"
  set $fp = $urbp
  set $i = 0
  while $i < 12 && $fp != 0 && (($fp >> 47) == 0)
    set $ret = *(unsigned long*)($fp + 8)
    printf "  [%d] fp=0x%lx  ret=0x%lx\n", $i, (unsigned long)$fp, $ret
    set $fp = *(unsigned long*)$fp
    set $i = $i + 1
  end
  printf "\n---- USER RIP disassembly (10 insns):\n"
  x/10i $urip
  printf "########## END ##########\n\n"
  continue
end

continue
GDB

echo "[debug-pf] waiting for QEMU gdb stub on localhost:1234 …"
until nc -z localhost 1234 2>/dev/null; do sleep 0.5; done
echo "[debug-pf] attaching; breakpoint armed.  Log: $LOG"

exec gdb --batch -x "$CMD"
