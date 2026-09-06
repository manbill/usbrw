/*
 * ntfs_fskit.c — libntfs-3g 文件操作到 FSKit 桥接的实现。
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 实现模式移植自 ntfs-3g 的 lowntfs-3g.c(低层、以 MFT 记录号为
 * 文件标识的 FUSE 变体),做了以下裁剪:
 *   · 无 usermapping:uid/gid 固定 0/0,chmod/chown 静默成功(与
 *     可移动盘语义一致,Finder 复制不会因 EOPNOTSUPP 弹错);
 *   · 无 reparse 插件:Windows 联接/符号链接按只读处理
 *     (ntfs_make_symlink),我们自建 symlink 用 Interix 格式
 *     (ntfs_create_symlink),与全部 ntfs-3g 版本兼容;
 *   · xattr 用「开放名字空间」:xattr 名即 NTFS 命名流名,与
 *     macOS 的 com.apple.* 命名习惯自然契合(ResourceFork 也是流)。
 *
 * 已知限制(后续轮次):
 *   · 名字查找大小写敏感(卷按默认 case-sensitive 打开,目录列表
 *     保留原始大小写;macOS 侧声明 case-insensitive 是目标态,
 *     需要时在此层加大小写不敏感回退扫描);
 *   · 删除仍被 VFS 打开的文件无 ghost 改名保护。
 */

#include "ntfs_fskit_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/endians.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/mft.h>
#include <ntfs-3g/misc.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/reparse.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/unistr.h>

static char g_last_error[640];

/* ---- 全局互斥 -------------------------------------------------------
 * FSKit 从多个线程并发派发卷操作;libntfs-3g 非线程安全(共享
 * vol->mftbmp_na->rl、inode 缓存、attr 搜索上下文等全局态)。
 * 桥接层以递归大锁串行化所有公开操作(等价 macFUSE 单线程语义)。
 * nfsk_last_error/nfsk_dev_write_log 为调试通道,不加锁。
 */
static pthread_mutex_t g_nfsk_mu;
static pthread_once_t g_nfsk_mu_once = PTHREAD_ONCE_INIT;
static void nfsk_mu_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_nfsk_mu, &a);
    pthread_mutexattr_destroy(&a);
}
void nfsk_lock(void)
{
    pthread_once(&g_nfsk_mu_once, nfsk_mu_init);
    pthread_mutex_lock(&g_nfsk_mu);
}
void nfsk_unlock(void)
{
    pthread_mutex_unlock(&g_nfsk_mu);
}

const char *nfsk_last_error(void)
{
    return g_last_error;
}

/* 设备层写错误经此通道报上来(避免 lib errno 残留干扰) */
void nfsk_dev_write_log(const char *msg)
{
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

/* 调试探针:lib alloc errno 来源标记 */
extern int usbrw_alloc_errno_src;
extern int usbrw_create_err_src;
extern int usbrw_alloc_src2;
extern int usbrw_rl_src;
extern int usbrw_src4;
extern int usbrw_src5;
extern int usbrw_src6;
extern int usbrw_pread_src;
extern char usbrw_rl_detail[256];

/* set_archive 等价物(lowntfs-3g.c 的宏) */
#define NFSK_SET_ARCHIVE(ni) ((ni)->flags |= FILE_ATTR_ARCHIVE)

/* NFSK_DOS_HIDDEN:FILE_ATTRIBUTE_HIDDEN 的 CPU 序数值(0x0002),
 * 用于绕过 le32 枚举取反的宽度陷阱。 */
#define NFSK_DOS_HIDDEN 0x0002u

/* ---------------- 通用辅助 ---------------- */

/*
 * 从已打开的 inode 组装 nfsk_stat。
 * 移植 lowntfs-3g.c 的 ntfs_fuse_getstat,去掉插件/usermapping 分支。
 * 失败返回负 -errno。
 */
static int fill_stat(nfsk_volume *v, ntfs_inode *ni, nfsk_stat *out)
{
    ntfs_attr *na;
    (void)v;

    memset(out, 0, sizeof(*out));
    out->mft = (uint64_t)ni->mft_no;
    out->links = le16_to_cpu(ni->mrec->link_count);

    if (ni->flags & FILE_ATTR_REPARSE_POINT) {
        /* Windows 符号链接/联接:只读展示为 symlink */
        char *target;
        out->mode = S_IFLNK | 0777;
        errno = 0;
        target = ntfs_make_symlink(ni, "/");
        if (target) {
            out->size = (uint64_t)strlen(target);
            free(target);
        } else if (errno != EOPNOTSUPP) {
            return -errno;
        } /* 无法解析的 reparse:size 留 0,仍按 symlink 展示 */
        out->alloc_size = (uint64_t)ni->allocated_size;
    } else if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
        out->mode = S_IFDIR | 0777;
        /* 目录大小取 $I30 索引分配(惰性,取过一次就缓存到 ni) */
        if (!test_nino_flag(ni, KnownSize)) {
            na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);
            if (na) {
                ni->data_size = na->data_size;
                ni->allocated_size = na->allocated_size;
                set_nino_flag(ni, KnownSize);
                ntfs_attr_close(na);
            }
        }
        out->size = (uint64_t)ni->data_size;
        out->alloc_size = (uint64_t)ni->allocated_size;
        out->links = 1; /* 无 posix_nlink 语义,让 find(1) 正常工作 */
    } else {
        out->mode = S_IFREG;
        out->size = (uint64_t)ni->data_size;
        out->alloc_size = (uint64_t)ni->allocated_size;
        if (ni->flags & FILE_ATTR_SYSTEM) {
            /* Interix 特殊文件:fifo/socket/symlink/设备 */
            na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
            if (na) {
                if (!(ni->flags & FILE_ATTR_HIDDEN)) {
                    if (na->data_size == 0)
                        out->mode = S_IFIFO;
                    else if (na->data_size == 1)
                        out->mode = S_IFSOCK;
                }
                if ((u64)na->data_size > sizeof(INTX_FILE_TYPES)
                    && (u64)na->data_size <= sizeof(INTX_FILE_TYPES)
                                + sizeof(ntfschar) * PATH_MAX) {
                    INTX_FILE *intx = ntfs_malloc(na->data_size);
                    if (!intx) {
                        ntfs_attr_close(na);
                        return -errno;
                    }
                    if (ntfs_attr_pread(na, 0, na->data_size, intx)
                            != na->data_size) {
                        int e = errno;
                        free(intx);
                        ntfs_attr_close(na);
                        return -e;
                    }
                    if (intx->magic == INTX_SYMBOLIC_LINK) {
                        char *target = NULL;
                        int len = ntfs_ucstombs(intx->target,
                                (int)((na->data_size
                                    - offsetof(INTX_FILE, target))
                                    / sizeof(ntfschar)), &target, 0);
                        if (len < 0) {
                            int e = errno;
                            free(intx);
                            ntfs_attr_close(na);
                            return -e;
                        }
                        free(target);
                        out->mode = S_IFLNK;
                        out->size = (uint64_t)len;
                    } else if (intx->magic == INTX_BLOCK_DEVICE) {
                        out->mode = S_IFBLK;
                    } else if (intx->magic == INTX_CHARACTER_DEVICE) {
                        out->mode = S_IFCHR;
                    }
                    free(intx);
                }
                ntfs_attr_close(na);
            }
        }
        out->mode |= 0777;
    }
    if (S_ISLNK(out->mode))
        out->mode = S_IFLNK | 0777;

    out->uid = 0;
    out->gid = 0;
    out->atime = ntfs2timespec(ni->last_access_time).tv_sec;
    out->ctime = ntfs2timespec(ni->last_mft_change_time).tv_sec;
    out->mtime = ntfs2timespec(ni->last_data_change_time).tv_sec;
    out->btime = ntfs2timespec(ni->creation_time).tv_sec;
    if (ni->flags & FILE_ATTR_HIDDEN)
        out->flags |= UF_HIDDEN;
    return 0;
}

