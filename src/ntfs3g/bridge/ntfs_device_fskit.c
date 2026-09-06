/*
 * ntfs_device_fskit.c — 把 libntfs-3g 的 ntfs_device I/O 重定向到
 * Swift 侧提供的 nfsk_blockio 回调(FSBlockDeviceResource)。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 扇区对齐问题在 Swift 侧解决(NTFSBlockDevice 做 read-modify-write),
 * 本层只做字节偏移的透传,保持简单可测。
 */

#include "ntfs_fskit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/misc.h>

static int fskit_open(struct ntfs_device *dev, int flags)
{
    nfsk_blockio *io = (nfsk_blockio *)dev->d_private;
    (void)flags;
    if (!io) return -EINVAL;
    NDevSetOpen(dev);
    if (io->readonly) NDevSetReadOnly(dev);
    return 0;
}

static int fskit_close(struct ntfs_device *dev)
{
    NDevClearOpen(dev);
    return 0;
}

static s64 fskit_seek(struct ntfs_device *dev, s64 offset, int whence)
{
    /* libntfs 内部只使用 pread/pwrite;seek 仅在启动探测期使用,
     * 我们用 d_heads 之外没有合适位置放游标,这里以静态游标近似。 */
    (void)dev; (void)offset; (void)whence;
    return 0;
}

static s64 fskit_read(struct ntfs_device *dev, void *buf, s64 count)
{
    (void)dev; (void)buf; (void)count;
    return -EOPNOTSUPP; /* 强制走 pread */
}

static s64 fskit_write(struct ntfs_device *dev, const void *buf, s64 count)
{
    (void)dev; (void)buf; (void)count;
    return -EOPNOTSUPP; /* 强制走 pwrite */
}

/* ---- 对齐垫片 -------------------------------------------------------
 * macOS 原始字符设备(/dev/rdisk)与 FSBlockDeviceResource 的读写都要求
 * offset 与 len 按设备块对齐,否则直接 EINVAL。libntfs 的设备请求是
 * 字节粒度的(典型:$MFT:$BITMAP 扫描在 ofs=3 pread 4093 字节——
 * 写路径 errno=22 的根因,2026-09-01 真机实证)。
 * 这里做"头尾弹跳 + 中段直传":
 *   读:头/尾不满一块的,整块读入栈缓冲再拷贝窗口;中段对齐直读。
 *   写:头/尾不满一块的,先读整块→补写窗口→整块写回(RMW);中段直写。
 * 对齐量子取 io->block_size(0→512,上限 4096 以约束栈缓冲)。
 * 返回约定与 unix 设备一致:成功返回字节数,失败 -1 且置 errno。
 */

#define NFSK_ALIGN_MAX 4096

static uint32_t nfsk_align_quantum(const nfsk_blockio *io)
{
    uint32_t a = io->block_size;
    if (!a) a = 512;
    if (a < 512 || a > NFSK_ALIGN_MAX || (a & (a - 1)))
        a = 512; /* 非法值回退 */
    return a;
}

/* 按 block_count 截尾;返回调整后的 end(<= 原 end)。 */
static uint64_t nfsk_clamp_end(const nfsk_blockio *io, uint32_t al,
                               uint64_t off, uint64_t end)
{
    uint64_t limit;
    if (!io->block_count) return end;
    limit = io->block_count * (uint64_t)al;
    if (off >= limit) return off;
    return end > limit ? limit : end;
}

static void nfsk_io_log(const char *op, int rc, s64 off, size_t len)
{
    extern void nfsk_dev_write_log(const char *msg);
    char m[128];
    snprintf(m, sizeof(m), "%s rc=%d @%lld len=%zu", op, rc,
             (long long)off, len);
    nfsk_dev_write_log(m);
}

