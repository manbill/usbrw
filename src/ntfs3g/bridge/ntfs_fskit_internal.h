/*
 * ntfs_fskit_internal.h — 桥接层两个 .c 之间共享的内部类型。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 不进 Swift 桥接头:这里会引入 libntfs-3g 的内部结构。
 */

#ifndef USBRW_NTFS_FSKIT_INTERNAL_H
#define USBRW_NTFS_FSKIT_INTERNAL_H

#include "ntfs_fskit.h"

#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>

struct nfsk_volume {
    ntfs_volume *vol;      /* libntfs-3g 卷句柄(nfsk_open 后有效) */
    struct ntfs_device *dev;
    nfsk_blockio io;       /* 自持一份回调副本 */
    uint32_t opts;         /* NFSK_OPEN_* 挂载选项位(见 ntfs_fskit.h) */
};

/* 桥接全局递归锁(ntfs_fskit.c 定义;FSKit 并发派发,libntfs-3g
 * 非线程安全,所有公开 nfsk_* 操作经此串行化)。 */
void nfsk_lock(void);
void nfsk_unlock(void);

#endif /* USBRW_NTFS_FSKIT_INTERNAL_H */
