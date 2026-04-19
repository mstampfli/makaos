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

extern void sched_yield(void);

// ── Serial helpers (inline, no serial.h needed) ───────────────────────────

static void ser_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, (uint8_t)c);
}
static void ser_puts(const char* s) {
    for (; *s; s++) ser_putc(*s);
}
static void ser_put_dec(uint32_t v) {
    if (v >= 10) ser_put_dec(v / 10);
    ser_putc((char)('0' + v % 10));
}
static void ser_put_hex8(uint8_t v) {
    static const char h[] = "0123456789abcdef";
    ser_putc(h[v >> 4]);
    ser_putc(h[v & 0xF]);
}

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

    // Log to serial so headless test runs can grep this line.
    uint8_t* ip = (uint8_t*)&s_our_ip;
    uint8_t* gw = (uint8_t*)&s_gw_ip;
    ser_puts("[net] ifconfig IP ");
    ser_put_dec(ip[0]); ser_putc('.');
    ser_put_dec(ip[1]); ser_putc('.');
    ser_put_dec(ip[2]); ser_putc('.');
    ser_put_dec(ip[3]);
    ser_puts(" GW ");
    ser_put_dec(gw[0]); ser_putc('.');
    ser_put_dec(gw[1]); ser_putc('.');
    ser_put_dec(gw[2]); ser_putc('.');
    ser_put_dec(gw[3]); ser_putc('\n');
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
// Wakes on virtio RX IRQ, drains the ring, dispatches each frame.
// Also fires tcp_timer_tick() every ~10 wakeups (loose timer, ±200 ms is fine
// since TCP RTO minimum is 1 s).

static void net_rx_thread(void) {
    uint32_t tick = 0;

    for (;;) {
        // Drain the entire RX ring before sleeping.
        skbuff_t* skb;
        while (virtio_net_rx_poll(&skb))
            eth_recv(skb);

        if (++tick >= 10u) {
            tick = 0;
            tcp_timer_tick();
        }

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
        ser_puts("[net] no virtio-net device\n");
        return 0;
    }

    // Print our IP config.
    uint8_t* ip = (uint8_t*)&s_our_ip;
    uint8_t* gw = (uint8_t*)&s_gw_ip;
    ser_puts("[net] IP ");
    ser_put_dec(ip[0]); ser_putc('.');
    ser_put_dec(ip[1]); ser_putc('.');
    ser_put_dec(ip[2]); ser_putc('.');
    ser_put_dec(ip[3]);
    ser_puts(" GW ");
    ser_put_dec(gw[0]); ser_putc('.');
    ser_put_dec(gw[1]); ser_putc('.');
    ser_put_dec(gw[2]); ser_putc('.');
    ser_put_dec(gw[3]); ser_putc('\n');

    const uint8_t* mac = virtio_net_mac();
    ser_puts("[net] MAC ");
    for (int i = 0; i < 6; i++) {
        ser_put_hex8(mac[i]);
        if (i < 5) ser_putc(':');
    }
    ser_putc('\n');

    // Gratuitous ARP — prime neighbours' caches.
    arp_announce();

    // Spawn the RX dispatch kernel thread.
    task_t* t = task_create_kthread(net_rx_thread, pid_alloc());
    if (!t) {
        ser_puts("[net] rx thread alloc failed\n");
        return 0;
    }
    sched_add(t);

    s_ready = 1;
    ser_puts("[net] ready\n");
    return 1;
}
