// MakaOS native libudev.  No sysfs, no netlink.  Enumeration walks a
// hardcoded device table (kernel exposes no discovery ioctl yet).
// The monitor path returns an eventfd that never signals — hot-plug
// isn't supported until the device-registry work (ledger #2).
//
// TODO(scalability-debt-ledger-#2): replace hardcoded table with
// kernel device-registry client; wire the monitor fd to real
// hot-plug events.

#include "libudev.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

// ── internal structs ────────────────────────────────────────────

struct udev {
    int refcnt;
};

struct udev_list_entry {
    char* name;
    char* value;   // NULL for syspath-only entries
    struct udev_list_entry* next;
};

struct udev_enumerate {
    struct udev* udev;
    char** match_subsystems;   // dynamic arrays — grow on add_match
    int    n_subsystems, cap_subsystems;
    char** match_sysnames;
    int    n_sysnames,   cap_sysnames;
    struct udev_list_entry* head;    // result list
};

struct udev_device {
    int    refcnt;
    struct udev* owner;      // back-ref for udev_device_get_udev
    char*  syspath;
    char*  sysname;
    char*  subsystem;
    char*  devnode;
    char*  action;           // NULL unless this is an event device
    dev_t  devnum;
    // properties / sysattrs are sparse — use two parallel linked lists.
    struct udev_list_entry* props;
    struct udev_list_entry* attrs;
    struct udev_device* parent;
};

struct udev_monitor {
    struct udev* udev;
    int          fd;                   // eventfd, never signalled
};

// ── hardcoded device descriptors ────────────────────────────────
//
// Each entry: syspath, sysname, subsystem, devnode, major/minor,
// a seat property ("seat0"), and (for DRM) a boot_vga sysattr.
// New devices go here until a kernel registry lands.

typedef struct {
    const char* syspath;
    const char* sysname;
    const char* subsystem;
    const char* devnode;
    unsigned    major;
    unsigned    minor;
    const char* seat;
    const char* boot_vga;     // NULL for non-DRM
    // libinput classifies input devices by udev properties; attach the
    // right combo for each event node.  NULL for non-input devices.
    const char* id_input;         // "1" on every input device
    const char* id_input_kind;    // "ID_INPUT_KEYBOARD" or "ID_INPUT_MOUSE" etc.
    const char* name;             // udev NAME= prop (used by libinput quirks)
} devdesc_t;

static const devdesc_t s_devices[] = {
    { "/sys/class/drm/card0",       "card0",   "drm",   "/dev/dri/card0",   226,   0, "seat0", "1",  NULL, NULL, NULL },
    { "/sys/class/input/event0",    "event0",  "input", "/dev/input/event0", 13,  64, "seat0", NULL, "1", "ID_INPUT_KEYBOARD", "MakaOS PS/2 Keyboard" },
    { "/sys/class/input/event1",    "event1",  "input", "/dev/input/event1", 13,  65, "seat0", NULL, "1", "ID_INPUT_MOUSE",    "MakaOS PS/2 Mouse"    },
};
static const int s_device_count = (int)(sizeof(s_devices) / sizeof(s_devices[0]));

// ── helpers ──────────────────────────────────────────────────────

