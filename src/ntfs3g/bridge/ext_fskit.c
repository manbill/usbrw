//
//  ext_fskit.c — libext2fs 文件操作到 FSKit 桥接(ext2/3/4 读写)
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  实现模式对照 ntfs_fskit.c;引擎语义移植自 e2fsprogs 的 fuse2fs.c。
//  数据通道:自定义 io_manager 把块读写转发到 nfsk_blockio
//  (FSBlockDeviceResource,与 NTFS 同一通道;ext 请求本身按
//  block_size 对齐,满足底层 512 对齐要求)。
//  日志:needs_recovery 时 ext2fs_run_ext3_journal 重放
//  (e2fsck 同源路径,规避 fuse2fs j_tail_sequence 缺陷)。
//

#include "ext_fskit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ext2fs/ext2fs.h>
#include <et/com_err.h>

/* 日志重放(ext_journal_replay.c 内 debugfs/journal.c 提供;
 * 手工原型以避免把 jbd2 兼容头拖进本文件) */
extern errcode_t ext2fs_run_ext3_journal(ext2_filsys *fsp);

static char g_last_error[512];

/* 字节粒度请求的弹跳缓冲(e2fsprogs blocksize 上限 64K) */
static unsigned char g_byte_buf[65536];

const char *extfsk_last_error(void)
{
    return g_last_error;
}

struct extfsk_volume {
    ext2_filsys fs;
};

/* ---------------- io_manager:块读写 → nfsk_blockio ---------------- */

/* libext2fs 的 open 回调拿不到上下文(ext2fs_open 只传 name)——
 * fuse2fs 同款做法:挂载前设置全局通道,open 后清零。 */
static const nfsk_blockio *g_io_channel_src;

static struct struct_io_manager ext_usbrw_manager_impl;

static errcode_t ext_io_open(const char *name, int flags, io_channel *ch)
{
    io_channel c;
    (void)flags;
    if (!g_io_channel_src)
        return EXT2_ET_BAD_DEVICE_NAME;
    c = calloc(1, sizeof(struct struct_io_channel));
    if (!c) return EXT2_ET_NO_MEMORY;
    c->magic = EXT2_ET_MAGIC_IO_CHANNEL;
    c->manager = &ext_usbrw_manager_impl;
    c->name = strdup(name ? name : "usbrw-ext");
    c->private_data = (void *)g_io_channel_src;
    c->block_size = 1024;
    *ch = c;
    return 0;
}

static errcode_t ext_io_close(io_channel channel)
{
    if (!channel) return 0;
    free(channel->name);
    free(channel);
    return 0;
}

static errcode_t ext_io_set_blocksize(io_channel channel, int blocksize)
{
    channel->block_size = blocksize;
    return 0;
}

static errcode_t ext_io_read_byte(io_channel channel, unsigned long offset,
                                  int count, void *data);
static errcode_t ext_io_write_byte(io_channel channel, unsigned long offset,
                                   int count, const void *data);

static errcode_t ext_io_read_blk64(io_channel channel, unsigned long long block,
                                   int count, void *data)
{
    const nfsk_blockio *io = channel->private_data;
    if (!io || !io->read) return EXT2_ET_BAD_DEVICE_NAME;
    size_t bs = (size_t)channel->block_size;
    int64_t off = (int64_t)(block * bs);
    /* 上游 unix_io 语义:count<0 = 读 -count 字节(超级块等字节粒度请求,
     * 走字节弹跳);count>=0 = 读 count 个块。
     * ⚠️ 曾把负数也乘 bs:超级块读(-1024)放大成 1MB 写入 1024 字节缓冲,
     * 每次挂载都在堆上越界(2026-09-03 日志重放调试中现形)。 */
    if (count < 0)
        return ext_io_read_byte(channel, (unsigned long)off, -count, data);
    size_t len = (size_t)count * bs;
    int rc = io->read(io->ctx, (uint64_t)off, len, data);
    if (rc) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "ext io read rc=%d @%lld len=%zu", rc, (long long)off, len);
        return EIO;
    }
    return 0;
}

