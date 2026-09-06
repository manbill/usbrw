//
//  NTFSBlockDevice.swift
//  USBRW — 把 FSKit 块设备资源包装成 C 回调(nfsk_blockio)
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  ⚠️ 本文件是全项目唯一依赖「FSKit 块设备 API 细节」的文件。
//  已按 Xcode 16.4 / macOS 15.5 SDK 核对:FSBlockDeviceResource 继承自
//  FSResource(直接 as? 转换),提供 read/write(into:startingAt:length:)。
//

import Foundation
import FSKit
import os

private let logger = Logger(subsystem: "com.usbrw.ntfs3g", category: "BlockDevice")

final class NTFSBlockDevice {

    private let resource: FSResource
    private var blockResource: FSBlockDeviceResource?
    private var io: nfsk_blockio
    private let queue = DispatchQueue(label: "com.usbrw.ntfs3g.blockio", qos: .userInitiated)

    init?(resource: FSResource) {
        self.resource = resource
        // TODO(verify): FSResource → FSBlockDeviceResource 的官方获取路径
        // (可能是 resource as? / resource.blockDevice / FSResourceSubtype)。
        // 占位:先按 cast 尝试,SDK 就绪后修正。
        self.blockResource = resource as? FSBlockDeviceResource
        guard blockResource != nil else { return nil }

        self.io = nfsk_blockio() // 占位:允许下方 passUnretained(self) 合法

        var io = nfsk_blockio()
        io.ctx = Unmanaged.passUnretained(self).toOpaque()
        io.read = { ctx, offset, len, buf in
            guard let ctx, let buf else { return -EINVAL }
            let me = Unmanaged<NTFSBlockDevice>.fromOpaque(ctx).takeUnretainedValue()
            return me.pread(Int64(offset), Int(len), buf)
        }
        io.write = { ctx, offset, len, buf in
            guard let ctx, let buf else { return -EINVAL }
            let me = Unmanaged<NTFSBlockDevice>.fromOpaque(ctx).takeUnretainedValue()
            return me.pwrite(Int64(offset), Int(len), buf)
        }
        io.sync = { ctx in
            guard let ctx else { return -EINVAL }
            let me = Unmanaged<NTFSBlockDevice>.fromOpaque(ctx).takeUnretainedValue()
            return me.sync()
        }
        let writable = self.blockResource!.isWritable
        io.readonly = writable ? 0 : 1
        // 用户偏好(经 App Group 与宿主 app 共享):默认只读挂载新盘 +
        // 按卷序列号例外(吸收 Nigate"尊重你的选择"语义;xntfs v1.0.2
        // 的 per-scenario 只读设计)。序列号取引导扇区 0x48(8 字节小端)。
        var ro = SharedSettings.forceReadOnly
        if let serial = Self.bootSerial(from: self.blockResource!) {
            ro = SharedSettings.shouldMountReadOnly(serial: serial)
        }
        // 排障:记录设置通道实况(组套件/公共落点/最终值)
        let suiteOK = UserDefaults(suiteName: SharedSettings.suiteName)?
            .object(forKey: SharedSettings.forceReadOnlyKey) != nil
        let pubOK = FileManager.default.isReadableFile(
            atPath: "/Users/Shared/usbrw/settings.json")
        logger.info("NTFSBlockDevice prefs: suite=\(suiteOK) pub=\(pubOK) ro=\(ro)")
        if ro { io.readonly = 1 }
        // 对齐量子与设备容量:C 桥接据此做字节粒度请求的对齐扩展/RMW
        // (macOS 块 I/O 要求 offset/len 按 blockSize 对齐,否则 EINVAL —
        // 真机写路径 errno=22 的根因,2026-09-01 实证)。
        io.block_size = UInt32(self.blockResource!.blockSize)
        io.block_count = self.blockResource!.blockCount
        logger.info("NTFSBlockDevice init: isWritable=\(writable) forceRO=\(ro) → io.readonly=\(io.readonly) blockSize=\(io.block_size) blockCount=\(io.block_count)")
        self.io = io
    }

