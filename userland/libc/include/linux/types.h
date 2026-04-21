// MakaOS shim for <linux/types.h> — libdrm, libinput and similar
// kernel-uAPI consumers include this for fixed-width integer typedefs
// in the Linux namespace (__u32 etc.).  We just map to stdint.h.

#pragma once
#include <stdint.h>

// __BITS_PER_LONG steers linux/input.h's struct input_event layout
// selection (timeval vs 64-bit counter).  x86_64 has 64-bit long.
#ifndef __BITS_PER_LONG
#define __BITS_PER_LONG 64
#endif

typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;
typedef unsigned long    __kernel_size_t;
typedef long             __kernel_ssize_t;
typedef long             __kernel_long_t;
typedef unsigned long    __kernel_ulong_t;
typedef unsigned int     __kernel_uid32_t;
typedef unsigned int     __kernel_gid32_t;
typedef long             __kernel_off_t;
typedef unsigned short   __le16;
typedef unsigned short   __be16;
typedef unsigned int     __le32;
typedef unsigned int     __be32;
typedef unsigned long    __le64;
typedef unsigned long    __be64;