static s64 fskit_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
    nfsk_blockio *io = (nfsk_blockio *)dev->d_private;
    uint8_t tmp[NFSK_ALIGN_MAX];
    uint8_t *out = (uint8_t *)buf;
    uint64_t off, end, cur;
    uint32_t al;
    int rc;

    if (!io || !io->read) { errno = EINVAL; return -1; }
    if (count < 0 || offset < 0) { errno = EINVAL; return -1; }
    if (count == 0) return 0;

    al = nfsk_align_quantum(io);
    off = (uint64_t)offset;
    end = nfsk_clamp_end(io, al, off, off + (uint64_t)count);
    if (end <= off) return 0; /* EOF */
    cur = off;

    /* 快路径:请求本身已对齐 */
    if (!(off & (al - 1)) && !(end & (al - 1))) {
        rc = io->read(io->ctx, off, (size_t)(end - off), out);
        if (rc) { errno = -rc; nfsk_io_log("pread", rc, off, (size_t)(end - off)); return -1; }
        return (s64)(end - off);
    }
    /* 头:cur 到块边界(部分块弹跳) */
    if (cur & (al - 1)) {
        uint64_t base = cur & ~(uint64_t)(al - 1);
        size_t skip = (size_t)(cur - base);
        size_t n;
        rc = io->read(io->ctx, base, al, tmp);
        if (rc) { errno = -rc; nfsk_io_log("pread-head", rc, base, al); return -1; }
        n = al - skip;
        if ((uint64_t)n > end - cur) n = (size_t)(end - cur);
        memcpy(out, tmp + skip, n);
        cur += n; out += n;
    }
    /* 中:整块直读 */
    {
        uint64_t mid = (end - cur) & ~(uint64_t)(al - 1);
        if (mid) {
            rc = io->read(io->ctx, cur, (size_t)mid, out);
            if (rc) { errno = -rc; nfsk_io_log("pread-mid", rc, cur, (size_t)mid); return -1; }
            cur += mid; out += mid;
        }
    }
    /* 尾:不足一块(部分块弹跳) */
    if (cur < end) {
        rc = io->read(io->ctx, cur, al, tmp);
        if (rc) { errno = -rc; nfsk_io_log("pread-tail", rc, cur, al); return -1; }
        memcpy(out, tmp, (size_t)(end - cur));
    }
    return (s64)(end - off);
}

static s64 fskit_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
    nfsk_blockio *io = (nfsk_blockio *)dev->d_private;
    uint8_t tmp[NFSK_ALIGN_MAX];
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t off, end, cur;
    uint32_t al;
    int rc;

    if (!io || !io->write) { errno = EINVAL; return -1; }
    if (count < 0 || offset < 0) { errno = EINVAL; return -1; }
    if (count == 0) return 0;
    if (io->readonly) { errno = EROFS; return -1; }

    al = nfsk_align_quantum(io);
    off = (uint64_t)offset;
    end = nfsk_clamp_end(io, al, off, off + (uint64_t)count);
    if (end <= off) return 0;
    cur = off;

    /* 快路径:请求本身已对齐 */
    if (!(off & (al - 1)) && !(end & (al - 1))) {
        rc = io->write(io->ctx, off, (size_t)(end - off), src);
        if (rc) { errno = -rc; nfsk_io_log("pwrite", rc, off, (size_t)(end - off)); return -1; }
        return (s64)(end - off);
    }
    /* 头:RMW(读整块→补窗口→写回) */
    if (cur & (al - 1)) {
        uint64_t base = cur & ~(uint64_t)(al - 1);
        size_t skip = (size_t)(cur - base);
        size_t n;
        rc = io->read(io->ctx, base, al, tmp);
        if (rc) { errno = -rc; nfsk_io_log("pwrite-head-r", rc, base, al); return -1; }
        n = al - skip;
        if ((uint64_t)n > end - cur) n = (size_t)(end - cur);
        memcpy(tmp + skip, src, n);
        rc = io->write(io->ctx, base, al, tmp);
        if (rc) { errno = -rc; nfsk_io_log("pwrite-head-w", rc, base, al); return -1; }
        cur += n; src += n;
    }
    /* 中:整块直写 */
    {
        uint64_t mid = (end - cur) & ~(uint64_t)(al - 1);
        if (mid) {
            rc = io->write(io->ctx, cur, (size_t)mid, src);
            if (rc) { errno = -rc; nfsk_io_log("pwrite-mid", rc, cur, (size_t)mid); return -1; }
            cur += mid; src += mid;
        }
    }
    /* 尾:RMW */
    if (cur < end) {
        size_t n = (size_t)(end - cur);
        rc = io->read(io->ctx, cur, al, tmp);
        if (rc) { errno = -rc; nfsk_io_log("pwrite-tail-r", rc, cur, al); return -1; }
        memcpy(tmp, src, n);
        rc = io->write(io->ctx, cur, al, tmp);
        if (rc) { errno = -rc; nfsk_io_log("pwrite-tail-w", rc, cur, al); return -1; }
    }
    return (s64)(end - off);
}

