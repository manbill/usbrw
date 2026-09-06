/*
 * ntfs_fskit.h — USBRW FSKit 扩展与 libntfs-3g 之间的 C 桥接层
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 设计:
 *   Swift 侧(FSKit)持有块设备资源,通过 nfsk_blockio 回调把扇区级
 *   读写能力交给 C 侧;C 侧用它实现 struct ntfs_device_operations,
 *   再经 ntfs_device_mount() 获得完整的 ntfs_volume 能力。
 *   所有文件操作以 MFT 记录号(u64)作为文件标识,与 FSKit 的
 *   FSItem.Identifier 一一对应。
 */

#ifndef USBRW_NTFS_FSKIT_H
#define USBRW_NTFS_FSKIT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回错误约定:0 成功,负值为 -errno。 */

/* Swift 提供的块 I/O 能力(封装 FSBlockDeviceResource)。 */
typedef struct nfsk_blockio {
    void *ctx;   /* Swift 侧上下文,原样传回回调 */
    int (*read)(void *ctx, uint64_t offset, size_t len, void *buf);
    int (*write)(void *ctx, uint64_t offset, size_t len, const void *buf);
    int (*sync)(void *ctx);
    int readonly; /* 1: 只读挂载 */
    /* 底层 I/O 的对齐量子(字节,2 的幂;0 视为 512)。
     * macOS 原始字符设备与 FSBlockDeviceResource 都要求 offset/len
     * 按块对齐;libntfs 会发出字节粒度请求(如 $MFT:$BITMAP 扫描在
     * ofs=3 处 pread),由 C 桥接做对齐扩展/RMW。 */
    uint32_t block_size;
    uint64_t block_count; /* 设备总块数(block_size 为单位;0=未知不截尾) */
} nfsk_blockio;

typedef struct nfsk_volume nfsk_volume; /* 封装 ntfs_volume + ntfs_device */

/* ---------------- 卷生命周期 ---------------- */

/* 挂载。recover=1 时置 NTFS_MNT_RECOVER(重放 $LogFile)。 */
nfsk_volume *nfsk_open(const nfsk_blockio *io, int recover,
                       char *err, size_t errlen);

/* 带选项挂载。opts 位: */
#define NFSK_OPEN_REMOVE_HIBERFILE 0x1 /* 休眠卷救援:IGNORE_HIBERFILE
                                          挂载并删除 hiberfil.sys
                                          (Windows 未保存会话丢失) */
#define NFSK_OPEN_WINDOWS_NAMES    0x2 /* 严格文件名(拒绝 \ / : * ? " < > |
                                          与 CON/PRN/AUX/NUL/COM1-9/LPT1-9) */
#define NFSK_OPEN_HIDE_HID_FILES   0x4 /* 不显示 Windows 隐藏文件 */
#define NFSK_OPEN_HIDE_DOT_FILES   0x8 /* 点开头文件标记为 Windows 隐藏 */
#define NFSK_OPEN_NOATIME          0x10 /* 读取不更新 atime(减写放大) */
#define NFSK_OPEN_NORECOVER        0x20 /* 不做 $LogFile 日志恢复(高级) */
#define NFSK_OPEN_NO_COMPRESSION   0x40 /* 禁用 NTFS 压缩属性读写(默认开) */
#define NFSK_OPEN_SHOW_SYS_FILES   0x80 /* 显示 NTFS 系统元数据文件(默认藏) */
nfsk_volume *nfsk_open_ex(const nfsk_blockio *io, int recover, int opts,
                          char *err, size_t errlen);
void nfsk_close(nfsk_volume *v, int force);

/* ---------------- 卷信息(statfs) ---------------- */

typedef struct nfsk_statfs {
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t bsize;      /* 卷簇大小 */
    uint64_t total_files;
    uint64_t free_files; /* 空闲 MFT 记录数,未知时为 UINT64_MAX */
    char name[256];      /* 卷名(NTFS 卷标签),UTF-8 */
} nfsk_statfs;

int nfsk_fsstat(nfsk_volume *v, nfsk_statfs *out);

/* ---------------- 文件属性 ---------------- */

