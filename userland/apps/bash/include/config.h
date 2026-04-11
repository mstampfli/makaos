/* config.h — MakaOS bash 5.2 configuration.
 * This replaces autoconf's generated config.h.
 * Lives in the repo so bash can be recompiled at any time.
 */

#ifndef _CONFIG_H_
#define _CONFIG_H_

/* ── Bash feature flags ─────────────────────────────────────────────── */
#define ALIAS                   1
#define PUSHD_AND_POPD          1
#define BRACE_EXPANSION         1
#define READLINE                1
#define BANG_HISTORY            1
#define HISTORY                 1
#define HELP_BUILTIN            1
#define RESTRICTED_SHELL        1
#define PROMPT_STRING_DECODE    1
#define SELECT_COMMAND          1
#define COMMAND_TIMING          1
#define ARRAY_VARS              1
#define DPAREN_ARITHMETIC       1
#define EXTENDED_GLOB           1
#define COND_COMMAND            1
#define COND_REGEXP             1
#define ARITH_FOR_COMMAND       1
#define CASEMOD_ATTRS           1
#define CASEMOD_EXPANSIONS      1
#define COPROCESS_SUPPORT       1
#define PROGRAMMABLE_COMPLETION 1

/* Job control */
#define JOB_CONTROL             1
/* Disabled features (no kernel/libc support yet) */
/* #undef PROCESS_SUBSTITUTION */
/* #undef NETWORK_REDIRECTIONS */
/* #undef DEBUGGER */
/* #undef STRICT_POSIX */
/* #undef AFS */
/* #undef ENABLE_NLS */
#define NO_MULTIBYTE_SUPPORT    1
#define EXTGLOB_DEFAULT         0
#define GLOBASCII_DEFAULT       0
#define FUNCTION_IMPORT         1

/* ── config-top.h inline (user-settable options) ────────────────────── */
#define CONTINUE_AFTER_KILL_ERROR
#define BREAK_COMPLAINS
#define CD_COMPLAINS
#define BUFFERED_INPUT
#define ONESHOT
#define V9_ECHO
#define DONT_REPORT_SIGPIPE
#define DONT_REPORT_SIGTERM
#define KSH_COMPATIBLE_SELECT

#ifndef DEFAULT_PATH_VALUE
#define DEFAULT_PATH_VALUE "/bin:/usr/bin:/sbin:/usr/sbin"
#endif
#ifndef STANDARD_UTILS_PATH
#define STANDARD_UTILS_PATH "/bin:/usr/bin:/sbin:/usr/sbin"
#endif

#define PPROMPT "\\s-\\v\\$ "
#define SPROMPT "> "
#define DEFAULT_BASHRC "~/.bashrc"
#define CHECKWINSIZE_DEFAULT    1
#define OPTIMIZE_SEQUENTIAL_ARRAY_ASSIGNMENT 1
#define CHECKHASH_DEFAULT       0
#define EVALNEST_MAX            0
#define SOURCENEST_MAX          0
#define USE_MKTEMP
#define USE_MKSTEMP
#define USE_MKDTEMP
#define OLDPWD_CHECK_DIRECTORY  1
#define HISTEXPAND_DEFAULT      1
#define ASSOC_KVPAIR_ASSIGNMENT 1
#define CASEMOD_TOGGLECASE
#define CASEMOD_CAPCASE
#ifndef MULTIPLE_COPROCS
#define MULTIPLE_COPROCS        0
#endif

/* ── Type aliases ───────────────────────────────────────────────────── */
#define GETGROUPS_T gid_t

/* ── Conf strings (conftypes.h) ─────────────────────────────────────── */
#define CONF_HOSTTYPE  "x86_64"
#define CONF_OSTYPE    "MakaOS"
#define CONF_MACHTYPE  "x86_64-unknown-makaos"
#define CONF_VENDOR    "unknown"

#ifndef DEFAULT_LOADABLE_BUILTINS_PATH
#define DEFAULT_LOADABLE_BUILTINS_PATH ""
#endif

#define DEFAULT_COMPAT_LEVEL 52

/* ── Compiler characteristics ───────────────────────────────────────── */
#define HAVE_STRINGIZE          1
#define HAVE_LONG_DOUBLE        1
#define HAVE_LONG_LONG_INT      1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define PROTOTYPES              1
#define __PROTOTYPES            1

/* ── sizeof types (x86_64) ──────────────────────────────────────────── */
#define SIZEOF_INT       4
#define SIZEOF_LONG      8
#define SIZEOF_CHAR_P    8
#define SIZEOF_SIZE_T    8
#define SIZEOF_DOUBLE    8
#define SIZEOF_INTMAX_T  8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_WCHAR_T   4

