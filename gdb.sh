#!/usr/bin/env bash
# Connect GDB to the running QEMU instance (started with -gdb tcp::1234).
# Usage: ./gdb.sh
#
# Useful commands once inside GDB:
#   c                        continue
#   Ctrl+C                   pause / interrupt
#   bt                       backtrace of current task
#   info threads             list all tasks (QEMU exposes vCPUs as threads)
#   p g_current->pid         current scheduled task pid
#   p *g_current             full current task struct
#   b net_rx_thread          breakpoint on net rx thread
#   b virtio_net_rx_poll     breakpoint on virtio RX poll
#   b socket_deliver_udp     breakpoint on UDP delivery
#   b udp_recv               breakpoint on UDP receive path
#   x/40xb 0xADDR            dump 40 hex bytes at address
#   monitor info network     QEMU monitor: show network state
#   monitor info irq         QEMU monitor: show IRQ stats

KERNEL_ELF="build/kernel.elf"

if [ ! -f "$KERNEL_ELF" ]; then
    echo "Error: $KERNEL_ELF not found. Run build.sh first."
    exit 1
fi

exec gdb "$KERNEL_ELF" \
    -ex "set architecture i386:x86-64" \
    -ex "set disassembly-flavor intel" \
    -ex "target remote localhost:1234" \
    -ex "set scheduler-locking off" \
    -ex "set pagination off" \
    -ex "echo \n=== MakaOS GDB connected ===\n" \
    -ex "echo Type 'c' to continue, Ctrl+C to pause.\n\n"
