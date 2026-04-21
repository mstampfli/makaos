#ifndef _LIBUDEV_H
#define _LIBUDEV_H

#include <stddef.h>
#include <sys/types.h>

struct udev;
struct udev_device;
struct udev_enumerate;
struct udev_monitor;
struct udev_list_entry;

struct udev*        udev_new(void);
struct udev*        udev_ref(struct udev* u);
struct udev*        udev_unref(struct udev* u);

struct udev_enumerate* udev_enumerate_new(struct udev* u);
struct udev_enumerate* udev_enumerate_unref(struct udev_enumerate* e);
int  udev_enumerate_add_match_subsystem(struct udev_enumerate* e, const char* subsystem);
int  udev_enumerate_add_match_sysname(struct udev_enumerate* e, const char* sysname);
int  udev_enumerate_add_match_property(struct udev_enumerate* e, const char* prop, const char* val);
int  udev_enumerate_scan_devices(struct udev_enumerate* e);
struct udev_list_entry* udev_enumerate_get_list_entry(struct udev_enumerate* e);

struct udev_list_entry* udev_list_entry_get_next(struct udev_list_entry* e);
const char*             udev_list_entry_get_name(struct udev_list_entry* e);
const char*             udev_list_entry_get_value(struct udev_list_entry* e);

#define udev_list_entry_foreach(entry, first) \
    for (entry = first; entry != (struct udev_list_entry*)0; \
         entry = udev_list_entry_get_next(entry))

struct udev_device* udev_device_new_from_syspath(struct udev* u, const char* syspath);
struct udev_device* udev_device_new_from_devnum(struct udev* u, char type, dev_t devnum);
struct udev_device* udev_device_ref(struct udev_device* d);
struct udev_device* udev_device_unref(struct udev_device* d);
const char*         udev_device_get_syspath(struct udev_device* d);
const char*         udev_device_get_sysname(struct udev_device* d);
const char*         udev_device_get_subsystem(struct udev_device* d);
const char*         udev_device_get_devnode(struct udev_device* d);
const char*         udev_device_get_action(struct udev_device* d);
const char*         udev_device_get_property_value(struct udev_device* d, const char* key);
const char*         udev_device_get_sysattr_value(struct udev_device* d, const char* attr);
dev_t               udev_device_get_devnum(struct udev_device* d);
struct udev_device* udev_device_get_parent_with_subsystem_devtype(
    struct udev_device* d, const char* subsystem, const char* devtype);

struct udev_monitor* udev_monitor_new_from_netlink(struct udev* u, const char* name);
struct udev_monitor* udev_monitor_unref(struct udev_monitor* m);
int  udev_monitor_filter_add_match_subsystem_devtype(
    struct udev_monitor* m, const char* subsystem, const char* devtype);
int  udev_monitor_enable_receiving(struct udev_monitor* m);
int  udev_monitor_get_fd(struct udev_monitor* m);
struct udev_device* udev_monitor_receive_device(struct udev_monitor* m);

#endif