static errcode_t ext_io_write_blk64(io_channel channel, unsigned long long block,
                                    int count, const void *data)
{
    const nfsk_blockio *io = channel->private_data;
    if (!io || !io->write) return EXT2_ET_BAD_DEVICE_NAME;
    if (io->readonly) return EROFS;
    size_t bs = (size_t)channel->block_size;
    int64_t off = (int64_t)(block * bs);
    /* count<0 = 写 -count 字节(RMW 弹跳,同读路径语义) */
    if (count < 0)
        return ext_io_write_byte(channel, (unsigned long)off, -count, data);
    size_t len = (size_t)count * bs;
    int rc = io->write(io->ctx, (uint64_t)off, len, data);
    if (rc) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "ext io write rc=%d @%lld len=%zu", rc, (long long)off, len);
        return EIO;
    }
    return 0;
}

static errcode_t ext_io_read_blk(io_channel channel, unsigned long block,
                                 int count, void *data)
{
    return ext_io_read_blk64(channel, block, count, data);
}

static errcode_t ext_io_write_blk(io_channel channel, unsigned long block,
                                  int count, const void *data)
{
    return ext_io_write_blk64(channel, block, count, data);
}

static errcode_t ext_io_flush(io_channel channel)
{
    const nfsk_blockio *io = channel->private_data;
    if (io && io->sync) io->sync(io->ctx);
    return 0;
}

static errcode_t ext_io_read_byte(io_channel channel, unsigned long offset,
                                  int count, void *data)
{
    /* 逐字节(仅 superblock 探测期用);按 block_size 弹跳 */
    unsigned char *p = data;
    int left = count;
    while (left > 0) {
        unsigned long blk = offset / channel->block_size;
        unsigned skip = offset % channel->block_size;
        int n = channel->block_size - skip;
        if (n > left) n = left;
        errcode_t rc = ext_io_read_blk64(channel, blk, 1, g_byte_buf);
        if (rc) return rc;
        memcpy(p, g_byte_buf + skip, n);
        p += n; offset += n; left -= n;
    }
    return 0;
}

static errcode_t ext_io_write_byte(io_channel channel, unsigned long offset,
                                   int count, const void *data)
{
    const unsigned char *p = data;
    int left = count;
    while (left > 0) {
        unsigned long blk = offset / channel->block_size;
        unsigned skip = offset % channel->block_size;
        int n = channel->block_size - skip;
        if (n > left) n = left;
        errcode_t rc = ext_io_read_blk64(channel, blk, 1, g_byte_buf);
        if (rc) return rc;
        memcpy(g_byte_buf + skip, p, n);
        rc = ext_io_write_blk64(channel, blk, 1, g_byte_buf);
        if (rc) return rc;
        p += n; offset += n; left -= n;
    }
    return 0;
}

io_manager ext_usbrw_manager = &ext_usbrw_manager_impl;

static struct struct_io_manager ext_usbrw_manager_impl = {
    .magic = EXT2_ET_MAGIC_IO_MANAGER,
    .name = "usbrw-fskit",
    .open = ext_io_open,
    .close = ext_io_close,
    .set_blksize = ext_io_set_blocksize,
    .read_blk = ext_io_read_blk,
    .write_blk = ext_io_write_blk,
    .flush = ext_io_flush,
    .write_byte = ext_io_write_byte,
    .set_option = NULL,
    .get_stats = NULL,
    .read_blk64 = ext_io_read_blk64,
    .write_blk64 = ext_io_write_blk64,
    .discard = NULL,
    .cache_readahead = NULL,
    .zeroout = NULL,
};

/* ---------------- 挂载/卸载 ---------------- */