    /// 读引导扇区取 NTFS 卷序列号(0x48,8 字节小端)→ 16 位 hex。
    /// 与 app 侧(AppModel)及 NTFSFileSystem.uuid(fromSerial:) 同源,
    /// 三方以序列号为按盘偏好的共同键。
    static func bootSerial(from resource: FSBlockDeviceResource) -> String? {
        let buf = UnsafeMutableRawBufferPointer.allocate(byteCount: 512, alignment: 1)
        defer { buf.deallocate() }
        guard let _ = try? resource.read(into: buf, startingAt: 0, length: 512) else { return nil }
        let serial = buf.loadUnaligned(fromByteOffset: 0x48, as: UInt64.self)
        return String(serial, radix: 16, uppercase: false)
    }

    // MARK: - 高层入口(供 NTFSFileSystem 调用)

    /// 读取引导扇区并回调(0 号扇区,512 字节起步)。
    func readBootSector(_ completion: @escaping (Data?) -> Void) {
        let buf = UnsafeMutableRawPointer.allocate(byteCount: 512, alignment: 512)
        defer { buf.deallocate() }
        let rc = pread(0, 512, buf)
        guard rc == 0 else {
            logger.error("boot sector read failed: \(rc)")
            completion(nil)
            return
        }
        completion(Data(bytes: UnsafeRawPointer(buf), count: 512))
    }

