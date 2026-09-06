//
//  EXTFileSystem.swift
//  USBRW — ext2/3/4(libext2fs)FSKit 模块
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  与 NTFSFileSystem 同一 appex 双引擎:probeResource 按超级块魔数
//  分流(NTFS OEM / ext 0xEF53)。脏卷(日志待恢复)策略:RW 挂载前
//  先重放日志(e2fsck 同源 recover 路径,桥内完成);重放后位图校验
//  仍不一致(真实损坏)才拒绝 RW 并指向 e2fsck。
//

import Foundation
import FSKit
import os

private let logger = Logger(subsystem: "com.usbrw.ntfs3g", category: "EXTFileSystem")

final class EXTFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations,
                            FSManageableResourceMaintenanceOperations {

    func probeResource(
        resource: FSResource,
        replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void
    ) {
        guard let block = resource as? FSBlockDeviceResource else {
            replyHandler(nil, nil)
            return
        }
        let buf = UnsafeMutableRawPointer.allocate(byteCount: 2048, alignment: 512)
        let buffer = UnsafeMutableRawBufferPointer(start: buf, count: 2048)
        do {
            _ = try block.read(into: buffer, startingAt: 0, length: 2048)
            // ext 魔数 0xEF53 @ 超级块偏移 56(块 1024 起)+ 超级块健全性
            // (s_log_block_size ≤6、inode/块计数非零):裸媒体内容任意,单靠
            // 2 字节魔数有 1/65536 误认领率,健全性检查后降至可忽略。
            let magic = buffer.loadUnaligned(fromByteOffset: 1024 + 56, as: UInt16.self)
            let inodes = buffer.loadUnaligned(fromByteOffset: 1024 + 0, as: UInt32.self)
            let blocks = buffer.loadUnaligned(fromByteOffset: 1024 + 4, as: UInt32.self)
            let logBlock = buffer.loadUnaligned(fromByteOffset: 1024 + 24, as: UInt32.self)
            logger.debug("EXT probe: magic=0x\(String(magic, radix: 16)) inodes=\(inodes) blocks=\(blocks) logBlock=\(logBlock)")
            guard magic == 0xEF53, logBlock <= 6, inodes > 0, blocks > 0 else {
                buf.deallocate()
                replyHandler(nil, nil)
                return
            }
            // UUID(超级块 104 起,16 字节)→ 容器 ID。
            // ⚠️ 曾用 withUnsafeBytes(of: buffer) 取"结构体自身字节"(16B)
            // 再下标 [104] ——越界 trap,partitionless 媒体每探必崩
            // (2026-09-03 实证:MBR 卷走 content hint 不跑探针所以从未暴露)。
            var ub = [UInt8](repeating: 0, count: 16)
            for i in 0..<16 { ub[i] = buffer[104 + i] }
            let containerID = FSContainerIdentifier(uuid: Self.uuid(fromSuperblock: ub))
            buf.deallocate()
            replyHandler(FSProbeResult.usable(name: "ext Volume", containerID: containerID), nil)
        } catch {
            logger.error("EXT probe read 失败: \(error.localizedDescription, privacy: .public)")
            buf.deallocate()
            replyHandler(nil, nil)
        }
    }

    func loadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler: @escaping (FSVolume?, (any Error)?) -> Void
    ) {
        guard let block = resource as? FSBlockDeviceResource else {
            replyHandler(nil, fs_errorForPOSIXError(POSIXError.ENODEV.rawValue))
            return
        }
        let writable = block.isWritable && !SharedSettings.forceReadOnly
        blockResourceHolder.setup(block, writable: writable)
        var io = nfsk_blockio()
        io.ctx = Unmanaged.passUnretained(BlockIOHolder.shared).toOpaque()
        io.read = { ctx, off, len, buf in BlockIOHolder.shared.read(ctx, off, len, buf) }
        io.write = { ctx, off, len, buf in BlockIOHolder.shared.write(ctx, off, len, buf) }
        io.sync = { ctx in BlockIOHolder.shared.sync(ctx) }
        io.readonly = writable ? 0 : 1
        io.block_size = UInt32(block.blockSize)
        io.block_count = block.blockCount

        var err = [CChar](repeating: 0, count: 512)
        guard let raw = extfsk_open(&io, writable ? 1 : 0, &err, err.count) else {
            let msg = String(cString: err)
            logger.error("extfsk_open 失败: \(msg, privacy: .public)")
            replyHandler(nil, fs_errorForPOSIXError(POSIXError.EIO.rawValue))
            return
        }
        var statfs = nfsk_statfs()
        _ = extfsk_fsstat(raw, &statfs)
        let name = withUnsafeBytes(of: statfs.name) {
            String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        // 卷 ID 与 probe 的容器 ID 同源(超级块 UUID):读 120 字节取
        // 104..120。extfsk_statfs 不带 UUID,只能从资源读。
        let buf = UnsafeMutableRawPointer.allocate(byteCount: 1024, alignment: 512)
        var uid = UUID()
        let rbuf = UnsafeMutableRawBufferPointer(start: buf, count: 1024)
        if (try? block.read(into: rbuf, startingAt: 0, length: 1024)) != nil {
            var ub = [UInt8](repeating: 0, count: 16)
            for i in 0..<16 { ub[i] = rbuf[104 + i] }
            uid = Self.uuid(fromSuperblock: ub)
        }
        buf.deallocate()
        replyHandler(EXTVolume(rawVolume: raw,
                               volumeName: name.isEmpty ? "ext Volume" : name,
                               writable: writable, volumeID: uid), nil)
    }

    /// 超级块 UUID → 规范化 UUID:强制 v4 version/variant 位。
    /// 超级块 16 字节是任意值,variant 位非法的 UUID 会被 DA/fskitd
    /// 拒收——探针回复后卷记录不建、挂载 approval 后静默死
    /// (2026-09-04 实证:NTFS 侧 uuid(fromSerial:) 一直有此规范化)。
    static func uuid(fromSuperblock b: [UInt8]) -> UUID {
        var v = b
        v[6] = (v[6] & 0x0F) | 0x40
        v[8] = (v[8] & 0x3F) | 0x80
        return UUID(uuid: (v[0], v[1], v[2], v[3],
                           v[4], v[5], v[6], v[7],
                           v[8], v[9], v[10], v[11],
                           v[12], v[13], v[14], v[15]))
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler reply: @escaping ((any Error)?) -> Void) {
        reply(nil)
    }

    func didFinishLoading() {
        logger.info("EXT FSKit module loaded")
    }

    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let progress = Progress(totalUnitCount: 1)
        DispatchQueue.main.async {
            progress.completedUnitCount = 1
            task.didComplete(error: nil)
        }
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        throw fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue)
    }
}