extfsk_volume *extfsk_open(const nfsk_blockio *io, int rw,
                           char *err, size_t errlen)
{
    if (!io || !io->read) {
        snprintf(err, errlen, "bad io callbacks");
        return NULL;
    }

    g_io_channel_src = io;
    ext2_filsys fs = NULL;
    int flags = rw ? EXT2_FLAG_RW : EXT2_FLAG_FORCE; /* FORCE=只读打开脏卷 */
    if (!rw) flags |= EXT2_FLAG_FORCE;

    errcode_t rc = ext2fs_open("usbrw-ext", flags, 0, 0,
                               ext_usbrw_manager, &fs);
    g_io_channel_src = NULL;
    if (rc) {
        snprintf(err, errlen, "ext2fs_open 失败: %s", error_message(rc));
        return NULL;
    }

    /* 脏卷(needs_recovery):先重放日志再交给上层——e2fsck 同源 recover
     * 路径(见 ext_journal_replay.c);fuse2fs 自带 replay 有 #120 缺陷,
     * 本路径即 Linux fsck 的实际重放逻辑。
     * ext2fs_run_ext3_journal 内部会 ext2fs_free 后按原 io_manager 重开,
     * 重开时 ext_io_open 仍要读 g_io_channel_src,故调用期间保持挂接。 */
    if (rw && (fs->super->s_feature_incompat &
               EXT3_FEATURE_INCOMPAT_RECOVER)) {
        errcode_t jrc;
        g_io_channel_src = io;
        jrc = ext2fs_run_ext3_journal(&fs);
        g_io_channel_src = NULL;
        if (jrc || !fs) {
            snprintf(err, errlen,
                     "日志重放失败(%s):请只读挂载备份数据,并在终端运行 "
                     "e2fsck -f 修复", jrc ? error_message(jrc) : "重开失败");
            if (fs) ext2fs_close(fs);
            return NULL;
        }
    }

    extfsk_volume *v = calloc(1, sizeof(*v));
    if (!v) {
        ext2fs_close(fs);
        snprintf(err, errlen, "oom");
        return NULL;
    }
    v->fs = fs;

    /* 位图读入(fuse2fs 同款:new_inode/write/punch 都依赖内存位图)。
     * 位图校验失败(重放后仍不一致=卷有真实损坏)绝不能 IGNORE_CSUM
     * 强行挂载——按坏位图分配块会造成真损坏;拒绝并指向 e2fsck。 */
    if (rw) {
        rc = ext2fs_read_bitmaps(fs);
        if (rc) {
            if (rc == EXT2_ET_BLOCK_BITMAP_CSUM_INVALID ||
                rc == EXT2_ET_INODE_BITMAP_CSUM_INVALID) {
                snprintf(err, errlen,
                         "位图校验不一致(卷存在损坏):请在终端运行 "
                         "e2fsck -f /dev/对应设备 修复后再读写挂载"
                         "(只读挂载不受影响)");
            } else {
                snprintf(err, errlen, "位图读入失败: %s", error_message(rc));
            }
            ext2fs_close(fs);
            free(v);
            return NULL;
        }
    }

    /* RW 首次挂载:写 superblock 的挂载时间/状态(与 e2fsck -p 同款) */
    if (rw) {
        fs->super->s_mnt_count++;
        fs->super->s_mtime = time(NULL);
        ext2fs_mark_super_dirty(fs);
        ext2fs_flush(fs);
    }
    return v;
}

void extfsk_close(extfsk_volume *v, int force)
{
    (void)force;
    if (!v) return;
    if (v->fs) ext2fs_close(v->fs);
    free(v);
}

int extfsk_fsstat(extfsk_volume *v, nfsk_statfs *out)
{
    if (!v || !v->fs || !out) return -EINVAL;
    ext2_filsys fs = v->fs;
    blk64_t free_blocks = ext2fs_free_blocks_count(fs->super);
    blk64_t total = ext2fs_blocks_count(fs->super);
    __u32 bsize = EXT2_BLOCK_SIZE(fs->super);

    memset(out, 0, sizeof(*out));
    out->bsize = bsize;
    out->total_bytes = (uint64_t)total * bsize;
    out->free_bytes = (uint64_t)free_blocks * bsize;
    out->total_files = fs->super->s_inodes_count;
    out->free_files = fs->super->s_free_inodes_count;
    if (fs->super->s_volume_name[0])
        strlcpy(out->name, fs->super->s_volume_name, sizeof(out->name));
    return 0;
}

/* ---------------- 文件操作(按 fuse2fs 语义,逐个落地) ---------------- */

static int ext_fill_stat(ext2_filsys fs, struct ext2_inode *inode,
                         uint64_t ino, nfsk_stat *out)
{
    memset(out, 0, sizeof(*out));
    out->mft = ino;                 /* 复用字段名:通用文件标识 */
    out->links = inode->i_links_count;
    out->uid = inode->i_uid;
    out->gid = inode->i_gid;
    out->size = EXT2_I_SIZE(inode);
    out->alloc_size = (uint64_t)inode->i_blocks * 512;
    switch (inode->i_mode & LINUX_S_IFMT) {
    case LINUX_S_IFDIR:  out->mode = S_IFDIR | 0777; break;
    case LINUX_S_IFREG:  out->mode = S_IFREG | 0777; break;
    case LINUX_S_IFLNK:  out->mode = S_IFLNK | 0777; break;
    case LINUX_S_IFIFO:  out->mode = S_IFIFO | 0777; break;
    case LINUX_S_IFSOCK: out->mode = S_IFSOCK | 0777; break;
    default:             out->mode = S_IFREG | 0777; break;
    }
    out->atime = inode->i_atime;
    out->mtime = inode->i_mtime;
    out->ctime = inode->i_ctime;
    return 0;
}