typedef struct nfsk_stat {
    uint64_t mft;        /* MFT 记录号(文件标识) */
    uint32_t mode;       /* st_mode */
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t alloc_size;
    uint64_t links;
    int64_t atime, mtime, ctime, btime; /* Unix 秒 */
    uint32_t flags;      /* chflags */
} nfsk_stat;

int nfsk_getattr(nfsk_volume *v, uint64_t mft, nfsk_stat *out);
/* which 位掩码:1 mode 2 uid 4 gid 8 size 16 mtime 32 flags
 * 64 atime 128 btime(NTFS 原生创建时间)。
 * 注:无 usermapping 时 mode/uid/gid 为静默 no-op。 */
int nfsk_setattr(nfsk_volume *v, uint64_t mft, const nfsk_stat *st,
                 uint32_t which);

/* ---------------- 目录操作 ---------------- */

/* 在目录 dir_mft 中按名字查找;UTF-8。找不到返回 -ENOENT。 */
int nfsk_lookup(nfsk_volume *v, uint64_t dir_mft, const char *name,
                uint64_t *out_mft, nfsk_stat *out_stat);

typedef int (*nfsk_dirent_cb)(void *ctx, uint64_t mft, const char *name,
                              const nfsk_stat *st, uint64_t next_cookie);
/* 从 cookie 开始枚举;每命中一项调用 cb,next_cookie 传回给 FSKit。
 * want_attrs=0 时 st 仅保证 mft/mode 有效(避免逐项开 inode)。
 * cb 返回 0 继续,1 packer 已满(中止但不算错误),负值 -errno。
 * 返回 0 正常结束,负值 -errno。 */
int nfsk_readdir(nfsk_volume *v, uint64_t dir_mft, uint64_t *io_cookie,
                 int want_attrs, nfsk_dirent_cb cb, void *ctx);

/* ---------------- 创建/删除/改名 ---------------- */

/* type: 'f' 普通文件 'd' 目录 */
int nfsk_create(nfsk_volume *v, uint64_t dir_mft, const char *name,
                char type, uint32_t mode, uint32_t uid, uint32_t gid,
                uint64_t *out_mft);
int nfsk_unlink(nfsk_volume *v, uint64_t dir_mft, const char *name,
                uint64_t mft);
int nfsk_rename(nfsk_volume *v, uint64_t old_dir, const char *old_name,
                uint64_t new_dir, const char *new_name);
int nfsk_link(nfsk_volume *v, uint64_t mft, uint64_t new_dir,
              const char *new_name);
int nfsk_symlink(nfsk_volume *v, uint64_t dir_mft, const char *name,
                 const char *target, uint32_t uid, uint32_t gid,
                 uint64_t *out_mft);
int nfsk_readlink(nfsk_volume *v, uint64_t mft, char *buf, size_t buflen);

/* ---------------- 数据读写 ---------------- */

/* 返回读取字节数,负值 -errno。 */
int64_t nfsk_read(nfsk_volume *v, uint64_t mft, uint64_t offset,
                  size_t len, void *buf);
int64_t nfsk_write(nfsk_volume *v, uint64_t mft, uint64_t offset,
                   size_t len, const void *buf);
int nfsk_fsync(nfsk_volume *v, uint64_t mft);
int nfsk_truncate(nfsk_volume *v, uint64_t mft, uint64_t newsize);

/* ---------------- 扩展属性 ---------------- */

int64_t nfsk_getxattr(nfsk_volume *v, uint64_t mft, const char *name,
                      void *buf, size_t buflen);
int nfsk_setxattr(nfsk_volume *v, uint64_t mft, const char *name,
                  const void *buf, size_t buflen);
int nfsk_removexattr(nfsk_volume *v, uint64_t mft, const char *name);
typedef int (*nfsk_xattr_cb)(void *ctx, const char *name);
int nfsk_listxattr(nfsk_volume *v, uint64_t mft, nfsk_xattr_cb cb, void *ctx);

/* ---------------- 维护 ---------------- */

int nfsk_sync_volume(nfsk_volume *v);
const char *nfsk_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* USBRW_NTFS_FSKIT_H */
