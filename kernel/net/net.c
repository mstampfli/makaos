#include "net.h"
#include "virtio_net.h"
#include "eth.h"
#include "arp.h"
#include "tcp.h"
#include "skbuff.h"
#include "process.h"
#include "sched.h"
#include "irq_wait.h"
#include "common.h"
#include "kprintf.h"   // kprintf_atomic: whole-line-locked serial output (see below)
#include "tsc.h"

extern void sched_yield(void);

// Serial logging goes through kprintf_atomic (whole-line serial lock) so net
// log lines never interleave with the concurrent SMP kprintf output.  The old
// hand-rolled ser_putc wrote to COM1 unlocked and shredded lines mid-word.

// ── IP configuration ──────────────────────────────────────────────────────
// All values stored and returned in network byte order (big-endian).
//
// Default static config matching QEMU user-mode networking:
//   Our IP:  10.0.2.15    (0x0F02000A in LE memory, first byte = 10)
//   Gateway: 10.0.2.2
//   Mask:    255.255.255.0
//
// On a little-endian host, the uint32_t holding 10.0.2.15 in network
// byte order has byte[0]=10, byte[1]=0, byte[2]=2, byte[3]=15:
//   0x0F_02_00_0A   (LE host stores LSB first → byte[0]=0x0A=10 ✓)

#define IP4_BE(a,b,c,d)  ((uint32_t)(a) | ((uint32_t)(b)<<8) | \
                           ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))

// Start UNCONFIGURED.  The userspace /bin/net DHCP client runs during
// boot and calls net_ifconfig() with the lease assignment from the
// server.  Pre-DHCP, any sendto() on a SOCK_DGRAM inherits src_ip = 0
// (= 0.0.0.0) which is exactly what RFC 2131 requires for DISCOVER /
// REQUEST before a lease exists.  Previously we hardcoded 10.0.2.15
// (the QEMU SLIRP default) which broke DHCP: the server silently
// dropped our DISCOVER because the src IP claimed an address we don't
// yet own.
static uint32_t s_our_ip = 0;
static uint32_t s_gw_ip  = 0;
static uint32_t s_mask   = 0;
static uint32_t s_bcast  = IP4_BE(255, 255, 255, 255);   // limited broadcast
static int      s_ready  = 0;

#define NET_MAX_DNS 4
static uint32_t s_dns[NET_MAX_DNS];
static uint32_t s_dns_count = 0;

uint32_t net_our_ip(void)       { return s_our_ip; }
uint32_t net_gateway_ip(void)   { return s_gw_ip;  }
uint32_t net_subnet_mask(void)  { return s_mask;   }
uint32_t net_broadcast_ip(void) { return s_bcast;  }
int      net_ready(void)        { return s_ready;  }

void net_set_config(uint32_t our_ip_be, uint32_t gw_be, uint32_t mask_be) {
    s_our_ip = our_ip_be;
    s_gw_ip  = gw_be;
    s_mask   = mask_be;
    // Broadcast = (ip & mask) | ~mask
    s_bcast  = (our_ip_be & mask_be) | (~mask_be);

    // Log to serial so headless test runs can grep this line.  kprintf_atomic
    // emits the WHOLE line under the serial lock: this runs concurrently with
    // the SMP selftest kprintf storm, and the old char-by-char ser_putc path
    // was unlocked, so the line got shredded mid-word and the grep gate
    // intermittently missed it (the interface was configured either way).
    uint8_t* ip = (uint8_t*)&s_our_ip;
    uint8_t* gw = (uint8_t*)&s_gw_ip;
    kprintf_atomic("[net] ifconfig IP %u.%u.%u.%u GW %u.%u.%u.%u\n",
                   (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3],
                   (unsigned)gw[0], (unsigned)gw[1], (unsigned)gw[2], (unsigned)gw[3]);
}

uint32_t net_get_dns(uint32_t* out, uint32_t max) {
    uint32_t n = s_dns_count < max ? s_dns_count : max;
    __builtin_memcpy(out, s_dns, n * sizeof(uint32_t));
    return n;
}

void net_set_dns(const uint32_t* servers, uint32_t count) {
    if (count > NET_MAX_DNS) count = NET_MAX_DNS;
    __builtin_memcpy(s_dns, servers, count * sizeof(uint32_t));
    s_dns_count = count;
}

// ── Net receive thread ────────────────────────────────────────────────────
// Wakes on virtio RX IRQ, drains the ring, dispatches each frame.  The TCP
// retransmit/reap timer runs on the dedicated net_tcp_timer_thread ONLY, so
// tcp_timer_tick keeps a single writer (its snd_nxt/rexmit/rto_deadline updates
// are lockless -- see tcp.c).  This thread must NOT also call it: doing so ran
// tcp_timer_tick from two threads and raced a pcb's sequence accounting.

// TCP timer thread: fires tcp_timer_tick() roughly every 200 ms so
// retransmits and TIME_WAIT expiry don't depend on incoming traffic.
static void net_tcp_timer_thread(void) {
    for (;;) {
        uint64_t until = tsc_read_ns() + 200ull * 1000ull * 1000ull;
        while (tsc_read_ns() < until) sched_yield();
        tcp_timer_tick();
    }
}

static void net_rx_thread(void) {
    for (;;) {
        // Drain the entire RX ring before sleeping.
        skbuff_t* skb;
        while (virtio_net_rx_poll(&skb))
            eth_recv(skb);

        // Sleep until the virtio-net MSI fires.
        // The IRQ handler calls irq_notify(g_virtio_net_irq) on RX so we
        // wake immediately.  irq_wait returns instantly if an IRQ is pending.
        // Fall back to a yield if the IRQ slot isn't assigned yet.
        if (g_virtio_net_irq != 0xFFu)
            irq_wait(g_virtio_net_irq);
        else
            sched_yield();
    }
}

// ── Initialisation ────────────────────────────────────────────────────────

int net_init(void) {
    if (!virtio_net_init()) {
        kprintf_atomic("[net] no virtio-net device\n");
        return 0;
    }

    // Print our IP config.  Atomic whole-line output (see net_set_config).
    uint8_t* ip = (uint8_t*)&s_our_ip;
    uint8_t* gw = (uint8_t*)&s_gw_ip;
    kprintf_atomic("[net] IP %u.%u.%u.%u GW %u.%u.%u.%u\n",
                   (unsigned)ip[0], (unsigned)ip[1], (unsigned)ip[2], (unsigned)ip[3],
                   (unsigned)gw[0], (unsigned)gw[1], (unsigned)gw[2], (unsigned)gw[3]);

    const uint8_t* mac = virtio_net_mac();
    kprintf_atomic("[net] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                   (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                   (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);

    // Gratuitous ARP — prime neighbours' caches.
    arp_announce();

    // Spawn the RX dispatch kernel thread.
    task_t* t = task_create_kthread(net_rx_thread, pid_alloc());
    if (!t) {
        kprintf_atomic("[net] rx thread alloc failed\n");
        return 0;
    }
    sched_add(t);

    // Spawn a dedicated TCP timer thread.  net_rx_thread only ticks when
    // packets arrive; that's a problem for retransmits — if the initial
    // SYN fails or its SYN-ACK never comes back, there's no RX traffic to
    // drive the tick, and the handshake wedges forever.
    task_t* tt = task_create_kthread(net_tcp_timer_thread, pid_alloc());
    if (tt) sched_add(tt);

    s_ready = 1;
    kprintf_atomic("[net] ready\n");
    return 1;
}