/* dt_type(目录索引里的类型位)→ st_mode 类型位。 */
static uint32_t nfsk_dt_mode(unsigned dt_type)
{
    switch (dt_type) {
    case NTFS_DT_DIR:     return S_IFDIR | 0777;
    case NTFS_DT_LNK:     return S_IFLNK | 0777;
    case NTFS_DT_FIFO:    return S_IFIFO;
    case NTFS_DT_SOCK:    return S_IFSOCK;
    case NTFS_DT_BLK:     return S_IFBLK | 0666;
    case NTFS_DT_CHR:     return S_IFCHR | 0666;
    case NTFS_DT_REPARSE: return S_IFLNK | 0777;
    default:              return S_IFREG | 0777;
    }
}

/* 打开 inode 并把 errno 转成返回值。 */
static ntfs_inode *nfsk_iget(nfsk_volume *v, uint64_t mft)
{
    return ntfs_inode_open(v->vol, (MFT_REF)mft);
}

/*
 * 按名字在目录索引中查找,返回 MFT 引用(失败 (u64)-1,errno 已置)。
 *
 * 绕过 ntfs_inode_lookup_by_mbsname():其无缓存回退路径在卷大小写
 * 敏感(默认)时会把 NULL 名字喂给 ntfs_mbstoucs,必然失败——上游
 * 只有 FUSE 主程序创建 lookup_cache 后才走通。本层不建缓存,直接
 * 自行转换后调 ntfs_inode_lookup_by_name。
 * (因此也不需要 ntfs_inode_update_mbsname 的缓存维护调用。)
 */
static u64 nfsk_lookup_mft(ntfs_inode *dir_ni, const char *name)
{
    ntfschar *uname = NULL;
    int ulen;
    u64 iref;

    ulen = ntfs_mbstoucs(name, &uname);
    if (ulen < 0)
        return (u64)-1;
    iref = ntfs_inode_lookup_by_name(dir_ni, uname, ulen);
    free(uname);
    return iref;
}

/* ---------------- statfs ---------------- */

static int nfsk_fsstat_impl(nfsk_volume *v, nfsk_statfs *out)
{
    if (!v || !v->vol || !out) return -EINVAL;
    ntfs_volume *vol = v->vol;

    memset(out, 0, sizeof(*out));
    out->bsize = vol->cluster_size;
    /* free_clusters/free_mft_records 惰性统计:首次 statfs 时扫位图 */
    if (!NVolFreeSpaceKnown(vol))
        ntfs_volume_get_free_space(vol);
    out->total_bytes = (uint64_t)vol->nr_clusters * vol->cluster_size;
    out->free_bytes = (vol->free_clusters > 0 ? (uint64_t)vol->free_clusters : 0)
                      * vol->cluster_size;
    /* inode 数:MFT 位图覆盖数(allocated<<3,与 ntfs-3g 的 f_files 同基)
     * 与空闲记录。total 用真实值——报 UINT64_MAX 或 $MFT data_size 会让
     * df 的 iused 溢出为负(位图覆盖 32832 记录 >> $MFT 数据 256 记录)。 */
    out->total_files = (uint64_t)(vol->mftbmp_na ? vol->mftbmp_na->allocated_size : 0)
                       << 3;
    out->free_files = vol->free_mft_records > 0
                          ? (uint64_t)vol->free_mft_records
                          : out->total_files;
    /* vol_name 在挂载时已由 lib 转成 UTF-8(char*),直接拷贝 */
    if (vol->vol_name)
        strlcpy(out->name, vol->vol_name, sizeof(out->name));
    return 0;
}

/* ---------------- 文件属性 ---------------- */

static int nfsk_getattr_impl(nfsk_volume *v, uint64_t mft, nfsk_stat *out)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    int res;

    if (!v || !v->vol || !out) return -EINVAL;
    if (mft <= 1) return -ENOENT;

    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    res = fill_stat(v, ni, out);
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