    /// 通过 C 桥接挂载,产出 NTFSVolume。
    func mountVolume(_ completion: @escaping (NTFSVolume?, (any Error)?) -> Void) {
        var err = [CChar](repeating: 0, count: 256)
        // 休眠卷救援(一次性):app 侧"删除休眠文件并读写挂载"按钮
        // 置的标记;IGNORE_HIBERFILE 挂载并删 hiberfil.sys(桥内完成)。
        // 仅在真正可写挂载时消费——RO 探测/检查实例不可见标记,防误耗。
        var opts: Int32 = SharedSettings.mountOptsMask
        if io.readonly == 0,
           let serial = Self.bootSerial(from: self.blockResource!),
           SharedSettings.consumeHiberfileRescue(serial: serial) {
            opts |= Int32(NFSK_OPEN_REMOVE_HIBERFILE)
            logger.info("mountVolume: 休眠卷救援模式 serial=\(serial, privacy: .public)")
        }
        if opts != 0 {
            logger.info("mountVolume: opts=0x\(String(opts, radix: 16))")
        }
        guard let raw = opts != 0
                ? nfsk_open_ex(&io, 1, opts, &err, err.count)
                : nfsk_open(&io, /*recover*/ 1, &err, err.count) else {
            let msg = String(cString: err)
            logger.error("nfsk_open failed: \(msg, privacy: .public)")
            completion(nil, fs_errorForPOSIXError(POSIXError.EIO.rawValue))
            return
        }
        var statfs = nfsk_statfs()
        _ = nfsk_fsstat(raw, &statfs)
        // C 定长 char[256] 导入为元组,需经 withUnsafeBytes 取 CChar 指针
        let name = withUnsafeBytes(of: statfs.name) { raw in
            String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        // 卷序列号(offset 0x48)推导稳定 UUID。注意与 probe 的容器 ID
        // 差一个命名空间位(bytes[15] ^= 0x5A):卷 ID 与容器 ID 是不同
        // 概念,且避开历史悬空挂载记录(516 已存在错误,26.6.2 实测)。
        let buf = UnsafeMutableRawPointer.allocate(byteCount: 512, alignment: 512)
        var vid = UUID()
        if pread(0, 512, buf) == 0 {
            let serial = UnsafeRawBufferPointer(start: buf, count: 512)
                .loadUnaligned(fromByteOffset: 0x48, as: UInt64.self)
            var u = NTFSFileSystem.uuid(fromSerial: serial).uuid
            u.15 ^= 0x5A
            vid = UUID(uuid: u)
        }
        buf.deallocate()
        completion(NTFSVolume(device: self, rawVolume: raw,
                              volumeName: name.isEmpty ? "NTFS Volume" : name,
                              volumeID: vid), nil)
    }

    // MARK: - 同步块 I/O(C 回调入口;FSKit 异步 → 信号量桥接)

    /// 返回 0 或 -errno。
    func pread(_ offset: Int64, _ len: Int, _ buf: UnsafeMutableRawPointer) -> Int32 {
        return blockIO(direction: 0, offset: offset, len: len, buf: buf)
    }

    func pwrite(_ offset: Int64, _ len: Int, _ buf: UnsafeRawPointer) -> Int32 {
        return blockIO(direction: 1, offset: offset, len: len,
                       buf: UnsafeMutableRawPointer(mutating: buf))
    }

    func sync() -> Int32 {
        // 真实落盘:冲刷资源缓冲缓存到物理设备(metadataFlush 是 FSKit
        // 唯一的刷盘出口;ObjC-only,经 USBRWFSKCompat 桥)。
        guard let br = blockResource else {
            return Int32(POSIXError.ENODEV.rawValue)
        }
        do {
            try USBRWFSKCompat.flushResource(br)
            cacheDirty = false
            return 0
        } catch {
            logger.error("sync(metadataFlush) 失败: \(error)")
            return Int32(POSIXError.EIO.rawValue)
        }
    }

    /// FSBlockDeviceResource 同步读写(在 C 回调线程上阻塞完成)。
    ///
    /// 按请求尺寸分路(正确性不依赖分类,纯性能启发式):
    ///   小请求(<64KB,元数据:MFT 记录/位图/目录索引/日志)
    ///     读 metadataRead(命中缓存),写 delayedMetadataWrite(入缓存即返回,
    ///     create 等小文件操作 130ms → ~2ms,实测 500 文件 65s → 1s);
    ///   大请求(≥64KB,文件数据流)plain read/write
    ///     —— metadataRead 无 kext readahead,顺序读曾 19s → 32s,故数据
    ///     流不走元数据缓存。
    /// 一致性:缓存脏后小读失败先冲刷再 plain read;
    /// 落盘点:sync/fsync/unmount → metadataFlush(见 sync())。
    private var cacheDirty = false
    private static let metadataCutover = 64 * 1024

    private func blockIO(direction: Int32, offset: Int64, len: Int,
                         buf: UnsafeMutableRawPointer) -> Int32 {
        guard let br = blockResource else {
            return Int32(POSIXError.ENODEV.rawValue)
        }
        let buffer = UnsafeMutableRawBufferPointer(start: buf, count: len)
        if len < Self.metadataCutover {
            if direction == 0 {
                do {
                    try br.metadataRead(into: buffer, startingAt: off_t(offset), length: len)
                    return 0
                } catch {
                    if cacheDirty { _ = try? USBRWFSKCompat.flushResource(br); cacheDirty = false }
                }
            } else {
                do {
                    try br.delayedMetadataWrite(from: UnsafeRawBufferPointer(buffer),
                                                startingAt: off_t(offset), length: len)
                    cacheDirty = true
                    return 0
                } catch {
                    /* 冷区间延迟写不支持时回退直写 */
                }
            }
        }
        do {
            let n: Int
            if direction == 0 {
                n = try br.read(into: buffer, startingAt: off_t(offset), length: len)
            } else {
                n = try br.write(from: UnsafeRawBufferPointer(buffer),
                                 startingAt: off_t(offset), length: len)
            }
            return n == len ? 0 : Int32(POSIXError.EIO.rawValue)
        } catch let e as POSIXError {
            if direction == 1 {
                logger.error("pwrite 失败: POSIX \(e.code.rawValue) @\(offset) len=\(len)")
            }
            return -Int32(e.code.rawValue)
        } catch {
            if direction == 1 {
                logger.error("pwrite 失败: \(error) @\(offset) len=\(len)")
            }
            return Int32(POSIXError.EIO.rawValue)
        }
    }
}