/* ── System type definitions ────────────────────────────────────────── */
/* Bash-specific integer type aliases */
typedef short          bits16_t;
typedef unsigned short u_bits16_t;
typedef int            bits32_t;
typedef unsigned int   u_bits32_t;
typedef long           bits64_t;
typedef unsigned int   u_int;
typedef unsigned long  u_long;

#define GETPGRP_VOID            1
#define HAVE_HASH_BANG_EXEC     1
#define CAN_REDEFINE_GETENV     1
#define HAVE_POSIX_SIGNALS      1

#define RETSIGTYPE void
#define VOID_SIGHANDLER         1

/* ── Presence of C library functions ────────────────────────────────── */
#define HAVE_ASPRINTF           1
#define HAVE_BCOPY              1
#define HAVE_BZERO              1
#define HAVE_DUP2               1
#define HAVE_FCNTL              1
#define HAVE_FNMATCH            1
#define HAVE_GETCWD             1
#define HAVE_GETHOSTNAME        1
#define HAVE_GETPWENT           1
#define HAVE_GETPWNAM           1
#define HAVE_GETPWUID           1
#define HAVE_GETRLIMIT          1
#define HAVE_GETRUSAGE          1
#define HAVE_ISASCII            1
#define HAVE_ISBLANK            1
#define HAVE_ISGRAPH            1
#define HAVE_ISPRINT            1
#define HAVE_ISSPACE            1
#define HAVE_ISXDIGIT           1
#define HAVE_KILL               1
#define HAVE_KILLPG             1
#define HAVE_LSTAT              1
#define HAVE_MEMMOVE            1
#define HAVE_MEMSET             1
#define HAVE_MKDTEMP            1
#define HAVE_MKSTEMP            1
#define HAVE_MUNMAP             1
#define HAVE_PATHCONF           1
#define HAVE_PUTENV             1
#define HAVE_RAISE              1
#define HAVE_REGCOMP            1
#define HAVE_REGEXEC            1
#define HAVE_RENAME             1
#define HAVE_SELECT             1
#define HAVE_SETENV             1
#define HAVE_SETVBUF            1
#define HAVE_SIGSETJMP          1
#define HAVE_POSIX_SIGSETJMP    1
#define HAVE_SNPRINTF           1
#define HAVE_STRCASECMP         1
#define HAVE_STRCASESTR         1
#define HAVE_STRCHR             1
#define HAVE_STRCHRNUL          1
#define HAVE_STRCOLL            1
#define HAVE_STRERROR           1
#define HAVE_STRFTIME           1
#define HAVE_STRNLEN            1
#define HAVE_STRPBRK            1
#define HAVE_STRSTR             1
#define HAVE_STRTOD             1
#define HAVE_STRTOL             1
#define HAVE_STRTOLL            1
#define HAVE_STRTOUL            1
#define HAVE_STRTOULL           1
#define HAVE_STRTOIMAX          1
#define HAVE_STRTOUMAX          1
#define HAVE_STRDUP             1
#define HAVE_STPCPY             1
#define HAVE_STRCSPN            1
#define HAVE_STRSIGNAL          1
#define HAVE_SYSCONF            1
#define HAVE_TCGETATTR          1
#define HAVE_TCGETPGRP          1
#define HAVE_TIMES              1
#define HAVE_TTYNAME            1
#define HAVE_TZSET              1
#define HAVE_UNAME              1
#define HAVE_UNSETENV           1
#define HAVE_VASPRINTF          1
#define HAVE_VPRINTF            1
#define HAVE_VSNPRINTF          1
#define HAVE_WAITPID            1
#define HAVE_SETITIMER          1
#define HAVE_GETTIMEOFDAY       1

#define HAVE_DECL_STRTOL        1
#define HAVE_DECL_STRTOLL       1
#define HAVE_DECL_STRTOUL       1
#define HAVE_DECL_STRTOULL      1
#define HAVE_DECL_STRTOIMAX     1
#define HAVE_DECL_STRTOUMAX     1
#define HAVE_DECL_STRSIGNAL     1
#define HAVE_DECL_PRINTF        1
#define HAVE_DECL_STRCPY        1
#define HAVE_DECL_CONFSTR       1

#define HAVE_PSELECT            1