static int nfsk_setattr_impl(nfsk_volume *v, uint64_t mft, const nfsk_stat *st, uint32_t which)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    int res = 0;

    if (!v || !v->vol || !st) return -EINVAL;
    if (mft <= 1) return -ENOENT;

    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;

    /* size → 截断/扩展 */
    if (which & 8) {
        ntfs_attr *na;
        s64 oldsize;

        if (mft < FILE_first_user) { res = -EPERM; goto out; }
        if (ni->flags & FILE_ATTR_REPARSE_POINT) { res = -EOPNOTSUPP; goto out; }
        na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
        if (!na) { res = -errno; goto out; }
        oldsize = na->data_size;
        if ((na->data_flags & ATTR_COMPRESSION_MASK)
            && ((s64)st->size > na->initialized_size)) {
            /* 压缩文件向上扩展:写末尾零字节(可能落成空洞) */
            char zero = 0;
            if (ntfs_attr_pwrite(na, (s64)st->size - 1, 1, &zero) <= 0) {
                res = -errno;
                ntfs_attr_close(na);
                goto out;
            }
        } else if (ntfs_attr_truncate(na, (s64)st->size)) {
            res = -errno;
            ntfs_attr_close(na);
            goto out;
        }
        ntfs_attr_close(na);
        if (oldsize != (s64)st->size)
            NFSK_SET_ARCHIVE(ni);
        ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
    }

    /* 时间戳:mtime / atime / btime(NTFS 原生支持创建时间) */
    if (which & (16 | 64 | 128)) {
        struct timespec ts;
        ts.tv_nsec = 0;
        if (which & 16) {
            ts.tv_sec = (time_t)st->mtime;
            ni->last_data_change_time = timespec2ntfs(ts);
        }
        if (which & 64) {
            ts.tv_sec = (time_t)st->atime;
            ni->last_access_time = timespec2ntfs(ts);
        }
        if (which & 128) {
            ts.tv_sec = (time_t)st->btime;
            ni->creation_time = timespec2ntfs(ts);
        }
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    }

    /* chflags:UF_HIDDEN ↔ DOS hidden 位(经正式 setter,同步 FILE_NAME) */
    if (which & 32) {
        u32 a = le32_to_cpu(ni->flags);
        if (st->flags & UF_HIDDEN)
            a |= NFSK_DOS_HIDDEN;
        else
            a &= ~NFSK_DOS_HIDDEN;
        {
            le32 la = cpu_to_le32(a);
            if (ntfs_set_ntfs_attrib(ni, (const char *)&la, sizeof(la), 0))
                res = -errno;
        }
    }

    /* mode/uid/gid(which&1/2/4):无 usermapping,静默成功 */
out:
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

/* ---------------- 目录操作 ---------------- */

static int nfsk_lookup_impl(nfsk_volume *v, uint64_t dir_mft, const char *name,
                uint64_t *out_mft, nfsk_stat *out_stat)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *dir_ni, *ni;
    u64 iref;
    int res;

    if (!v || !v->vol || !name || !out_mft) return -EINVAL;
    if (strlen(name) >= 256) return -ENAMETOOLONG;

    dir_ni = nfsk_iget(v, dir_mft);
    if (!dir_ni) return -errno;

    iref = nfsk_lookup_mft(dir_ni, name);
    if (ntfs_inode_close(dir_ni))
        return -errno;
    if ((iref == (u64)-1) || MREF(iref) <= 1)
        return -ENOENT;

    *out_mft = MREF(iref);
    if (!out_stat)
        return 0;

    ni = nfsk_iget(v, *out_mft);
    if (!ni) return -errno;
    res = fill_stat(v, ni, out_stat);
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

/*
 * readdir 的前看(look-ahead)上下文。
 *
 * FSKit 要求每个条目附带「恢复枚举时取下一项」的 cookie;ntfs_readdir
 * 的 filldir 只给当前项位置 pos。因此对第 i 项延迟落盘:第 i+1 项
 * 到达时,用它的 pos 作为第 i 项的 next_cookie 再回调;流结束时用
 * ntfs_readdir 给出的最终 pos 落盘最后一项。packer 满了(回调返回 1)
 * 时未落盘的项随下次枚举自然重放,不丢不重。
 */
struct nfsk_rd_ctx {
    nfsk_volume *v;
    nfsk_dirent_cb cb;
    void *cbctx;
    int want_attrs;
    int stopped;      /* packer 已满,已要求 ntfs_readdir 中止 */
    int err;          /* 回调的首个负错误(-errno) */
    int have_prev;
    uint64_t prev_mft;
    char *prev_name;
    nfsk_stat prev_st;
};

static int nfsk_rd_filldir(void *dirent, const ntfschar *name,
        const int name_len, const int name_type, const s64 pos,
        const MFT_REF mref, const unsigned dt_type)
{
    struct nfsk_rd_ctx *c = (struct nfsk_rd_ctx *)dirent;
    char *mbs = NULL;
    int mlen;
    uint64_t m;

    if (c->stopped)
        return 1;
    if (name_type == FILE_NAME_DOS)
        return 0; /* 8.3 别名不重复上报 */
    /* 跳过 . 与 ..(macOS VFS 自行合成) */
    if ((name_len == 1 && name[0] == (ntfschar)'.')
        || (name_len == 2 && name[0] == (ntfschar)'.'
            && name[1] == (ntfschar)'.'))
        return 0;
    m = MREF(mref);
    if (m <= 1)
        return 0;

    /* 前一项随当前项位置落盘 */
    if (c->have_prev) {
        int rc = c->cb(c->cbctx, c->prev_mft, c->prev_name,
                       &c->prev_st, (uint64_t)pos);
        free(c->prev_name);
        c->prev_name = NULL;
        c->have_prev = 0;
        if (rc > 0) {
            c->stopped = 1;
            errno = 0; /* 标记性中止,ntfs_readdir 会返回 -1/errno=0 */
            return 1;
        }
        if (rc < 0) {
            c->err = rc;
            errno = -rc;
            return -1;
        }
    }

    mlen = ntfs_ucstombs(name, name_len, &mbs, 0);
    if (mlen < 0)
        return 0; /* 无法转 UTF-8 的名字(孤代理等)跳过 */
    if (mlen > 255)
        mbs[255] = 0; /* Darwin 传统:超长名截断而不是消失 */

    memset(&c->prev_st, 0, sizeof(c->prev_st));
    c->prev_st.mft = m;
    c->prev_st.mode = nfsk_dt_mode(dt_type);
    if (c->want_attrs) {
        ntfs_inode *ni = ntfs_inode_open(c->v->vol, (MFT_REF)m);
        if (ni) {
            if (!fill_stat(c->v, ni, &c->prev_st))
                c->prev_st.mft = m;
            if (ntfs_inode_close(ni)) {
                /* 属性取不到不致命:退回轻量 stat */
                memset(&c->prev_st, 0, sizeof(c->prev_st));
                c->prev_st.mft = m;
                c->prev_st.mode = nfsk_dt_mode(dt_type);
            }
        }
    }
    c->prev_mft = m;
    c->prev_name = mbs;
    c->have_prev = 1;
    return 0;
}