/// 块设备持有(nfsk_blockio 的 C 回调需要稳定对象图)。
private final class BlockIOHolder {
    static let shared = BlockIOHolder()
    private var block: FSBlockDeviceResource?
    private let queue = DispatchQueue(label: "com.usbrw.ext.blockio")

    func setup(_ b: FSBlockDeviceResource, writable: Bool) {
        queue.sync { self.block = b }
    }

    func read(_ ctx: UnsafeMutableRawPointer?, _ off: UInt64, _ len: Int,
              _ bufOpt: UnsafeMutableRawPointer?) -> Int32 {
        guard let buf = bufOpt else { return -Int32(EFAULT) }
        guard let b = queue.sync(execute: { block }) else { return -Int32(ENODEV) }
        let buffer = UnsafeMutableRawBufferPointer(start: buf, count: len)
        do {
            _ = try b.read(into: buffer, startingAt: off_t(off), length: len)
            return 0
        } catch { return -Int32(EIO) }
    }

    func write(_ ctx: UnsafeMutableRawPointer?, _ off: UInt64, _ len: Int,
               _ buf: UnsafeRawPointer?) -> Int32 {
        guard let b = queue.sync(execute: { block }) else { return -Int32(ENODEV) }
        guard let buf else { return -Int32(EFAULT) }
        let buffer = UnsafeRawBufferPointer(start: buf, count: len)
        do {
            _ = try b.write(from: buffer, startingAt: off_t(off), length: len)
            return 0
        } catch { return -Int32(EIO) }
    }

    func sync(_ ctx: UnsafeMutableRawPointer?) -> Int32 {
        guard let b = queue.sync(execute: { block }) else { return -Int32(ENODEV) }
        _ = try? USBRWFSKCompat.flushResource(b)
        return 0
    }
}

private let blockResourceHolder = BlockIOHolder.shared
