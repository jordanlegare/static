/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef STATICBUS_IOCTL_H
#define STATICBUS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define STATICBUS_ABI_VERSION 1U
#define STATICBUS_IOC_MAGIC 'S'

/* Snapshot counters and queue occupancy for /dev/staticbus0. */
struct staticbus_stats {
    __u32 abi_version;
    __u32 buffer_capacity;
    __u32 buffer_used;
    __u32 reserved;
    __u64 bytes_written;
    __u64 bytes_read;
};

#define STATICBUS_IOC_GET_STATS _IOR(STATICBUS_IOC_MAGIC, 0x01, struct staticbus_stats)

#endif /* STATICBUS_IOCTL_H */