static int nfsk_readdir_impl(nfsk_volume *v, uint64_t dir_mft, uint64_t *io_cookie,
                 int want_attrs, nfsk_dirent_cb cb, void *ctx)
{
    ntfs_inode *dir_ni;
    struct nfsk_rd_ctx c;
    s64 pos;
    int rc, res = 0;

    if (!v || !v->vol || !io_cookie || !cb) return -EINVAL;

    dir_ni = nfsk_iget(v, dir_mft);
    if (!dir_ni) return -errno;
    if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
        ntfs_inode_close(dir_ni);
        return -ENOTDIR;
    }

    memset(&c, 0, sizeof(c));
    c.v = v;
    c.cb = cb;
    c.cbctx = ctx;
    c.want_attrs = want_attrs;

    errno = 0;
    pos = (s64)*io_cookie;
    rc = ntfs_readdir(dir_ni, &pos, &c, nfsk_rd_filldir);
    if (getenv("NFSK_DEBUG"))
        fprintf(stderr, "[nfsk] readdir rc=%d errno=%d stopped=%d err=%d have_prev=%d pos=%lld\n",
                rc, errno, c.stopped, c.err, c.have_prev, (long long)pos);
    if (rc && !c.stopped) {
        res = c.err ? c.err : (errno ? -errno : -EIO);
    } else if (c.err) {
        res = c.err;
    }

    /* 冲刷最后一项:next_cookie = ntfs_readdir 给出的结束位置 */
    if (!res && !c.stopped && c.have_prev) {
        int prc = cb(ctx, c.prev_mft, c.prev_name, &c.prev_st, (uint64_t)pos);
        free(c.prev_name);
        c.prev_name = NULL;
        c.have_prev = 0;
        if (prc < 0)
            res = prc;
        /* prc > 0(packer 恰好满了):该项下次枚举重放,不算错误 */
    }
    if (c.have_prev) {
        free(c.prev_name);
        c.prev_name = NULL;
    }
    *io_cookie = (uint64_t)pos;
    if (ntfs_inode_close(dir_ni) && !res)
        res = -errno;
    return res;
}

/* ---------------- 创建/删除/改名/链接 ---------------- */

static int nfsk_create_impl(nfsk_volume *v, uint64_t dir_mft, const char *name,
                char type, uint32_t mode, uint32_t uid, uint32_t gid,
                uint64_t *out_mft)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfschar *uname = NULL;
    ntfs_inode *dir_ni = NULL, *ni;
    int res = 0, ulen;
    mode_t t;

    (void)mode; (void)uid; (void)gid; /* 固定 0777 / 0:0,见文件头说明 */
    if (!v || !v->vol || !name || !out_mft) return -EINVAL;
    if (strlen(name) >= 256) return -ENAMETOOLONG;
    if (dir_mft == FILE_Extend) return -EPERM;
    {
        /* 诊断:alloc 入口视角(与守卫同表达式) */
        snprintf(g_last_error, sizeof(g_last_error),
                 "pre-alloc-view: vol=%p na=%p bmpna=%p",
                 (void*)v->vol,
                 (void*)(v->vol ? v->vol->mft_na : NULL),
                 (void*)(v->vol ? v->vol->mftbmp_na : NULL));
    }

    ulen = ntfs_mbstoucs(name, &uname);
    if (ulen < 0) return -errno;
    if (ulen > 255) { res = -ENAMETOOLONG; goto exit; }

    dir_ni = nfsk_iget(v, dir_mft);
    if (!dir_ni) { res = -errno; goto exit; }
    fprintf(stderr, "[nfsk] create: NVolReadOnly=%d io->readonly=%d\n",
            NVolReadOnly(v->vol) ? 1 : 0, v->io.readonly);
    if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
        res = -EOPNOTSUPP;
        goto exit;
    }

    t = (type == 'd') ? S_IFDIR : S_IFREG;
    errno = 0;
    /* windows_names 选项:严格文件名校验(拒绝 Windows 非法字符/保留名,
     * 防"Mac 上建好、Windows 上打不开";ntfs-3g 同款,默认关)。 */
    if ((v->opts & NFSK_OPEN_WINDOWS_NAMES) &&
            ntfs_forbidden_names(v->vol, uname, ulen, TRUE)) {
        int e = errno;
        snprintf(g_last_error, sizeof(g_last_error),
                 "create: forbidden name path, errno=%d NVolRO=%d", e,
                 NVolReadOnly(v->vol) ? 1 : 0);
        res = -e;
        goto exit;
    }
    errno = 0;
    ni = ntfs_create(dir_ni, const_cpu_to_le32(0), uname, (u8)ulen, t);
    {
        int e = errno;
        snprintf(g_last_error, sizeof(g_last_error),
                 "create: ni=%p errno=%d NVolRO=%d asrc=%d csrc=%d src2=%d rlsrc=%d src4=%d src5=%d src6=%d prsrc=%d vol=%p mft_na=%p mftbmp_na=%p",
                 (void*)ni, e, NVolReadOnly(v->vol) ? 1 : 0, usbrw_alloc_errno_src, usbrw_create_err_src, usbrw_alloc_src2, usbrw_rl_src, usbrw_src4, usbrw_src5, usbrw_src6, usbrw_pread_src,
                 (void*)v->vol, (void*)(v->vol ? v->vol->mft_na : NULL),
                 (void*)(v->vol ? v->vol->mftbmp_na : NULL));
    }
    if (!ni) {
        int e = errno;
        if (!e) e = EIO;
        snprintf(g_last_error, sizeof(g_last_error),
                 "create-fail: errno=%d NVolRO=%d asrc=%d csrc=%d src2=%d rlsrc=%d src4=%d src5=%d src6=%d prsrc=%d vol=%p mft_na=%p mftbmp_na=%p rl[%s]",
                 e, NVolReadOnly(v->vol) ? 1 : 0, usbrw_alloc_errno_src, usbrw_create_err_src,
                 usbrw_alloc_src2, usbrw_rl_src, usbrw_src4, usbrw_src5, usbrw_src6, usbrw_pread_src,
                 (void*)v->vol, (void*)v->vol->mft_na, (void*)v->vol->mftbmp_na,
                 usbrw_rl_detail);
        res = -e;
        goto exit;
    }
    NFSK_SET_ARCHIVE(ni);
    NInoSetDirty(ni);
    *out_mft = (uint64_t)ni->mft_no;
    /* 关 ni 需要 dir_ni 同步索引,用 close_in_dir 避免重开 */
    if (ntfs_inode_close_in_dir(ni, dir_ni) && !res)
        res = -errno;
    ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
exit:
    free(uname);
    if (ntfs_inode_close(dir_ni) && !res)
        res = -errno;
    return res;
}