int extfsk_getattr(extfsk_volume *v, uint64_t ino, nfsk_stat *out)
{
    if (!v || !v->fs || !out) return -EINVAL;
    struct ext2_inode inode;
    errcode_t rc = ext2fs_read_inode(v->fs, (ext2_ino_t)ino, &inode);
    if (rc) return -EIO;
    return ext_fill_stat(v->fs, &inode, ino, out);
}

int extfsk_lookup(extfsk_volume *v, uint64_t dir_ino, const char *name,
                  uint64_t *out_ino, nfsk_stat *out_stat)
{
    if (!v || !v->fs || !name) return -EINVAL;
    ext2_ino_t ino = 0;
    errcode_t rc = ext2fs_lookup(v->fs, (ext2_ino_t)dir_ino, name,
                                 (int)strlen(name), NULL, &ino);
    if (rc == EXT2_ET_FILE_NOT_FOUND) return -ENOENT;
    if (rc) return -EIO;
    if (out_ino) *out_ino = ino;
    if (out_stat) return extfsk_getattr(v, ino, out_stat);
    return 0;
}

int64_t extfsk_read(extfsk_volume *v, uint64_t ino, uint64_t offset,
                    size_t len, void *buf)
{
    if (!v || !v->fs || !buf) return -EINVAL;
    ext2_file_t f = NULL;
    errcode_t rc = ext2fs_file_open(v->fs, (ext2_ino_t)ino, 0, &f);
    if (rc) return -EIO;
    rc = ext2fs_file_lseek(f, (ext2_off_t)offset, EXT2_SEEK_SET, NULL);
    if (rc) {
        ext2fs_file_close(f);
        return -EIO;
    }
    unsigned int got = 0;
    rc = ext2fs_file_read(f, buf, (unsigned int)len, &got);
    ext2fs_file_close(f);
    if (rc && rc != EXT2_ET_FILE_NOT_FOUND && rc != EXT2_ET_BLOCK_ALLOC_FAIL)
        return -EIO;
    return (int64_t)got;
}

/* ---------------- readdir(cookie=目录内字节偏移) ---------------- */

struct rd_ctx {
    extfsk_dirent_cb cb;
    void *ctx;
    void *fs_opaque;
    uint64_t start;      /* 本次起点(cookie) */
    uint64_t next;       /* 下一项偏移(dir_iterate2 的 offset) */
    int stopped;         /* cb 返回 1(packer 满) */
};

static int rd_proc(ext2_ino_t dir, int entry,
                   struct ext2_dir_entry *dirent,
                   int offset, int blocksize,
                   char *buf, void *private)
{
    struct rd_ctx *rc = private;
    (void)dir; (void)blocksize; (void)buf;
    if (rc->stopped) return DIRENT_ABORT;
    /* offset=当前项在目录文件内的偏移;next_cookie=下一项 */
    rc->next = (uint64_t)(offset + dirent->rec_len);
    if ((uint64_t)offset < rc->start) return 0;

    int len = dirent->name_len & 0xFF;
    char name[256];
    memcpy(name, dirent->name, len);
    name[len] = 0;
    /* FSKit packer 自带 . .. 语义(NTFS 桥同款:不吐) */
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    if (!dirent->inode) return 0;

    nfsk_stat st = {0};
    extfsk_getattr((extfsk_volume *)rc->fs_opaque, dirent->inode, &st);
    int r = rc->cb(rc->ctx, dirent->inode, name, &st, rc->next);
    if (r) rc->stopped = 1;
    return r ? DIRENT_ABORT : 0;
}

