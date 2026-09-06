//
//  ext_journal_replay.c
//  USBRW — ext3/4 日志重放(e2fsck 同源 recover 路径)
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  组合方式与上游 debugfs 完全同源:debugfs 链 journal.o + revoke.o +
//  recovery.o(debugfs/Makefile.in:23-24),我们把三个编译单元包进同一
//  翻译单元,并复用其 -DDEBUGFS 分支(jfs_user.h 的 buffer_head 变体
//  与 e2fsck.h 都在该分支下裁剪掉)。
//
//  覆盖缺陷:fuse2fs 自带 replay 有 j_tail_sequence 不更新问题
//  (e2fsprogs#120)——本路径是 e2fsck 的 recover 实现,即 Linux fsck
//  实际重放逻辑,重放后由 ext2fs_run_ext3_journal 内部重开文件系统并
//  清除 needs_recovery(语义见 debugfs/journal.c)。
//
//  头文件解析(HEADER_SEARCH_PATHS,project.yml 注入):
//    config.h        → refs/e2fsprogs/lib/config.h(构建期 configure 生成)
//    ext2fs/*.h      → build/libext2fs/include(或 refs/e2fsprogs/lib)
//    jfs_user.h      → refs/e2fsprogs/e2fsck
//    journal.h       → 与 journal.c 同目录(#include "" 语义)
//

#define DEBUGFS 1
#include "../../../refs/e2fsprogs/debugfs/journal.c"
#include "../../../refs/e2fsprogs/e2fsck/recovery.c"
#include "../../../refs/e2fsprogs/e2fsck/revoke.c"