/* 内部版 unlink:供 nfsk_unlink 与 rename 组合复用。 */
static int nfsk_do_unlink(nfsk_volume *v, uint64_t dir_mft, const char *name)
{
    ntfschar *uname = NULL;
    ntfs_inode *dir_ni = NULL, *ni = NULL;
    u64 iref;
    int res = 0, ulen;

    ulen = ntfs_mbstoucs(name, &uname);
    if (ulen < 0) return -errno;

    dir_ni = nfsk_iget(v, dir_mft);
    if (!dir_ni) { res = -errno; goto out; }

    iref = nfsk_lookup_mft(dir_ni, name);
    if (iref == (u64)-1) { res = -errno; goto out; }
    if (MREF(iref) < FILE_first_user) { res = -EPERM; goto out; }

    ni = nfsk_iget(v, MREF(iref));
    if (!ni) { res = -errno; goto out; }

    if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
        res = -EOPNOTSUPP;
        goto out;
    }
    if (ntfs_delete(v->vol, (char *)NULL, ni, dir_ni, uname, (u8)ulen))
        res = -errno;
    /* ntfs_delete() 无论成败都会关闭 ni 与 dir_ni */
    ni = NULL;
    dir_ni = NULL;
out:
    free(uname);
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    if (ntfs_inode_close(dir_ni) && !res)
        res = -errno;
    return res;
}

static int nfsk_unlink_impl(nfsk_volume *v, uint64_t dir_mft, const char *name, uint64_t mft)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    (void)mft; /* 以名字为准(与索引一致),mft 仅调用方校验用 */
    if (!v || !v->vol || !name) return -EINVAL;
    if (dir_mft == FILE_Extend) return -EPERM;
    return nfsk_do_unlink(v, dir_mft, name);
}

/* 内部版 link:供 nfsk_link 与 rename 组合复用。 */
static int nfsk_do_link(nfsk_volume *v, uint64_t mft, uint64_t dir_mft,
                        const char *name, int allow_dir)
{
    ntfschar *uname = NULL;
    ntfs_inode *dir_ni = NULL, *ni = NULL;
    int res = 0, ulen;

    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    /* 公开硬链不接受目录;rename 内部允许(与 lowntfs-3g 的
     * newlink+rm 改名路径一致,ntfs_link 本身正确处理目录)。 */
    if (!allow_dir && (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
        res = -EPERM;
        goto out;
    }
    ulen = ntfs_mbstoucs(name, &uname);
    if (ulen < 0) { res = -errno; goto out; }
    if (ulen > 255) { res = -ENAMETOOLONG; goto out; }

    dir_ni = nfsk_iget(v, dir_mft);
    if (!dir_ni) { res = -errno; goto out; }

    if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
        res = -EOPNOTSUPP;
    } else if (ntfs_link(ni, dir_ni, uname, (u8)ulen)) {
        res = -errno;
    } else {
        NFSK_SET_ARCHIVE(ni);
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
    }
    /* 先关 dir 再关 ni:反序时 file name 同步可能失败(FUSE 注释) */
    if (ntfs_inode_close(dir_ni) && !res)
        res = -errno;
out:
    free(uname);
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

static int nfsk_link_impl(nfsk_volume *v, uint64_t mft, uint64_t new_dir,
              const char *new_name)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    if (!v || !v->vol || !new_name) return -EINVAL;
    if (mft < FILE_first_user) return -EPERM;
    return nfsk_do_link(v, mft, new_dir, new_name, 0);
}

static int nfsk_rename_impl(nfsk_volume *v, uint64_t old_dir, const char *old_name,
                uint64_t new_dir, const char *new_name)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *dir_ni, *ni;
    u64 iref, xiref;
    uint64_t ino, xino;
    int res;
    char *tmp;
    static int rename_seq;
    size_t tmp_len;

    if (!v || !v->vol || !old_name || !new_name) return -EINVAL;
    if (old_dir == FILE_Extend || new_dir == FILE_Extend) return -EPERM;

    /* 源查找 */
    dir_ni = nfsk_iget(v, old_dir);
    if (!dir_ni) return -errno;
    iref = nfsk_lookup_mft(dir_ni, old_name);
    if (ntfs_inode_close(dir_ni)) return -errno;
    if (iref == (u64)-1 || MREF(iref) <= 1) return -ENOENT;
    ino = MREF(iref);
    if (ino < FILE_first_user) return -EPERM;

    /* 目标是否已存在 */
    dir_ni = nfsk_iget(v, new_dir);
    if (!dir_ni) return -errno;
    xiref = nfsk_lookup_mft(dir_ni, new_name);
    if (ntfs_inode_close(dir_ni)) return -errno;

    if (xiref == (u64)-1) {
        /* 目标不存在:link 新名 + 删旧名(失败回滚新名) */
        res = nfsk_do_link(v, ino, new_dir, new_name, 1);
        if (res) return res;
        res = nfsk_do_unlink(v, old_dir, old_name);
        if (res)
            nfsk_do_unlink(v, new_dir, new_name);
        return res;
    }

    /* 目标已存在:三段式安全替换(临时名腾挪),语义对齐 ntfs-3g */
    xino = MREF(xiref);
    if (xino == ino)
        return 0; /* 同一 inode 改名到已链接的名字:视为成功 */
    ni = nfsk_iget(v, xino);
    if (!ni) return -errno;
    res = ntfs_check_empty_dir(ni);
    if (ntfs_inode_close(ni)) return -errno;
    if (res < 0) return -errno;
    /* 非空目录目标:交给后续 unlink 报 ENOTEMPTY */

    tmp_len = strlen(new_name) + 32;
    tmp = (char *)ntfs_malloc(tmp_len);
    if (!tmp) return -errno;
    snprintf(tmp, tmp_len, "%s.ntfs-3g-%010d", new_name, ++rename_seq);

    res = nfsk_do_link(v, xino, new_dir, tmp, 1);
    if (!res) {
        res = nfsk_do_unlink(v, new_dir, new_name);
        if (!res) {
            res = nfsk_do_link(v, ino, new_dir, new_name, 1);
            if (!res) {
                res = nfsk_do_unlink(v, old_dir, old_name);
                if (res)
                    nfsk_do_unlink(v, new_dir, new_name);
            }
            if (res)
                nfsk_do_link(v, xino, new_dir, new_name, 1); /* 尝试还原 */
        }
        nfsk_do_unlink(v, new_dir, tmp); /* 清理临时名 */
    }
    free(tmp);
    return res;
}