int extfsk_readdir(extfsk_volume *v, uint64_t dir_ino, uint64_t *io_cookie,
                   extfsk_dirent_cb cb, void *ctx)
{
    if (!v || !v->fs || !io_cookie || !cb) return -EINVAL;
    struct rd_ctx rc = {
        .cb = cb, .ctx = ctx,
        .start = *io_cookie, .next = *io_cookie,
        .fs_opaque = v,
    };
    errcode_t err = ext2fs_dir_iterate2(v->fs, (ext2_ino_t)dir_ino, 0,
                                        NULL, rd_proc, &rc);
    if (err == DIRENT_ABORT) err = 0;   /* packer 满=正常中止 */
    if (err) return -EIO;
    *io_cookie = rc.next;
    return 0;
}

/* ---------------- write ---------------- */

static int translate_errno(errcode_t err)
{
    if (err == EXT2_ET_FILE_NOT_FOUND) return -ENOENT;
    if (err == EXT2_ET_NO_MEMORY) return -ENOMEM;
    if (err == EXT2_ET_INODE_ALLOC_FAIL || err == EXT2_ET_BLOCK_ALLOC_FAIL)
        return -ENOSPC;
    return -EIO;
}



static int ext_update_inode_size(ext2_filsys fs, ext2_ino_t ino, __u64 size)
{
    struct ext2_inode inode;
    errcode_t err = ext2fs_read_inode(fs, ino, &inode);
    if (err) return -EIO;
    err = ext2fs_inode_size_set(fs, &inode, size);
    if (err) return translate_errno(err);
    inode.i_mtime = time(NULL);
    inode.i_ctime = time(NULL);
    inode.i_atime = time(NULL);
    err = ext2fs_write_inode(fs, ino, &inode);
    if (err) return -EIO;
    return 0;
}

int64_t extfsk_write(extfsk_volume *v, uint64_t ino, uint64_t offset,
                     size_t len, const void *buf)
{
    if (!v || !v->fs || !buf) return -EINVAL;
    ext2_file_t f = NULL;
    errcode_t err = ext2fs_file_open(v->fs, (ext2_ino_t)ino,
                                     EXT2_FILE_WRITE, &f);
    if (err) return translate_errno(err);
    err = ext2fs_file_lseek(f, (ext2_off_t)offset, EXT2_SEEK_SET, NULL);
    if (err) {
        ext2fs_file_close(f);
        return translate_errno(err);
    }
    unsigned int written = 0;
    err = ext2fs_file_write(f, buf, (unsigned int)len, &written);
    if (!err) err = ext2fs_file_flush(f);
    ext2fs_file_close(f);
    if (err) return translate_errno(err);
    /* 更新 size/mtime(flush 不总写 size——fuse2fs update_inode_size 同款) */
    int rc = ext_update_inode_size(v->fs, (ext2_ino_t)ino,
                                   offset + written);
    if (rc) return rc;
    return (int64_t)written;
}

/* ---------------- create/mkdir ---------------- */

int extfsk_create(extfsk_volume *v, uint64_t dir_ino, const char *name,
                  char type, uint32_t mode, uint32_t uid, uint32_t gid,
                  uint64_t *out_ino)
{
    if (!v || !v->fs || !name || !out_ino) return -EINVAL;
    if (strlen(name) > 255) return -ENAMETOOLONG;
    ext2_filsys fs = v->fs;

    if (type == 'd') {
        /* mkdir:库一步 API(含目录块与 .. 初始化) */
        errcode_t err = ext2fs_mkdir(fs, (ext2_ino_t)dir_ino, 0, name);
        if (err) return translate_errno(err);
        ext2_ino_t child = 0;
        err = ext2fs_lookup(fs, (ext2_ino_t)dir_ino, name,
                            (int)strlen(name), NULL, &child);
        if (err) return translate_errno(err);
        *out_ino = child;
        return 0;
    }

    /* 普通文件:fuse2fs op_mknod 语义 */
    mode = (mode & 07777) | LINUX_S_IFREG;
    ext2_ino_t child = 0;
    errcode_t err = ext2fs_new_inode(fs, (ext2_ino_t)dir_ino, mode, 0, &child);
    if (err) return translate_errno(err);

    err = ext2fs_link(fs, (ext2_ino_t)dir_ino, name, child, EXT2_FT_REG_FILE);
    if (err == EXT2_ET_DIR_NO_SPACE) {
        /* 目录满:扩一块再试(fuse2fs 同款) */
        err = ext2fs_expand_dir(fs, (ext2_ino_t)dir_ino);
        if (!err) err = ext2fs_link(fs, (ext2_ino_t)dir_ino, name, child,
                                    EXT2_FT_REG_FILE);
    }
    if (err) return translate_errno(err);

    struct ext2_inode_large inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = mode;
    inode.i_links_count = 1;
    inode.i_extra_isize = sizeof(struct ext2_inode_large) -
                          EXT2_GOOD_OLD_INODE_SIZE;
    inode.i_uid = uid & 0xFFFF;
    inode.i_gid = gid & 0xFFFF;
    time_t now = time(NULL);
    inode.i_atime = inode.i_ctime = inode.i_mtime = now;
    inode.i_crtime = now;
    err = ext2fs_write_new_inode(fs, child, (struct ext2_inode *)&inode);
    if (err) return translate_errno(err);

    ext2fs_inode_alloc_stats2(fs, child, 1, 0);
    *out_ino = child;
    return 0;
}

