/*
 * ext_fskit.h — ext2/3/4(libext2fs)到 FSKit 的桥接层
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 与 ntfs_fskit.h 平行:共享 nfsk_blockio/nfsk_stat/nfsk_statfs 类型,
 * 卷句柄为 extfsk_volume。文件标识 = ext inode 号(root=2)。
 * 返回约定:0 成功,负值 -errno。
 */

#ifndef USBRW_EXT_FSKIT_H
#define USBRW_EXT_FSKIT_H

#include "ntfs_fskit.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct extfsk_volume extfsk_volume;

/* 挂载:rw=1 读写;脏卷(needs_recovery)自动重放日志(e2fsck 同源
 * ext2fs_run_ext3_journal)。失败返回 NULL,err 填原因。 */
extfsk_volume *extfsk_open(const nfsk_blockio *io, int rw,
                           char *err, size_t errlen);
void extfsk_close(extfsk_volume *v, int force);

int extfsk_fsstat(extfsk_volume *v, nfsk_statfs *out);

/* 文件操作(inode 号即标识)——随桥接推进逐个实现 */
int extfsk_getattr(extfsk_volume *v, uint64_t ino, nfsk_stat *out);
int extfsk_lookup(extfsk_volume *v, uint64_t dir_ino, const char *name,
                  uint64_t *out_ino, nfsk_stat *out_stat);
int64_t extfsk_read(extfsk_volume *v, uint64_t ino, uint64_t offset,
                    size_t len, void *buf);

const char *extfsk_last_error(void);

/* ---- 完整操作集(第二梯队,随桥接推进) ---- */
typedef int (*extfsk_dirent_cb)(void *ctx, uint64_t ino, const char *name,
                                const nfsk_stat *st, uint64_t next_cookie);
int extfsk_readdir(extfsk_volume *v, uint64_t dir_ino, uint64_t *io_cookie,
                   extfsk_dirent_cb cb, void *ctx);
int64_t extfsk_write(extfsk_volume *v, uint64_t ino, uint64_t offset,
                     size_t len, const void *buf);
int extfsk_create(extfsk_volume *v, uint64_t dir_ino, const char *name,
                  char type, uint32_t mode, uint32_t uid, uint32_t gid,
                  uint64_t *out_ino);
int extfsk_unlink(extfsk_volume *v, uint64_t dir_ino, const char *name,
                  uint64_t ino);
int extfsk_rename(extfsk_volume *v, uint64_t old_dir, const char *old_name,
                  uint64_t new_dir, const char *new_name);
int extfsk_symlink(extfsk_volume *v, uint64_t dir_ino, const char *name,
                   const char *target, uint64_t *out_ino);
int extfsk_readlink(extfsk_volume *v, uint64_t ino, char *buf, size_t buflen);
int extfsk_fsync(extfsk_volume *v);
int extfsk_truncate(extfsk_volume *v, uint64_t ino, uint64_t newsize);


#ifdef __cplusplus
}
#endif

#endif /* USBRW_EXT_FSKIT_H */