static int nfsk_symlink_impl(nfsk_volume *v, uint64_t dir_mft, const char *name,
                 const char *target, uint32_t uid, uint32_t gid,
                 uint64_t *out_mft)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfschar *uname = NULL, *utarget = NULL;
    ntfs_inode *dir_ni = NULL, *ni;
    int res = 0, ulen, tlen;

    (void)uid; (void)gid;
    if (!v || !v->vol || !name || !target || !out_mft) return -EINVAL;
    if (strlen(name) >= 256) return -ENAMETOOLONG;
    if (dir_mft == FILE_Extend) return -EPERM;

    ulen = ntfs_mbstoucs(name, &uname);
    if (ulen < 0) return -errno;
    if (ulen > 255) { res = -ENAMETOOLONG; goto exit; }
    tlen = ntfs_mbstoucs(target, &utarget);
    if (tlen < 0) { res = -errno; goto exit; }

    dir_ni = nfsk_iget(v, dir_mft);
    if (!dir_ni) { res = -errno; goto exit; }
    fprintf(stderr, "[nfsk] create: NVolReadOnly=%d io->readonly=%d\n",
            NVolReadOnly(v->vol) ? 1 : 0, v->io.readonly);
    if (dir_ni->flags & FILE_ATTR_REPARSE_POINT) {
        res = -EOPNOTSUPP;
        goto exit;
    }

    ni = ntfs_create_symlink(dir_ni, const_cpu_to_le32(0),
                             uname, (u8)ulen, utarget, tlen);
    if (!ni) {
        res = -errno;
        goto exit;
    }
    NFSK_SET_ARCHIVE(ni);
    NInoSetDirty(ni);
    *out_mft = (uint64_t)ni->mft_no;
    if (ntfs_inode_close_in_dir(ni, dir_ni) && !res)
        res = -errno;
    ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
exit:
    free(uname);
    free(utarget);
    if (ntfs_inode_close(dir_ni) && !res)
        res = -errno;
    return res;
}

static int nfsk_readlink_impl(nfsk_volume *v, uint64_t mft, char *buf, size_t buflen)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    ntfs_attr *na = NULL;
    INTX_FILE *intx = NULL;
    int res = 0;

    if (!v || !v->vol || !buf || !buflen) return -EINVAL;

    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;

    if (ni->flags & FILE_ATTR_REPARSE_POINT) {
        char *target;
        errno = 0;
        target = ntfs_make_symlink(ni, "/");
        if (target) {
            strlcpy(buf, target, buflen);
            free(target);
        } else if (errno == EOPNOTSUPP) {
            REPARSE_POINT *rp = ntfs_get_reparse_point(ni);
            le32 tag = rp ? rp->reparse_tag : const_cpu_to_le32(0);
            free(rp);
            snprintf(buf, buflen, "unsupported reparse tag 0x%08lx",
                     (long)le32_to_cpu(tag));
        } else {
            res = -errno;
        }
        goto out;
    }

    /* Interix 格式(我们自建 symlink 的形态) */
    if (!(ni->flags & FILE_ATTR_SYSTEM)) {
        res = -EINVAL;
        goto out;
    }
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) { res = -errno; goto out; }
    if ((u64)na->data_size <= sizeof(INTX_FILE_TYPES)
        || (u64)na->data_size > sizeof(INTX_FILE_TYPES)
                    + sizeof(ntfschar) * PATH_MAX) {
        res = -EINVAL;
        goto out;
    }
    intx = (INTX_FILE *)ntfs_malloc(na->data_size);
    if (!intx) { res = -errno; goto out; }
    if (ntfs_attr_pread(na, 0, na->data_size, intx) != na->data_size) {
        res = -errno;
        goto out;
    }
    if (intx->magic != INTX_SYMBOLIC_LINK) {
        res = -EINVAL;
        goto out;
    }
    {
        char *target = NULL;
        int len = ntfs_ucstombs(intx->target,
                (int)((na->data_size - offsetof(INTX_FILE, target))
                    / sizeof(ntfschar)), &target, 0);
        if (len < 0) {
            res = -errno;
        } else {
            strlcpy(buf, target, buflen);
            free(target);
        }
    }
out:
    free(intx);
    if (na)
        ntfs_attr_close(na);
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

/* ---------------- 数据读写 ---------------- */

static int64_t nfsk_read_impl(nfsk_volume *v, uint64_t mft, uint64_t offset,
                  size_t len, void *buf)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    ntfs_attr *na;
    s64 total = 0, max_read;

    if (!v || !v->vol || (!buf && len)) return -EINVAL;
    if (!len) return 0;

    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    if (ni->flags & FILE_ATTR_REPARSE_POINT) {
        ntfs_inode_close(ni);
        return -EOPNOTSUPP;
    }
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        int e = errno;
        ntfs_inode_close(ni);
        return -e;
    }

    max_read = na->data_size;
    if ((s64)offset < max_read) {
        s64 size = (s64)len;
        s64 off = (s64)offset;
        if (off + size > max_read)
            size = max_read - off;
        while (size > 0) {
            s64 ret = ntfs_attr_pread(na, off, size,
                                      (char *)buf + total);
            if (ret <= 0 || ret > size) {
                int e = (ret < 0) ? errno : EIO;
                ntfs_attr_close(na);
                ntfs_inode_close(ni);
                return -e;
            }
            size -= ret;
            off += ret;
            total += ret;
        }
        if (total > 0 && !(v->opts & NFSK_OPEN_NOATIME))
            ntfs_inode_update_times(ni, NTFS_UPDATE_ATIME);
    }
    ntfs_attr_close(na);
    if (ntfs_inode_close(ni) && !total)
        return -errno;
    return total;
}

static int64_t nfsk_write_impl(nfsk_volume *v, uint64_t mft, uint64_t offset,
                   size_t len, const void *buf)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    ntfs_attr *na;
    s64 total = 0;

    if (!v || !v->vol || (!buf && len)) return -EINVAL;
    if (!len) return 0;

    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    if (mft < FILE_first_user) {
        ntfs_inode_close(ni);
        return -EPERM; /* 元数据文件禁写 */
    }
    if (ni->flags & FILE_ATTR_REPARSE_POINT) {
        ntfs_inode_close(ni);
        return -EOPNOTSUPP;
    }
    na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        int e = errno;
        ntfs_inode_close(ni);
        return -e;
    }
    {
        s64 size = (s64)len;
        s64 off = (s64)offset;
        while (size > 0) {
            s64 ret = ntfs_attr_pwrite(na, off, size,
                                       (const char *)buf + total);
            if (ret <= 0) {
                int e = errno;
                ntfs_attr_close(na);
                ntfs_inode_close(ni);
                return total > 0 ? total : -e;
            }
            size -= ret;
            off += ret;
            total += ret;
        }
    }
    ntfs_attr_close(na);
    if (total > 0) {
        NFSK_SET_ARCHIVE(ni);
        ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
    }
    if (ntfs_inode_close(ni) && !total)
        return -errno;
    return total;
}