/* ---------------- unlink(fuse2fs remove_inode 语义) ---------------- */

/* 摘除目录项后的 inode 处理:
 *  kill=1(真 unlink/rmdir):文件 links-- 归零释放;目录直接全释放
 *    (目录 links 含 ./.. 语义,摘名即无引用——punch 清目录块 +
 *    dtime + alloc_stats,与 debugfs rmdir 一致)
 *  kill=0(rename 旧名):仅摘项+ctime,不动链接(fuse2fs 同款) */
static int ext_after_unlink(extfsk_volume *v, ext2_ino_t ino, int kill,
                           ext2_ino_t parent_ino)
{
    ext2_filsys fs = v->fs;
    struct ext2_inode inode;
    errcode_t err = ext2fs_read_inode(fs, ino, &inode);
    if (err) return translate_errno(err);
    int isdir = LINUX_S_ISDIR(inode.i_mode);

    if (!kill) {
        inode.i_ctime = time(NULL);
        err = ext2fs_write_inode(fs, ino, &inode);
        if (err) return translate_errno(err);
        return 0;
    }

    if (isdir) {
        inode.i_links_count = 0;
        inode.i_dtime = time(NULL);
        /* 子目录 '..' 是父目录的一个链接——rmdir 需给父目录减链 */
        if (parent_ino) {
            struct ext2_inode pinode;
            if (ext2fs_read_inode(fs, parent_ino, &pinode) == 0 &&
                    pinode.i_links_count > 1) {
                pinode.i_links_count--;
                pinode.i_mtime = time(NULL);
                ext2fs_write_inode(fs, parent_ino, &pinode);
            }
        }
        if (ext2fs_inode_has_valid_blocks2(fs, &inode)) {
            err = ext2fs_punch(fs, ino, &inode, NULL, 0, ~0ULL);
            if (err) return translate_errno(err);
        }
        ext2fs_inode_alloc_stats2(fs, ino, -1, 1);
    } else {
        if (inode.i_links_count == 0) return 0;
        inode.i_links_count--;
        if (inode.i_links_count == 0) {
            inode.i_dtime = time(NULL);
            if (ext2fs_inode_has_valid_blocks2(fs, &inode)) {
                err = ext2fs_punch(fs, ino, &inode, NULL, 0, ~0ULL);
                if (err) return translate_errno(err);
            }
            ext2fs_inode_alloc_stats2(fs, ino, -1, 0);
        } else {
            inode.i_ctime = time(NULL);
        }
    }
    err = ext2fs_write_inode(fs, ino, &inode);
    if (err) return translate_errno(err);
    return 0;
}

int extfsk_unlink(extfsk_volume *v, uint64_t dir_ino, const char *name,
                  uint64_t ino_hint)
{
    if (!v || !v->fs || !name) return -EINVAL;
    ext2_filsys fs = v->fs;

    ext2_ino_t ino = (ext2_ino_t)ino_hint;
    if (!ino) {
        errcode_t err = ext2fs_lookup(fs, (ext2_ino_t)dir_ino, name,
                                      (int)strlen(name), NULL, &ino);
        if (err == EXT2_ET_FILE_NOT_FOUND) return -ENOENT;
        if (err) return translate_errno(err);
    }

    errcode_t err = ext2fs_unlink(fs, (ext2_ino_t)dir_ino, name,
                                  (ext2_ino_t)ino, 0);
    if (err) return translate_errno(err);
    return ext_after_unlink(v, ino, 1, (ext2_ino_t)dir_ino);
}

