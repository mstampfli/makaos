#ifndef _LIBSEAT_H
#define _LIBSEAT_H

#include <stdarg.h>

struct libseat;

struct libseat_seat_listener {
    void (*enable_seat)(struct libseat* seat, void* userdata);
    void (*disable_seat)(struct libseat* seat, void* userdata);
};

struct libseat* libseat_open_seat(const struct libseat_seat_listener* listener, void* userdata);
int             libseat_disable_seat(struct libseat* seat);
int             libseat_close_seat(struct libseat* seat);

int         libseat_open_device(struct libseat* seat, const char* path, int* fd);
int         libseat_close_device(struct libseat* seat, int device_id);
const char* libseat_seat_name(struct libseat* seat);

int libseat_switch_session(struct libseat* seat, int session);
int libseat_get_fd(struct libseat* seat);
int libseat_dispatch(struct libseat* seat, int timeout);

enum libseat_log_level {
    LIBSEAT_LOG_LEVEL_SILENT = 0,
    LIBSEAT_LOG_LEVEL_ERROR  = 1,
    LIBSEAT_LOG_LEVEL_INFO   = 2,
    LIBSEAT_LOG_LEVEL_DEBUG  = 3,
    LIBSEAT_LOG_LEVEL_LAST,
};
typedef void (*libseat_log_func)(enum libseat_log_level level, const char* fmt, va_list args);
void libseat_set_log_handler(libseat_log_func fn);
void libseat_set_log_level(enum libseat_log_level level);

#endif
