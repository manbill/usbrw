//
//  NTFSVolume.swift
//  USBRW — FSVolume 全操作 → C 桥接转发
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  映射约定:
//   · FSItem.Identifier ↔ MFT 记录号
//   · FSFileName ↔ UTF-8 字节流(C 侧转 NTFS 的 UTF-16LE)
//   · 错误:C 桥接返回 -errno → fs_errorForPOSIXError
//

import Foundation
import FSKit
import os

private let logger = Logger(subsystem: "com.usbrw.ntfs3g", category: "NTFSVolume")

final class NTFSVolume: FSVolume {

    private let device: NTFSBlockDevice
    private let raw: OpaquePointer // nfsk_volume*
    private var statfsCache = nfsk_statfs()

    // MARK: - item 缓存(mft → 唯一 NTFSItem 对象)
    //
    // FSKit 运行时按 FSItem.Identifier 索引条目,但对回调返回的
    // FSItem 对象本身只做弱持有。每次 lookup 都 new 一个对象会让
    // VNOP_RENAME 解析不到条目 → 内核报 ERECYCLE 重试 32 次后以
    // ENOENT 放弃(2026-09-01 实证:"rename retry limit due to
    // ERECYCLE reached")。按 mft 缓存保证同一文件永远是同一对象。
    private let itemCacheLock = NSLock()
    private var itemCache: [UInt64: NTFSItem] = [:]

    func item(forMFT mft: UInt64, parent: UInt64 = NTFSVolume.rootMFT) -> NTFSItem {
        itemCacheLock.lock()
        defer { itemCacheLock.unlock() }
        if let it = itemCache[mft] { return it }
        let it = NTFSItem(mft: mft, parent: parent)
        itemCache[mft] = it
        return it
    }

    init(device: NTFSBlockDevice, rawVolume: OpaquePointer, volumeName: String,
         volumeID: UUID) {
        self.device = device
        self.raw = rawVolume
        // 卷 ID 与 probeResource 报告的容器 ID 同源(卷序列号推导):
        // 随机 UUID 与探测结果无关联,26 框架激活时校验会失败
        // (Accessing invalid type,26.6.2 实测)。
        super.init(volumeID: FSVolume.Identifier(uuid: volumeID),
                   volumeName: FSFileName(string: volumeName))
        _ = nfsk_fsstat(raw, &statfsCache)
    }

    deinit {
        nfsk_close(raw, /*force*/ 0)
    }

    private func posixError(_ errno_: Int32) -> any Error {
        fs_errorForPOSIXError(errno_)
    }

    /// 根目录 MFT 记录号。
    static let rootMFT: UInt64 = 5

    /// MFT 记录号 → 对外 fileID。+3 偏移避开 FSItem.Identifier 的
    /// 保留枚举值(0=invalid, 1=parentOfRoot, 2=rootDirectory):
    /// 读路径容忍任意 raw,写路径(创建等)的条目解析会拒绝未定义值
    /// (EINVAL,26.6.2 实测)。ID 只由我方生产/框架消费,统一偏移安全。
    static func itemID(_ mft: UInt64) -> FSItem.Identifier {
        FSItem.Identifier(rawValue: mft + 3) ?? .invalid
    }
}

// MARK: - PathConf

extension NTFSVolume: FSVolume.PathConfOperations {
    var maximumLinkCount: Int { 1024 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { false }
    var truncatesLongNames: Bool { false }
    var maximumXattrSize: Int { 1 << 20 }
    var maximumFileSize: UInt64 { 16 << 40 /* 16 TiB,NTFS 上限 */ }
}

// MARK: - 卷级操作

extension NTFSVolume: FSVolume.Operations {

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsHardLinks = true
        c.supportsSymbolicLinks = true
        c.supportsPersistentObjectIDs = true
        c.supportsHiddenFiles = true
        c.supports64BitObjectIDs = true
        c.caseFormat = .insensitiveCasePreserving
        return c
    }

    var volumeStatistics: FSStatFSResult {
        let r = FSStatFSResult(fileSystemTypeName: "ntfs3g")
        r.blockSize = Int(statfsCache.bsize)
        // ioSize 是 VFS 的 I/O 尺寸提示:报 512 会把读写切成极小块,
        // 每块一次 FSKit 往返,吞吐被调用开销吃掉(2026-09-01 实测:
        // 500MB 写 46.5s@512)。报 1MB 与主流卷一致。
        r.ioSize = 1 << 20
        let bsize = UInt64(max(statfsCache.bsize, 1))
        r.totalBlocks = statfsCache.total_bytes / bsize
        r.availableBlocks = statfsCache.free_bytes / bsize
        r.freeBlocks = statfsCache.free_bytes / bsize
        r.totalFiles = statfsCache.total_files
        r.freeFiles = statfsCache.free_files
        return r
    }