static int nfsk_truncate_impl(nfsk_volume *v, uint64_t mft, uint64_t newsize)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    nfsk_stat st;

    memset(&st, 0, sizeof(st));
    st.size = newsize;
    return nfsk_setattr(v, mft, &st, 8);
}

static int nfsk_fsync_impl(nfsk_volume *v, uint64_t mft)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    (void)mft;
    if (!v || !v->vol) return -EINVAL;
    /* 与 ntfs-3g 的 fsync 一致:整设备同步 */
    return ntfs_device_sync(v->dev);
}

/* ---------------- 扩展属性(开放名字空间:名即流名) ---------------- */

/* 用户 xattr 只允许常规文件与目录(排除 symlink/Interix/系统文件)。 */
static int nfsk_xattr_ok(ntfs_inode *ni)
{
    if (ni->flags & FILE_ATTR_REPARSE_POINT)
        return 0;
    if (ni->flags & FILE_ATTR_SYSTEM)
        return 0;
    return 1;
}

static int64_t nfsk_getxattr_impl(nfsk_volume *v, uint64_t mft, const char *name,
                      void *buf, size_t buflen)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    ntfs_attr *na;
    ntfschar *lename = NULL;
    int lename_len;
    s64 size, res;

    if (!v || !v->vol || !name) return -EINVAL;
    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    if (!nfsk_xattr_ok(ni)) {
        res = -ENOATTR;
        goto out_ni;
    }
    lename_len = ntfs_mbstoucs(name, &lename);
    if (lename_len < 0) {
        res = -errno;
        goto out_ni;
    }
    na = ntfs_attr_open(ni, AT_DATA, lename, lename_len);
    free(lename);
    if (!na) {
        res = -ENOATTR;
        goto out_ni;
    }
    size = na->data_size;
    if (!buflen) {
        res = size; /* 仅查询大小 */
    } else if (buflen < (size_t)size) {
        res = -ERANGE;
    } else {
        res = ntfs_attr_pread(na, 0, size, buf);
        if (res != size)
            res = (res < 0) ? -errno : -EIO;
    }
    ntfs_attr_close(na);
out_ni:
    if (ntfs_inode_close(ni) && res >= 0)
        res = -errno;
    return res;
}

static int nfsk_setxattr_impl(nfsk_volume *v, uint64_t mft, const char *name,
                  const void *buf, size_t buflen)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    ntfs_attr *na = NULL;
    ntfschar *lename = NULL;
    int lename_len, res = 0;
    size_t total = 0;

    if (!v || !v->vol || !name) return -EINVAL;
    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    if (!nfsk_xattr_ok(ni)) {
        res = -EPERM;
        goto out_ni;
    }
    lename_len = ntfs_mbstoucs(name, &lename);
    if (lename_len < 0) {
        res = -errno;
        goto out_ni;
    }
    na = ntfs_attr_open(ni, AT_DATA, lename, lename_len);
    if (na) {
        /* 已存在:清到 0 再整体重写(macOS 语义:同名覆盖) */
        if (ntfs_attr_truncate(na, (s64)0)) {
            res = -errno;
            goto out;
        }
    } else {
        if (ntfs_attr_add(ni, AT_DATA, lename, lename_len, NULL, 0)) {
            res = -errno;
            goto out;
        }
        if (!(ni->flags & FILE_ATTR_ARCHIVE)) {
            NFSK_SET_ARCHIVE(ni);
            NInoFileNameSetDirty(ni);
        }
        na = ntfs_attr_open(ni, AT_DATA, lename, lename_len);
        if (!na) {
            res = -errno;
            goto out;
        }
    }
    while (total < buflen) {
        s64 part = ntfs_attr_pwrite(na, (s64)total, buflen - total,
                                    (const char *)buf + total);
        if (part <= 0) {
            res = -errno;
            goto out;
        }
        total += (size_t)part;
    }
    if (ntfs_attr_pclose(na)) {
        res = -errno;
        goto out;
    }
    ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
    if (!(ni->flags & FILE_ATTR_ARCHIVE)) {
        NFSK_SET_ARCHIVE(ni);
        NInoFileNameSetDirty(ni);
    }
out:
    if (na)
        ntfs_attr_close(na);
    free(lename);
out_ni:
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

static int nfsk_removexattr_impl(nfsk_volume *v, uint64_t mft, const char *name)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    ntfs_attr *na;
    ntfschar *lename = NULL;
    int lename_len, res = 0;

    if (!v || !v->vol || !name) return -EINVAL;
    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    if (!nfsk_xattr_ok(ni)) {
        res = -ENOATTR;
        goto out_ni;
    }
    lename_len = ntfs_mbstoucs(name, &lename);
    if (lename_len < 0) {
        res = -errno;
        goto out_ni;
    }
    na = ntfs_attr_open(ni, AT_DATA, lename, lename_len);
    free(lename);
    if (!na) {
        res = -ENOATTR;
        goto out_ni;
    }
    if (ntfs_attr_rm(na)) {
        res = -errno;
    } else {
        ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
        NFSK_SET_ARCHIVE(ni);
        NInoFileNameSetDirty(ni);
    }
    ntfs_attr_close(na); /* ntfs_attr_rm 不释放 na,照常关闭 */
out_ni:
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