static int fskit_sync(struct ntfs_device *dev)
{
    nfsk_blockio *io = (nfsk_blockio *)dev->d_private;
    if (io && io->sync) return io->sync(io->ctx);
    return 0;
}

static int fskit_stat(struct ntfs_device *dev, struct stat *buf)
{
    /* 提供一个"块设备"形态的 stat:libntfs 据此走块设备路径。 */
    memset(buf, 0, sizeof(*buf));
    buf->st_mode = S_IFBLK | 0600;
    buf->st_blksize = 512;
    nfsk_blockio *io = (nfsk_blockio *)dev->d_private;
    if (io && io->readonly) buf->st_mode = S_IFBLK | 0400;
    return 0;
}

static int fskit_ioctl(struct ntfs_device *dev, unsigned long request, void *argp)
{
    (void)dev; (void)request; (void)argp;
    return -ENOTTY;
}

static struct ntfs_device_operations fskit_dev_ops = {
    .open   = fskit_open,
    .close  = fskit_close,
    .seek   = fskit_seek,
    .read   = fskit_read,
    .write  = fskit_write,
    .pread  = fskit_pread,
    .pwrite = fskit_pwrite,
    .sync   = fskit_sync,
    .stat   = fskit_stat,
    .ioctl  = fskit_ioctl,
};

#include "ntfs_fskit_internal.h"

nfsk_volume *nfsk_open(const nfsk_blockio *io, int recover,
                       char *err, size_t errlen)
{
    return nfsk_open_ex(io, recover, 0, err, errlen);
}

/* 带选项的挂载:opts 位 0x1 = remove_hiberfile(休眠卷救援:
 * IGNORE_HIBERFILE 挂载后删除 hiberfil.sys,与 ntfs-3g
 * remove_hiberfile 语义一致——Windows 未保存会话将丢失)。 */
