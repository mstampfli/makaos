// linux/fs.h - filesystem/block-device ioctls (Linux-compatible subset).
//
// Only the constants portable code actually references are provided. MakaOS
// does not implement these ioctls yet, so the calls fail at runtime and the
// caller (e.g. OpenJDK sizing a block-device file) falls back.

#ifndef _LINUX_FS_H
#define _LINUX_FS_H 1

#include <sys/ioctl.h>
#include <sys/types.h>

// Return the size of a block device in bytes.
#ifndef BLKGETSIZE64
#define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif

#endif /* linux/fs.h */