/* Functions we do NOT have */
/* #undef HAVE_CHOWN */
/* #undef HAVE_CONFSTR — we have the decl but it's a stub */
/* #undef HAVE_DLOPEN */
/* #undef HAVE_DLCLOSE */
/* #undef HAVE_DLSYM */
/* #undef HAVE_GETADDRINFO */
/* #undef HAVE_GETDTABLESIZE */
/* #undef HAVE_GETGROUPS */
/* #undef HAVE_GETPAGESIZE */
/* #undef HAVE_GETPEERNAME */
/* #undef HAVE_GETTIMEOFDAY */
/* #undef HAVE_ICONV */
/* #undef HAVE_LOCALE_CHARSET */
/* #undef HAVE_MBRLEN */
/* #undef HAVE_MBRTOWC */
/* #undef HAVE_MKFIFO */
/* #undef HAVE_PSELECT */
/* #undef HAVE_RANDOM */
/* #undef HAVE_READLINK */
/* #undef HAVE_SBRK */
/* #undef HAVE_SETLOCALE */
/* #undef HAVE_SIGINTERRUPT */
/* #undef HAVE_SYSLOG */
/* #undef HAVE_ULIMIT */
/* #undef HAVE_WAIT3 */
/* #undef HAVE_WCRTOMB */
/* #undef HAVE_WCSCOLL */
/* #undef HAVE_WCSDUP */
/* #undef HAVE_WCTYPE */
/* #undef HAVE_WCSWIDTH */
/* #undef HAVE_WCWIDTH */
/* #undef HAVE_ARC4RANDOM */
/* #undef HAVE_GETENTROPY */
/* #undef HAVE_GETRANDOM */
/* #undef HAVE_DPRINTF */
/* #undef USING_BASH_MALLOC */

/* ── Presence of system headers ─────────────────────────────────────── */
#define HAVE_DIRENT_H           1
#define HAVE_ERRNO_H            1
#define HAVE_FCNTL_H            1
#define HAVE_INTTYPES_H         1
#define HAVE_LIMITS_H           1
#define HAVE_PWD_H              1
#define HAVE_REGEX_H            1
#define HAVE_STDBOOL_H          1
#define HAVE_STDARG_H           1
#define HAVE_STDDEF_H           1
#define HAVE_STDINT_H           1
#define HAVE_STDLIB_H           1
#define HAVE_STRING_H           1
#define HAVE_STRINGS_H          1
#define HAVE_MEMORY_H           1
#define HAVE_SYS_FILE_H         1
#define HAVE_SYS_IOCTL_H        1
#define HAVE_SYS_STAT_H         1
#define HAVE_SYS_TIME_H         1
#define HAVE_SYS_TIMES_H        1
#define HAVE_SYS_TYPES_H        1
#define HAVE_SYS_WAIT_H         1
#define HAVE_TERMCAP_H          1
#define HAVE_TERMIOS_H          1
#define HAVE_UNISTD_H           1

/* Headers we do NOT have */
/* #undef HAVE_ARPA_INET_H */
/* #undef HAVE_DLFCN_H */
/* #undef HAVE_GRP_H */
/* #undef HAVE_LANGINFO_H */
/* #undef HAVE_LOCALE_H */
/* #undef HAVE_NETDB_H */
/* #undef HAVE_NETINET_IN_H */
/* #undef HAVE_SYS_MMAN_H */
/* #undef HAVE_SYS_PARAM_H */
/* #undef HAVE_SYS_RESOURCE_H */
/* #undef HAVE_SYS_SELECT_H */
/* #undef HAVE_SYS_SOCKET_H */
/* #undef HAVE_SYSLOG_H */
/* #undef HAVE_WCHAR_H */
/* #undef HAVE_WCTYPE_H */

/* ── Struct characteristics ─────────────────────────────────────────── */
#define HAVE_STRUCT_DIRENT_D_INO        1
#define HAVE_STRUCT_STAT_ST_BLOCKS      1
#define HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC 1
#define TYPEOF_STRUCT_STAT_ST_ATIM_IS_STRUCT_TIMESPEC 1
#define HAVE_STRUCT_TIMESPEC            1
#define HAVE_TIMEVAL                    1
#define GWINSZ_IN_SYS_IOCTL            1
#define STRUCT_WINSIZE_IN_SYS_IOCTL     1
#define FIONREAD_IN_SYS_IOCTL           1

#define HAVE_GETPW_DECLS                1
#define WEXITSTATUS_OFFSET              8

/* ── System identification ──────────────────────────────────────────── */
/* #undef WORDS_BIGENDIAN */
#define STACK_DIRECTION (-1)

/* POSIX version — needed by readline to select termios tty driver */
#ifndef _POSIX_VERSION
#define _POSIX_VERSION 200809L
#endif

/* ── Mail ───────────────────────────────────────────────────────────── */
#define DEFAULT_MAIL_DIRECTORY "/var/mail"

/* ── Misc ───────────────────────────────────────────────────────────── */
#define HAVE_MBSTATE_T          0
/* #undef HAVE_LOCALE_CHARSET */
/* #undef HAVE_LANGINFO_CODESET */
#define NAMED_PIPES_MISSING     1
/* #undef HAVE_DEV_FD */
/* #undef HAVE_DEV_STDIN */
/* #undef TRANSLATABLE_STRINGS */

/* PARAMS macro — bash uses this everywhere for K&R compat.
   Normally defined in include/stdc.h but needed before posixtime.h. */
#if !defined(PARAMS)
#define PARAMS(protos) protos
#endif

/* Need the config-bot.h from bash source */
#include "config-bot.h"

#endif /* _CONFIG_H_ */