static char* xstrdup(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static int match_subsystem(const struct udev_enumerate* en, const char* sub) {
    if (en->n_subsystems == 0) return 1;    // no filter = match
    if (!sub) return 0;
    for (int i = 0; i < en->n_subsystems; i++)
        if (strcmp(en->match_subsystems[i], sub) == 0) return 1;
    return 0;
}

// sysname matching — wlroots passes patterns like "card[0-9]*".
// We only support the two patterns it actually uses: exact strings
// (e.g. "seat0") and the "<prefix>[0-9]*" family where <prefix> is
// a literal.  Implement precisely enough for wlroots; document the
// narrowness here.
static int match_sysname(const struct udev_enumerate* en, const char* name) {
    if (en->n_sysnames == 0) return 1;
    if (!name) return 0;
    for (int i = 0; i < en->n_sysnames; i++) {
        const char* pat = en->match_sysnames[i];
        // look for a "[" to detect the "<prefix>[0-9]*" form
        const char* bracket = strchr(pat, '[');
        if (!bracket) {
            if (strcmp(pat, name) == 0) return 1;
        } else {
            size_t plen = (size_t)(bracket - pat);
            if (strncmp(pat, name, plen) == 0 && name[plen] >= '0' && name[plen] <= '9')
                return 1;
        }
    }
    return 0;
}

static void entry_free(struct udev_list_entry* e) {
    while (e) {
        struct udev_list_entry* n = e->next;
        free(e->name); free(e->value); free(e);
        e = n;
    }
}

static int str_append(char*** arr, int* n, int* cap, const char* s) {
    if (*n == *cap) {
        int ncap = *cap ? *cap * 2 : 4;
        char** na = (char**)realloc(*arr, (size_t)ncap * sizeof(char*));
        if (!na) return -1;
        *arr = na; *cap = ncap;
    }
    (*arr)[*n] = xstrdup(s);
    if (!(*arr)[*n]) return -1;
    (*n)++;
    return 0;
}

// ── udev lifecycle ──────────────────────────────────────────────

struct udev* udev_new(void)                        { struct udev* u = (struct udev*)calloc(1, sizeof(*u)); if (u) u->refcnt = 1; return u; }
struct udev* udev_ref(struct udev* u)              { if (u) u->refcnt++; return u; }
struct udev* udev_unref(struct udev* u)            { if (!u) return 0; if (--u->refcnt == 0) { free(u); } return 0; }

// ── enumerate ────────────────────────────────────────────────────

struct udev_enumerate* udev_enumerate_new(struct udev* u) {
    if (!u) return 0;
    struct udev_enumerate* e = (struct udev_enumerate*)calloc(1, sizeof(*e));
    if (e) e->udev = udev_ref(u);
    return e;
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate* e, const char* s) {
    if (!e || !s) return -EINVAL;
    return str_append(&e->match_subsystems, &e->n_subsystems, &e->cap_subsystems, s);
}

int udev_enumerate_add_match_sysname(struct udev_enumerate* e, const char* s) {
    if (!e || !s) return -EINVAL;
    return str_append(&e->match_sysnames, &e->n_sysnames, &e->cap_sysnames, s);
}

int udev_enumerate_add_match_property(struct udev_enumerate* e, const char* k, const char* v) {
    (void)e; (void)k; (void)v;
    return 0;   // wlroots doesn't actually use this for match, just setup
}

int udev_enumerate_scan_devices(struct udev_enumerate* e) {
    if (!e) return -EINVAL;
    entry_free(e->head); e->head = 0;
    struct udev_list_entry** tail = &e->head;
    for (int i = 0; i < s_device_count; i++) {
        if (!match_subsystem(e, s_devices[i].subsystem)) continue;
        if (!match_sysname(e,   s_devices[i].sysname))   continue;
        struct udev_list_entry* ent = (struct udev_list_entry*)calloc(1, sizeof(*ent));
        if (!ent) return -ENOMEM;
        ent->name = xstrdup(s_devices[i].syspath);
        *tail = ent;
        tail = &ent->next;
    }
    return 0;
}

struct udev_list_entry* udev_enumerate_get_list_entry(struct udev_enumerate* e) {
    return e ? e->head : 0;
}

struct udev_enumerate* udev_enumerate_unref(struct udev_enumerate* e) {
    if (!e) return 0;
    entry_free(e->head);
    for (int i = 0; i < e->n_subsystems; i++) free(e->match_subsystems[i]);
    for (int i = 0; i < e->n_sysnames;   i++) free(e->match_sysnames[i]);
    free(e->match_subsystems);
    free(e->match_sysnames);
    udev_unref(e->udev);
    free(e);
    return 0;
}

// ── list entry ──────────────────────────────────────────────────

struct udev_list_entry* udev_list_entry_get_next(struct udev_list_entry* e) { return e ? e->next : 0; }
const char*             udev_list_entry_get_name(struct udev_list_entry* e)  { return e ? e->name : 0; }
const char*             udev_list_entry_get_value(struct udev_list_entry* e) { return e ? e->value : 0; }

// ── device lookup ───────────────────────────────────────────────

static const devdesc_t* find_desc_by_syspath(const char* path) {
    if (!path) return 0;
    for (int i = 0; i < s_device_count; i++)
        if (strcmp(s_devices[i].syspath, path) == 0) return &s_devices[i];
    return 0;
}

static void entry_list_add(struct udev_list_entry** head, const char* name, const char* value) {
    if (!name) return;
    struct udev_list_entry* e = (struct udev_list_entry*)calloc(1, sizeof(*e));
    if (!e) return;
    e->name  = xstrdup(name);
    e->value = value ? xstrdup(value) : 0;
    e->next  = *head;
    *head = e;
}

struct udev_device* udev_device_new_from_syspath(struct udev* u, const char* syspath) {
    const devdesc_t* d = find_desc_by_syspath(syspath);
    if (!d) { errno = ENODEV; return 0; }
    struct udev_device* dev = (struct udev_device*)calloc(1, sizeof(*dev));
    if (!dev) return 0;
    dev->refcnt    = 1;
    dev->owner     = u ? udev_ref(u) : 0;
    dev->syspath   = xstrdup(d->syspath);
    dev->sysname   = xstrdup(d->sysname);
    dev->subsystem = xstrdup(d->subsystem);
    dev->devnode   = xstrdup(d->devnode);
    dev->action    = 0;
    dev->devnum    = ((dev_t)d->major << 8) | (d->minor & 0xFF);
    if (d->seat)          entry_list_add(&dev->props, "ID_SEAT",     d->seat);
    if (d->boot_vga)      entry_list_add(&dev->attrs, "boot_vga",    d->boot_vga);
    if (d->id_input)      entry_list_add(&dev->props, "ID_INPUT",    d->id_input);
    if (d->id_input_kind) entry_list_add(&dev->props, d->id_input_kind, "1");
    if (d->name)          entry_list_add(&dev->props, "NAME",        d->name);
    return dev;
}

struct udev_device* udev_device_new_from_devnum(struct udev* u, char type, dev_t devnum) {
    (void)type;
    for (int i = 0; i < s_device_count; i++) {
        dev_t dn = ((dev_t)s_devices[i].major << 8) | (s_devices[i].minor & 0xFF);
        if (dn == devnum) return udev_device_new_from_syspath(u, s_devices[i].syspath);
    }
    errno = ENODEV;
    return 0;
}

struct udev_device* udev_device_ref(struct udev_device* d) { if (d) d->refcnt++; return d; }

struct udev_device* udev_device_unref(struct udev_device* d) {
    if (!d) return 0;
    if (--d->refcnt > 0) return 0;
    free(d->syspath); free(d->sysname); free(d->subsystem);
    free(d->devnode); free(d->action);
    entry_free(d->props);
    entry_free(d->attrs);
    if (d->parent) udev_device_unref(d->parent);
    if (d->owner)  udev_unref(d->owner);
    free(d);
    return 0;
}

const char* udev_device_get_syspath(struct udev_device* d)   { return d ? d->syspath   : 0; }
const char* udev_device_get_sysname(struct udev_device* d)   { return d ? d->sysname   : 0; }
const char* udev_device_get_subsystem(struct udev_device* d) { return d ? d->subsystem : 0; }
const char* udev_device_get_devnode(struct udev_device* d)   { return d ? d->devnode   : 0; }
const char* udev_device_get_action(struct udev_device* d)    { return d ? d->action    : 0; }
dev_t       udev_device_get_devnum(struct udev_device* d)    { return d ? d->devnum    : 0; }

static const char* entry_find(struct udev_list_entry* head, const char* key) {
    for (; head; head = head->next)
        if (strcmp(head->name, key) == 0) return head->value;
    return 0;
}
const char* udev_device_get_property_value(struct udev_device* d, const char* k) { return (d && k) ? entry_find(d->props, k) : 0; }
const char* udev_device_get_sysattr_value(struct udev_device* d, const char* k)  { return (d && k) ? entry_find(d->attrs, k) : 0; }

struct udev_device* udev_device_get_parent_with_subsystem_devtype(
    struct udev_device* d, const char* subsystem, const char* devtype) {
    (void)devtype;
    if (!d) return 0;
    // MakaOS has no PCI topology exposed yet — synthesize a minimal
    // PCI parent for DRM cards so wlroots' boot_vga check resolves.
    if (d->parent) return d->parent;
    if (subsystem && strcmp(subsystem, "pci") == 0 &&
        d->subsystem && strcmp(d->subsystem, "drm") == 0) {
        struct udev_device* p = (struct udev_device*)calloc(1, sizeof(*p));
        if (!p) return 0;
        p->refcnt    = 1;
        p->owner     = d->owner ? udev_ref(d->owner) : 0;
        p->syspath   = xstrdup("/sys/bus/pci/devices/0000:00:02.0");
        p->sysname   = xstrdup("0000:00:02.0");
        p->subsystem = xstrdup("pci");
        entry_list_add(&p->attrs, "boot_vga", "1");
        d->parent = p;
        return p;
    }
    return 0;
}

// libinput walks the bus-hierarchy via get_parent during quirk
// matching.  We only synthesize one PCI parent (above); plain
// get_parent returns the cached chain or NULL at the root.
struct udev_device* udev_device_get_parent(struct udev_device* d) {
    return d ? d->parent : 0;
}

struct udev* udev_device_get_udev(struct udev_device* d) {
    return d ? d->owner : 0;
}

int udev_device_get_is_initialized(struct udev_device* d) {
    // All our synthesized devices spring to life fully populated;
    // there's no "waiting on kernel coldplug" state to report.
    return d ? 1 : 0;
}

struct udev_list_entry* udev_device_get_properties_list_entry(struct udev_device* d) {
    return d ? d->props : 0;
}

// ── monitor — stub ──────────────────────────────────────────────
// No hot-plug events until the kernel device registry lands
// (ledger #2).  The fd is an eventfd that never signals.

struct udev_monitor* udev_monitor_new_from_netlink(struct udev* u, const char* name) {
    (void)name;
    if (!u) return 0;
    struct udev_monitor* m = (struct udev_monitor*)calloc(1, sizeof(*m));
    if (!m) return 0;
    m->udev = udev_ref(u);
    m->fd   = eventfd(0, 0);
    return m;
}
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor* m, const char* s, const char* t) {
    (void)m; (void)s; (void)t; return 0;
}
int udev_monitor_enable_receiving(struct udev_monitor* m)    { (void)m; return 0; }
int udev_monitor_get_fd(struct udev_monitor* m)              { return m ? m->fd : -1; }
struct udev_device* udev_monitor_receive_device(struct udev_monitor* m) { (void)m; errno = EAGAIN; return 0; }
struct udev_monitor* udev_monitor_unref(struct udev_monitor* m) {
    if (!m) return 0;
    if (m->fd >= 0) close(m->fd);
    udev_unref(m->udev);
    free(m);
    return 0;
}