/* 目录 '..' 改写助手(fuse2fs update_dotdot_helper 同款) */
int dotdot_helper(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
                  int offset, int blocksize, char *buf, void *priv)
{
    (void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
    struct dotdot_ctx { ext2_ino_t new_parent; };
    struct dotdot_ctx *dd = priv;
    if (ext2fs_dirent_name_len(dirent) == 2 &&
        dirent->name[0] == '.' && dirent->name[1] == '.') {
        dirent->inode = dd->new_parent;
        return DIRENT_CHANGED | DIRENT_ABORT;
    }
    return 0;
}

/* ---------------- rename(文件 link+unlink;目录 link+改写 ..+摘旧) ---------------- */

int extfsk_rename(extfsk_volume *v, uint64_t old_dir, const char *old_name,
                  uint64_t new_dir, const char *new_name)
{
    if (!v || !v->fs || !old_name || !new_name) return -EINVAL;
    ext2_filsys fs = v->fs;

    uint64_t ino = 0;
    int rc = extfsk_lookup(v, old_dir, old_name, &ino, NULL);
    if (rc) return rc;

    struct ext2_inode inode;
    errcode_t err = ext2fs_read_inode(fs, (ext2_ino_t)ino, &inode);
    if (err) return translate_errno(err);
    /* 目录 rename:链接新名 + 更新其 '..' 指向新父 + 摘旧名 */
    if (LINUX_S_ISDIR(inode.i_mode)) {
        if (old_dir == new_dir) {
            /* 同目录改名:父不变,'..' 指向也不变,仅改两处名 */
            err = ext2fs_link(fs, (ext2_ino_t)new_dir, new_name,
                              (ext2_ino_t)ino, EXT2_FT_DIR);
            if (err) return translate_errno(err);
            err = ext2fs_unlink(fs, (ext2_ino_t)old_dir, old_name,
                                (ext2_ino_t)ino, 0);
            if (err) return translate_errno(err);
            /* 同目录改名:子目录数与父链接数均不变——不动 links_count */
            return 0;
        }
        /* 跨目录移动:改写 '..' 为新父 */
        err = ext2fs_link(fs, (ext2_ino_t)new_dir, new_name,
                          (ext2_ino_t)ino, EXT2_FT_DIR);
        if (err) return translate_errno(err);
        err = ext2fs_unlink(fs, (ext2_ino_t)old_dir, old_name,
                            (ext2_ino_t)ino, 0);
        if (err) return translate_errno(err);
        /* '..' 改写为 new_dir(fuse2fs update_dotdot 同款:
         * dir_iterate2 找到 .. 项直接改 inode,DIRENT_CHANGED) */
        struct dotdot_ctx { ext2_ino_t new_parent; };
        extern int dotdot_helper(ext2_ino_t, int, struct ext2_dir_entry *,
                                 int, int, char *, void *);
        struct dotdot_ctx dd = { .new_parent = (ext2_ino_t)new_dir };
        err = ext2fs_dir_iterate2(fs, (ext2_ino_t)ino,
                                  0, NULL,
                                  dotdot_helper, &dd);
        if (err) return translate_errno(err);
        /* 链接数:新父 +1(子目录 '..' 项),旧父 -1 */
        struct ext2_inode pinode;
        if (ext2fs_read_inode(fs, (ext2_ino_t)new_dir, &pinode) == 0) {
            pinode.i_links_count++;
            ext2fs_write_inode(fs, (ext2_ino_t)new_dir, &pinode);
        }
        if (ext2fs_read_inode(fs, (ext2_ino_t)old_dir, &pinode) == 0 &&
                pinode.i_links_count > 1) {
            pinode.i_links_count--;
            ext2fs_write_inode(fs, (ext2_ino_t)old_dir, &pinode);
        }
        return 0;
    }

    /* 目标存在:先删(POSIX rename 语义) */
    uint64_t existing = 0;
    if (extfsk_lookup(v, new_dir, new_name, &existing, NULL) == 0) {
        rc = extfsk_unlink(v, new_dir, new_name, existing);
        if (rc) return rc;
    }

    err = ext2fs_link(fs, (ext2_ino_t)new_dir, new_name,
                      (ext2_ino_t)ino, EXT2_FT_REG_FILE);
    if (err == EXT2_ET_DIR_NO_SPACE) {
        err = ext2fs_expand_dir(fs, (ext2_ino_t)new_dir);
        if (!err) err = ext2fs_link(fs, (ext2_ino_t)new_dir, new_name,
                                    (ext2_ino_t)ino, EXT2_FT_REG_FILE);
    }
    if (err) return translate_errno(err);

    /* 旧名仅摘项(rename 语义:链接不变) */
    err = ext2fs_unlink(fs, (ext2_ino_t)old_dir, old_name,
                        (ext2_ino_t)ino, 0);
    if (err) return translate_errno(err);
    return ext_after_unlink(v, (ext2_ino_t)ino, 0, 0);
}

int extfsk_fsync(extfsk_volume *v)
{
    if (!v || !v->fs) return -EINVAL;
    ext2fs_flush(v->fs);
    return 0;
}

/* ---------------- truncate(shrink=punch;grow=置 size) ---------------- */

int extfsk_truncate(extfsk_volume *v, uint64_t ino, uint64_t newsize)
{
    if (!v || !v->fs) return -EINVAL;
    ext2_filsys fs = v->fs;
    struct ext2_inode inode;
    errcode_t err = ext2fs_read_inode(fs, (ext2_ino_t)ino, &inode);
    if (err) return translate_errno(err);

    __u64 oldsize = EXT2_I_SIZE(&inode);
    if (newsize < oldsize) {
        /* 收缩:按块对齐 punch 尾部(punch 语义=整块,残余头部由 size 掩盖) */
        unsigned int bs = fs->blocksize;
        __u64 punch_start = (newsize + bs - 1) & ~((__u64)bs - 1);
        if (ext2fs_inode_has_valid_blocks2(fs, &inode)) {
            err = ext2fs_punch(fs, (ext2_ino_t)ino, &inode, NULL,
                               punch_start / bs, ~0ULL);
            if (err) return translate_errno(err);
        }
    }
    err = ext2fs_inode_size_set(fs, &inode, newsize);
    if (err) return translate_errno(err);
    inode.i_mtime = time(NULL);
    inode.i_ctime = time(NULL);
    err = ext2fs_write_inode(fs, (ext2_ino_t)ino, &inode);
    if (err) return translate_errno(err);
    return 0;
}

/* ---------------- symlink(建/读;ext 快速链 i_block 内嵌/慢速数据块) ---------------- */

int extfsk_symlink(extfsk_volume *v, uint64_t dir_ino, const char *name,
                   const char *target, uint64_t *out_ino)
{
    if (!v || !v->fs || !name || !target) return -EINVAL;
    if (strlen(name) > 255 || strlen(target) > 4095) return -ENAMETOOLONG;
    ext2_ino_t child = 0;
    errcode_t err = ext2fs_symlink(v->fs, (ext2_ino_t)dir_ino, 0, name, target);
    if (err) return translate_errno(err);
    err = ext2fs_lookup(v->fs, (ext2_ino_t)dir_ino, name,
                        (int)strlen(name), NULL, &child);
    if (err) return translate_errno(err);
    *out_ino = child;
    return 0;
}

int extfsk_readlink(extfsk_volume *v, uint64_t ino, char *buf, size_t buflen)
{
    if (!v || !v->fs || !buf) return -EINVAL;
    struct ext2_inode inode;
    errcode_t err = ext2fs_read_inode(v->fs, (ext2_ino_t)ino, &inode);
    if (err) return translate_errno(err);
    if (!LINUX_S_ISLNK(inode.i_mode)) return -EINVAL;
    const char *t;
    if (ext2fs_is_fast_symlink(&inode)) {
        /* 目标在 i_block(无数据块) */
        t = (const char *)inode.i_block;
    } else {
        /* 慢速:经 read 读目标文本 */
        int64_t n = extfsk_read(v, ino, 0, buflen - 1, buf);
        if (n < 0) return (int)n;
        buf[n] = 0;
        return 0;
    }
    size_t tl = strnlen(t, sizeof(inode.i_block));
    if (tl >= buflen) tl = buflen - 1;
    memcpy(buf, t, tl);
    buf[tl] = 0;
    return 0;
}