static int nfsk_listxattr_impl(nfsk_volume *v, uint64_t mft, nfsk_xattr_cb cb, void *ctx)
{
    errno = 0; /* 清上游残留污染,防 EINVAL 假象 */
    ntfs_inode *ni;
    ntfs_attr_search_ctx *actx;
    int res = 0, done = 0;

    if (!v || !v->vol || !cb) return -EINVAL;
    ni = nfsk_iget(v, mft);
    if (!ni) return -errno;
    if (!nfsk_xattr_ok(ni))
        goto out_ni; /* 空列表 */

    actx = ntfs_attr_get_search_ctx(ni, NULL);
    if (!actx) {
        res = -errno;
        goto out_ni;
    }
    errno = 0;
    while (!done) {
        char *tmp = NULL;
        int tl, crc;

        if (ntfs_attr_lookup(AT_DATA, NULL, 0, CASE_SENSITIVE,
                             0, NULL, 0, actx)) {
            done = (errno == ENOENT);
            break;
        }
        if (!actx->attr->name_length)
            continue;
        tl = ntfs_ucstombs((const ntfschar *)((const u8 *)actx->attr
                + le16_to_cpu(actx->attr->name_offset)),
                actx->attr->name_length, &tmp, 0);
        if (tl < 0)
            continue; /* 单个坏名不拖垮整个列表 */
        crc = cb(ctx, tmp);
        free(tmp);
        if (crc)
            done = 1; /* 调用方要求停止(packer 满),非错误 */
    }
    if (!done)
        res = -errno;
    ntfs_attr_put_search_ctx(actx);
out_ni:
    if (ntfs_inode_close(ni) && !res)
        res = -errno;
    return res;
}

/* ---- 加锁包装(公开 ABI;实现为上方同名 _impl) ------------------------------
 * FSKit 并发派发,libntfs-3g 非线程安全:所有公开操作串行化。
 * 内部跨调用(如 truncate_impl→nfsk_setattr)经递归锁安全嵌套。
 */

int nfsk_fsstat(nfsk_volume *v, nfsk_statfs *out)
{
    int r; nfsk_lock(); r = nfsk_fsstat_impl(v, out); nfsk_unlock(); return r;
}

int nfsk_getattr(nfsk_volume *v, uint64_t mft, nfsk_stat *out)
{
    int r; nfsk_lock(); r = nfsk_getattr_impl(v, mft, out); nfsk_unlock(); return r;
}

int nfsk_setattr(nfsk_volume *v, uint64_t mft, const nfsk_stat *st, uint32_t which)
{
    int r; nfsk_lock(); r = nfsk_setattr_impl(v, mft, st, which); nfsk_unlock(); return r;
}

int nfsk_lookup(nfsk_volume *v, uint64_t dir_mft, const char *name,
                uint64_t *out_mft, nfsk_stat *out_stat)
{
    int r; nfsk_lock(); r = nfsk_lookup_impl(v, dir_mft, name, out_mft, out_stat); nfsk_unlock(); return r;
}

int nfsk_readdir(nfsk_volume *v, uint64_t dir_mft, uint64_t *io_cookie,
                 int want_attrs, nfsk_dirent_cb cb, void *ctx)
{
    int r; nfsk_lock(); r = nfsk_readdir_impl(v, dir_mft, io_cookie, want_attrs, cb, ctx); nfsk_unlock(); return r;
}

int nfsk_create(nfsk_volume *v, uint64_t dir_mft, const char *name,
                char type, uint32_t mode, uint32_t uid, uint32_t gid,
                uint64_t *out_mft)
{
    int r; nfsk_lock(); r = nfsk_create_impl(v, dir_mft, name, type, mode, uid, gid, out_mft); nfsk_unlock(); return r;
}

int nfsk_unlink(nfsk_volume *v, uint64_t dir_mft, const char *name, uint64_t mft)
{
    int r; nfsk_lock(); r = nfsk_unlink_impl(v, dir_mft, name, mft); nfsk_unlock(); return r;
}

int nfsk_link(nfsk_volume *v, uint64_t mft, uint64_t new_dir,
              const char *new_name)
{
    int r; nfsk_lock(); r = nfsk_link_impl(v, mft, new_dir, new_name); nfsk_unlock(); return r;
}

int nfsk_rename(nfsk_volume *v, uint64_t old_dir, const char *old_name,
                uint64_t new_dir, const char *new_name)
{
    int r; nfsk_lock(); r = nfsk_rename_impl(v, old_dir, old_name, new_dir, new_name); nfsk_unlock(); return r;
}

int nfsk_symlink(nfsk_volume *v, uint64_t dir_mft, const char *name,
                 const char *target, uint32_t uid, uint32_t gid,
                 uint64_t *out_mft)
{
    int r; nfsk_lock(); r = nfsk_symlink_impl(v, dir_mft, name, target, uid, gid, out_mft); nfsk_unlock(); return r;
}

int nfsk_readlink(nfsk_volume *v, uint64_t mft, char *buf, size_t buflen)
{
    int r; nfsk_lock(); r = nfsk_readlink_impl(v, mft, buf, buflen); nfsk_unlock(); return r;
}

int64_t nfsk_read(nfsk_volume *v, uint64_t mft, uint64_t offset,
                  size_t len, void *buf)
{
    int64_t r; nfsk_lock(); r = nfsk_read_impl(v, mft, offset, len, buf); nfsk_unlock(); return r;
}

int64_t nfsk_write(nfsk_volume *v, uint64_t mft, uint64_t offset,
                   size_t len, const void *buf)
{
    int64_t r; nfsk_lock(); r = nfsk_write_impl(v, mft, offset, len, buf); nfsk_unlock(); return r;
}

int nfsk_truncate(nfsk_volume *v, uint64_t mft, uint64_t newsize)
{
    int r; nfsk_lock(); r = nfsk_truncate_impl(v, mft, newsize); nfsk_unlock(); return r;
}

int nfsk_fsync(nfsk_volume *v, uint64_t mft)
{
    int r; nfsk_lock(); r = nfsk_fsync_impl(v, mft); nfsk_unlock(); return r;
}

int64_t nfsk_getxattr(nfsk_volume *v, uint64_t mft, const char *name,
                      void *buf, size_t buflen)
{
    int64_t r; nfsk_lock(); r = nfsk_getxattr_impl(v, mft, name, buf, buflen); nfsk_unlock(); return r;
}

int nfsk_setxattr(nfsk_volume *v, uint64_t mft, const char *name,
                  const void *buf, size_t buflen)
{
    int r; nfsk_lock(); r = nfsk_setxattr_impl(v, mft, name, buf, buflen); nfsk_unlock(); return r;
}

int nfsk_removexattr(nfsk_volume *v, uint64_t mft, const char *name)
{
    int r; nfsk_lock(); r = nfsk_removexattr_impl(v, mft, name); nfsk_unlock(); return r;
}

int nfsk_listxattr(nfsk_volume *v, uint64_t mft, nfsk_xattr_cb cb, void *ctx)
{
    int r; nfsk_lock(); r = nfsk_listxattr_impl(v, mft, cb, ctx); nfsk_unlock(); return r;
}