nfsk_volume *nfsk_open_ex(const nfsk_blockio *io, int recover, int opts,
                          char *err, size_t errlen)
{
    nfsk_lock();
    if (!io || !io->read) { if (err) snprintf(err, errlen, "bad io callbacks"); nfsk_unlock(); return NULL; }


    nfsk_volume *v = calloc(1, sizeof(*v));
    if (!v) { if (err) snprintf(err, errlen, "oom"); return NULL; }
    v->io = *io;

    v->dev = ntfs_device_alloc("usbrw-fskit", 0, &fskit_dev_ops, &v->io);
    if (!v->dev) { free(v); if (err) snprintf(err, errlen, "device alloc failed"); return NULL; }

    ntfs_mount_flags flags = NTFS_MNT_EXCLUSIVE;
    if (v->io.readonly) flags |= NTFS_MNT_RDONLY;
    if (recover && !(opts & NFSK_OPEN_NORECOVER)) flags |= NTFS_MNT_RECOVER;
    if (opts & NFSK_OPEN_REMOVE_HIBERFILE) flags |= NTFS_MNT_IGNORE_HIBERFILE;
    v->opts = (uint32_t)opts;

    v->vol = ntfs_device_mount(v->dev, flags);
    if (v->vol)
        fprintf(stderr, "[nfsk] open: vol=%p mft_na=%p mftbmp_na=%p\n",
                (void*)v->vol, (void*)v->vol->mft_na, (void*)v->vol->mftbmp_na);
    if (!v->vol) {
        int e = ntfs_volume_error(errno);
        const char *why = "mount failed";
        switch (e) {
        case NTFS_VOLUME_NOT_NTFS:       why = "not an NTFS volume"; break;
        case NTFS_VOLUME_CORRUPT:        why = "volume corrupt"; break;
        case NTFS_VOLUME_HIBERNATED:     why = "hibernated windows volume"; break;
        case NTFS_VOLUME_UNCLEAN_UNMOUNT:why = "unclean unmount (needs recover)"; break;
        case NTFS_VOLUME_LOCKED:         why = "volume locked"; break;
        default: break;
        }
        if (err) snprintf(err, errlen, "%s (errno=%d)", why, errno);
        ntfs_device_free(v->dev);
        free(v);
        nfsk_unlock();
        return NULL;
    }
    /* 可见性默认与 ntfs-3g 一致:元数据文件隐藏,Windows 隐藏文件可见
     * (hidden 位经 getattr 的 UF_HIDDEN 上报给 Finder)。
     * 选项:hide_hid_files → 隐藏文件也不显示;hide_dot_files → 点文件
     * 写入时打 Windows 隐藏位(ntfs-3g 同款开关);
     * show_sys_files → 连 $ 元数据文件也显示(默认藏)。 */
    ntfs_set_shown_files(v->vol, (opts & NFSK_OPEN_SHOW_SYS_FILES) ? TRUE : FALSE,
                         !(opts & NFSK_OPEN_HIDE_HID_FILES),
                         (opts & NFSK_OPEN_HIDE_DOT_FILES) ? TRUE : FALSE);
    /* NTFS 压缩属性读写:默认开启(ntfs-3g 默认;漏开曾致压缩文件
     * 读失败——潜在缺陷,2026-09-02 补)。NO_COMPRESSION 选项可关。 */
    if (!(opts & NFSK_OPEN_NO_COMPRESSION))
        NVolSetCompression(v->vol);
    else
        NVolClearCompression(v->vol);
    /* USBRW:vol_name 兼作 lib 调试标记通道,但 lib 只按卷名长度分配
     * (ucstombs 的 u+1,无卷名时仅 1 字节),探针 snprintf(32~96)
     * 必然越界踩堆 —— 曾致 alloc 路径观察结果自相矛盾。
     * 统一扩到 128 字节,所有探针写入以 96 为上限。 */
    {
        char *nb = ntfs_malloc(128);
        if (nb) {
            memset(nb, 0, 128);
            if (v->vol->vol_name) {
                strncpy(nb, v->vol->vol_name, 127);
                ntfs_free(v->vol->vol_name);
            }
            v->vol->vol_name = nb;
        }
    }
    /* 休眠卷救援:挂载已 IGNORE_HIBERFILE 成功,若确为休眠卷,
     * 删除 hiberfil.sys(ntfs-3g remove_hiberfile 同款后处理)。
     * 失败不致命:记录到错误通道,卷保持已挂载状态。 */
    if ((opts & NFSK_OPEN_REMOVE_HIBERFILE) && !NVolReadOnly(v->vol) &&
            ntfs_volume_check_hiberfile(v->vol, 0)) {
        if (nfsk_unlink(v, 5, "hiberfil.sys", 0)) {
            snprintf(err, errlen, "hiberfil.sys 删除失败(errno=%d)", errno);
        }
    }
    nfsk_unlock();
    return v;
}

void nfsk_close(nfsk_volume *v, int force)
{
    if (!v) return;
    nfsk_lock();
    if (v->vol) ntfs_umount(v->vol, force ? 1 : 0);
    /* ntfs_umount 成功时会释放 dev;失败兜底再释放一次是安全的:
     * ntfs_device_free 对已释放指针无保护,因此失败路径不额外 free。 */
    free(v);
    nfsk_unlock();
}

int nfsk_sync_volume(nfsk_volume *v)
{
    int r;
    if (!v) return -EINVAL;
    nfsk_lock();
    r = !v->vol ? -EINVAL : ntfs_device_sync(v->dev);
    nfsk_unlock();
    return r;
}