    func activate(options: FSTaskOptions) async throws -> FSItem {
        item(forMFT: Self.rootMFT)
    }

    func deactivate(options: FSDeactivateOptions = []) async throws {}

    func mount(options: FSTaskOptions) async throws {
        logger.info("mount")
    }

    func unmount() async {
        _ = nfsk_sync_volume(raw)
        logger.info("unmount")
    }

    func synchronize(flags: FSSyncFlags) async throws {
        let rc = nfsk_sync_volume(raw)
        if rc != 0 { throw posixError(-rc) }
    }
}

// MARK: - 属性

extension NTFSVolume {

    func attributes(
        _ desiredAttributes: FSItem.GetAttributesRequest,
        of item: FSItem
    ) async throws -> FSItem.Attributes {
        // ⚠️ 不可对 FSKit 传入对象做 type(of:)/describing 内省:
        // 26.6.2 实证其可在日志插值时 NULL isa → swift_getObjectType
        // 段错误(2026-09-01 mv 触发的崩溃,堆栈止于本函数)。先强转再记 mft。
        guard let item = item as? NTFSItem else {
            logger.error("attributes(of): 强转失败 → EINVAL")
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        logger.debug("attributes(of): mft=\(item.mft)")
        var st = nfsk_stat()
        let rc = nfsk_getattr(raw, item.mft, &st)
        if rc != 0 { throw posixError(-rc) }
        return Self.attributes(from: st, parentMFT: item.parentMFT)
    }

    func setAttributes(
        _ newAttributes: FSItem.SetAttributesRequest,
        on item: FSItem
    ) async throws -> FSItem.Attributes {
        guard let item = item as? NTFSItem else {
            logger.error("setAttributes: 强转失败 → EINVAL")
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        logger.debug("setAttributes(on): mft=\(item.mft)")
        var st = nfsk_stat()
        _ = nfsk_getattr(raw, item.mft, &st) // 先取现值

        var which: UInt32 = 0
        if newAttributes.isValid(FSItem.Attribute.mode) {
            st.mode = newAttributes.mode; which |= 1
        }
        if newAttributes.isValid(FSItem.Attribute.uid) {
            st.uid = newAttributes.uid; which |= 2
        }
        if newAttributes.isValid(FSItem.Attribute.gid) {
            st.gid = newAttributes.gid; which |= 4
        }
        if newAttributes.isValid(FSItem.Attribute.size) {
            st.size = newAttributes.size; which |= 8
        }
        if newAttributes.isValid(FSItem.Attribute.modifyTime) {
            st.mtime = Int64(newAttributes.modifyTime.tv_sec); which |= 16
        }
        if newAttributes.isValid(FSItem.Attribute.flags) {
            st.flags = newAttributes.flags; which |= 32
        }
        if newAttributes.isValid(FSItem.Attribute.accessTime) {
            st.atime = Int64(newAttributes.accessTime.tv_sec); which |= 64
        }
        if newAttributes.isValid(FSItem.Attribute.birthTime) {
            st.btime = Int64(newAttributes.birthTime.tv_sec); which |= 128
        }
        let rc = nfsk_setattr(raw, item.mft, &st, which)
        if rc != 0 {
            logger.error("setAttributes: nfsk_setattr which=\(which) rc=\(rc)")
            throw posixError(-rc)
        }
        return Self.attributes(from: st, parentMFT: item.parentMFT)
    }

    /// nfsk_stat → 全新 FSItem.Attributes(供 attributes(of:) 与 packEntry 使用)。
    /// 注意:26 SDK 里 FSItem.Identifier 是 NS_ENUM(仅 invalid/parentOfRoot/
    /// rootDirectory 三个常量),文档说明"可用 inode 类唯一值"。二分定位
    /// activate 阶段 "Accessing invalid type" 故障时,此函数按需裁剪字段。
    static func attributes(from st: nfsk_stat, parentMFT: UInt64 = rootMFT) -> FSItem.Attributes {
        let a = FSItem.Attributes()
        // ⚠️ itemType 必须设置:FSKit 激活时会读 -[FSItemAttributes type]
        // (fetchAndSetTypeForItem),未初始化的字段在 macOS 26 触发
        // "Accessing invalid type" 故障并中止挂载(26.6.2 lldb 实证)。
        a.type = Self.itemType(fromMode: st.mode)
        a.fileID = Self.itemID(st.mft)
        a.parentID = st.mft == Self.rootMFT
            ? .parentOfRoot
            : Self.itemID(parentMFT)
        a.mode = st.mode
        a.uid = st.uid
        a.gid = st.gid
        a.size = st.size
        a.allocSize = st.alloc_size
        a.linkCount = UInt32(clamping: st.links)
        var t = timespec()
        t.tv_sec = Int(st.atime); a.accessTime = t
        t.tv_sec = Int(st.mtime); a.modifyTime = t
        t.tv_sec = Int(st.ctime); a.changeTime = t
        t.tv_sec = Int(st.btime); a.birthTime = t
        a.flags = st.flags
        return a
    }

    /// st_mode 高 4 位(S_IFMT)→ FSItem.ItemType。数值与 <sys/stat.h> 一致。
    static func itemType(fromMode mode: UInt32) -> FSItem.ItemType {
        switch mode & 0xF000 {
        case 0x4000: return .directory   // S_IFDIR
        case 0x8000: return .file        // S_IFREG
        case 0xA000: return .symlink     // S_IFLNK
        case 0x1000: return .fifo        // S_IFIFO
        case 0x2000: return .charDevice  // S_IFCHR
        case 0x6000: return .blockDevice // S_IFBLK
        case 0xC000: return .socket      // S_IFSOCK
        default: return .unknown
        }
    }
}

// MARK: - 目录

extension NTFSVolume {

    func lookupItem(
        named name: FSFileName,
        inDirectory directory: FSItem
    ) async throws -> (FSItem, FSFileName) {
        guard let dir = directory as? NTFSItem,
              let cname = name.string?.utf8CString else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        logger.debug("lookupItem: dir=\(dir.mft) name=\(name.string ?? "?", privacy: .public)")
        var mft: UInt64 = 0
        var st = nfsk_stat()
        let rc = cname.withUnsafeBufferPointer { ptr in
            nfsk_lookup(raw, dir.mft, ptr.baseAddress, &mft, &st)
        }
        if rc != 0 {
            logger.debug("lookupItem: miss name=\(name.string ?? "?", privacy: .public) rc=\(rc)")
            throw posixError(-rc)
        }
        logger.debug("lookupItem: hit name=\(name.string ?? "?", privacy: .public) mft=\(mft)")
        return (item(forMFT: mft, parent: dir.mft), name)
    }

    func enumerateDirectory(
        _ directory: FSItem,
        startingAt cookie: FSDirectoryCookie,
        verifier: FSDirectoryVerifier,
        attributes: FSItem.GetAttributesRequest?,
        packer: FSDirectoryEntryPacker
    ) async throws -> FSDirectoryVerifier {
        guard let dir = directory as? NTFSItem else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        var cookieValue: UInt64 = cookie.rawValue
        let ctx = EnumerationContext(packer: packer, wantAttrs: attributes != nil)
        let rc = ctx.withCookies { cctx in
            nfsk_readdir(raw, dir.mft, &cookieValue, ctx.wantAttrs ? 1 : 0, { cctx, mft, name, st, nextCookie in
                let me = Unmanaged<EnumerationContext>.fromOpaque(cctx!).takeUnretainedValue()
                return me.add(mft: mft, name: name, stat: st, nextCookie: nextCookie)
            }, cctx)
        }
        if rc != 0 { throw posixError(-rc) }
        return FSDirectoryVerifier(0)
    }
}

/// 枚举回调上下文:把 C 回调落盘到 packer。
private final class EnumerationContext {
    let packer: FSDirectoryEntryPacker
    let wantAttrs: Bool

    init(packer: FSDirectoryEntryPacker, wantAttrs: Bool) {
        self.packer = packer
        self.wantAttrs = wantAttrs
    }

    func withCookies(_ body: (UnsafeMutableRawPointer?) -> Int32) -> Int32 {
        let op = Unmanaged.passUnretained(self).toOpaque()
        return body(op)
    }

    /// 返回 0 继续枚举;1 packer 已满(C 侧中止,未打包项下次重放)。
    func add(mft: UInt64, name: UnsafePointer<CChar>?, stat: UnsafePointer<nfsk_stat>?, nextCookie: UInt64) -> Int32 {
        guard let name else { return 0 }
        let fname = FSFileName(string: String(cString: name))
        let st = stat.map { $0.pointee }
        let packed = packer.packEntry(
            name: fname,
            itemType: st.map { NTFSVolume.itemType(fromMode: $0.mode) } ?? .unknown,
            itemID: NTFSVolume.itemID(mft),
            nextCookie: FSDirectoryCookie(nextCookie),
            attributes: (wantAttrs && st != nil) ? NTFSVolume.attributes(from: st!) : nil
        )
        return packed ? 0 : 1
    }
}

// MARK: - 创建/删除/改名/链接

extension NTFSVolume {

    func createItem(
        named name: FSFileName,
        type: FSItem.ItemType,
        inDirectory directory: FSItem,
        attributes newAttributes: FSItem.SetAttributesRequest
    ) async throws -> (FSItem, FSFileName) {
        guard let dir = directory as? NTFSItem,
              let cname = name.string?.utf8CString else {
            logger.error("createItem: 参数强转失败 → EINVAL")
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        logger.debug("createItem: dir=\(dir.mft)")
        var mft: UInt64 = 0
        let kind: CChar = type == .directory ? 0x64 : 0x66 // 'd' / 'f'

        let rc = cname.withUnsafeBufferPointer { ptr in
            nfsk_create(raw, dir.mft, ptr.baseAddress, kind,
                        newAttributes.mode, newAttributes.uid, newAttributes.gid, &mft)
        }
        if rc != 0 {
            let diag = String(cString: nfsk_last_error())
            logger.error("createItem: nfsk_create rc=\(rc) diag=\(diag, privacy: .public)")
            throw posixError(-rc)
        }
        return (item(forMFT: mft, parent: dir.mft), name)
    }

    func createSymbolicLink(
        named name: FSFileName,
        inDirectory directory: FSItem,
        attributes newAttributes: FSItem.SetAttributesRequest,
        linkContents contents: FSFileName
    ) async throws -> (FSItem, FSFileName) {
        guard let dir = directory as? NTFSItem,
              let cname = name.string?.utf8CString,
              let ctarget = contents.string?.utf8CString else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        var mft: UInt64 = 0
        let rc = cname.withUnsafeBufferPointer { np in
            ctarget.withUnsafeBufferPointer { tp in
                nfsk_symlink(raw, dir.mft, np.baseAddress, tp.baseAddress,
                             newAttributes.uid, newAttributes.gid, &mft)
            }
        }
        if rc != 0 { throw posixError(-rc) }
        return (item(forMFT: mft, parent: dir.mft), name)
    }

    func createLink(
        to item: FSItem,
        named name: FSFileName,
        inDirectory directory: FSItem
    ) async throws -> FSFileName {
        guard let src = item as? NTFSItem, let dir = directory as? NTFSItem,
              let cname = name.string?.utf8CString else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        let rc = cname.withUnsafeBufferPointer { ptr in
            nfsk_link(raw, src.mft, dir.mft, ptr.baseAddress)
        }
        if rc != 0 { throw posixError(-rc) }
        return name
    }

    func removeItem(
        _ item: FSItem,
        named name: FSFileName,
        fromDirectory directory: FSItem
    ) async throws {
        guard let it = item as? NTFSItem, let dir = directory as? NTFSItem,
              let cname = name.string?.utf8CString else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        logger.debug("removeItem: dir=\(dir.mft) name=\(name.string ?? "?", privacy: .public) item.mft=\(it.mft)")
        let rc = cname.withUnsafeBufferPointer { ptr in
            nfsk_unlink(raw, dir.mft, ptr.baseAddress, it.mft)
        }
        if rc != 0 {
            logger.error("removeItem: rc=\(rc)")
            throw posixError(-rc)
        }
    }

    func renameItem(
        _ item: FSItem,
        inDirectory sourceDirectory: FSItem,
        named sourceName: FSFileName,
        to destinationName: FSFileName,
        inDirectory destinationDirectory: FSItem,
        overItem: FSItem?
    ) async throws -> FSFileName {
        guard let srcDir = sourceDirectory as? NTFSItem,
              let dstDir = destinationDirectory as? NTFSItem,
              let cold = sourceName.string?.utf8CString,
              let cnew = destinationName.string?.utf8CString else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        // overItem 已存在时的替换语义由 C 侧三段式安全改名处理
        logger.debug("renameItem: \(sourceName.string ?? "?", privacy: .public)→\(destinationName.string ?? "?", privacy: .public) src=\(srcDir.mft) dst=\(dstDir.mft) over=\(overItem != nil)")
        let rc2 = cold.withUnsafeBufferPointer { op in
            cnew.withUnsafeBufferPointer { np in
                nfsk_rename(raw, srcDir.mft, op.baseAddress, dstDir.mft, np.baseAddress)
            }
        }
        if rc2 != 0 {
            let diag = String(cString: nfsk_last_error())
            logger.error("renameItem: \(sourceName.string ?? "?", privacy: .public)→\(destinationName.string ?? "?", privacy: .public) src=\(srcDir.mft) dst=\(dstDir.mft) rc=\(rc2) diag=\(diag, privacy: .public)")
            throw posixError(-rc2)
        }
        return destinationName
    }

    func reclaimItem(_ item: FSItem) async throws {}

    func readSymbolicLink(_ item: FSItem) async throws -> FSFileName {
        guard let it = item as? NTFSItem else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        var buf = [CChar](repeating: 0, count: 4096)
        let rc = nfsk_readlink(raw, it.mft, &buf, buf.count)
        if rc != 0 { throw posixError(-rc) }
        return FSFileName(string: String(cString: buf))
    }
}

// MARK: - Open/Close

extension NTFSVolume: FSVolume.OpenCloseOperations {
    func openItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {}
    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {}
}

// MARK: - 读写

extension NTFSVolume: FSVolume.ReadWriteOperations {

    func read(
        from item: FSItem, at offset: off_t, length: Int,
        into buffer: FSMutableFileDataBuffer
    ) async throws -> Int {
        guard let it = item as? NTFSItem else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        let n = buffer.withUnsafeMutableBytes { dst -> Int64 in
            guard let base = dst.baseAddress else { return -Int64(POSIXError.EINVAL.rawValue) }
            return nfsk_read(raw, it.mft, UInt64(offset), min(length, dst.count), base)
        }
        if n < 0 { throw posixError(Int32(-n)) }
        return Int(n)
    }

    func write(
        contents: Data, to item: FSItem, at offset: off_t
    ) async throws -> Int {
        guard let it = item as? NTFSItem else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        let n: Int64 = contents.withUnsafeBytes { src in
            guard let base = src.baseAddress else { return -Int64(POSIXError.EINVAL.rawValue) }
            return nfsk_write(raw, it.mft, UInt64(offset), src.count, base)
        }
        if n < 0 { throw posixError(Int32(-n)) }
        return Int(n)
    }
}

// MARK: - 扩展属性

extension NTFSVolume: FSVolume.XattrOperations {

    func xattr(named name: FSFileName, of item: FSItem) async throws -> Data {
        guard let it = item as? NTFSItem,
              let cname = name.string?.utf8CString else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        var buf = [UInt8](repeating: 0, count: 64 << 10)
        let n: Int64 = cname.withUnsafeBufferPointer { ptr in
            nfsk_getxattr(raw, it.mft, ptr.baseAddress, &buf, buf.count)
        }
        if n < 0 { throw posixError(Int32(-n)) }
        return Data(buf.prefix(Int(n)))
    }

    func setXattr(
        named name: FSFileName, to value: Data?, on item: FSItem,
        policy: FSVolume.SetXattrPolicy
    ) async throws {
        guard let it = item as? NTFSItem,
              let cname = name.string?.utf8CString else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        if let value {
            let rc: Int32 = value.withUnsafeBytes { src in
                cname.withUnsafeBufferPointer { ptr in
                    nfsk_setxattr(raw, it.mft, ptr.baseAddress,
                                   src.baseAddress, src.count)
                }
            }
            if rc != 0 { throw posixError(-rc) }
        } else {
            let rc = cname.withUnsafeBufferPointer { ptr in
                nfsk_removexattr(raw, it.mft, ptr.baseAddress)
            }
            if rc != 0 { throw posixError(-rc) }
        }
    }

    func xattrs(of item: FSItem) async throws -> [FSFileName] {
        guard let it = item as? NTFSItem else {
            throw posixError(Int32(POSIXError.EINVAL.rawValue))
        }
        final class ListCtx { var names: [FSFileName] = [] }
        let ctx = ListCtx()
        let op = Unmanaged.passUnretained(ctx).toOpaque()
        let rc = nfsk_listxattr(raw, it.mft, { cctx, name in
            let me = Unmanaged<ListCtx>.fromOpaque(cctx!).takeUnretainedValue()
            if let name { me.names.append(FSFileName(string: String(cString: name))) }
            return 0
        }, op)
        if rc != 0 { throw posixError(-rc) }
        return ctx.names
    }
}
