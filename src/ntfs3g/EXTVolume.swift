//
//  EXTVolume.swift
//  USBRW — ext2/3/4 FSVolume 全操作 → extfsk_* 转发
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  文件标识 = ext inode 号(root=2;itemID 偏移 +3 同 NTFS 惯例)。
//  首版范围:lookup/attributes/readdir/create/unlink/rename(文件)/
//  read/write/fsync/truncate;目录 rename 桥接层 EPERM(UI 走 Finder)。
//

import Foundation
import FSKit
import os

private let logger = Logger(subsystem: "com.usbrw.ntfs3g", category: "EXTVolume")

final class EXTVolume: FSVolume {

    private let raw: OpaquePointer   // extfsk_volume*
    private let writableFlag: Bool

    init(rawVolume: OpaquePointer, volumeName: String, writable: Bool,
         volumeID: UUID) {
        // 卷 ID 必须与 probeResource 报告的容器 ID 同源(超级块 UUID):
        // 曾用 statfs 计数推导——与探测结果无关联,26 框架激活校验失败,
        // 挂载在 mount approval 后静默死亡(2026-09-04 partitionless ext4
        // 调试实证;NTFSVolume 同款教训的注释早已写明)。
        self.raw = rawVolume
        self.writableFlag = writable
        super.init(volumeID: FSVolume.Identifier(uuid: volumeID),
                   volumeName: FSFileName(string: volumeName))
    }

    deinit {
        extfsk_close(raw, 0)
    }

    private func posixError(_ e: Int32) -> any Error {
        fs_errorForPOSIXError(e)
    }

    static let rootIno: UInt64 = 2

    static func itemID(_ ino: UInt64) -> FSItem.Identifier {
        FSItem.Identifier(rawValue: ino + 3) ?? .invalid
    }
    static func ino(_ id: FSItem.Identifier) -> UInt64 {
        max(0, id.rawValue - 3)
    }
}

// MARK: - 基础能力

extension EXTVolume: FSVolume.PathConfOperations {
    var maximumLinkCount: Int { 32000 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { false }
    var truncatesLongNames: Bool { false }
    var maximumXattrSize: Int { 0 }
    var maximumFileSize: UInt64 { 16 << 40 }
}

extension EXTVolume: FSVolume.Operations {
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsHardLinks = true
        c.supportsSymbolicLinks = true
        c.supportsPersistentObjectIDs = true
        c.supportsHiddenFiles = true
        c.supports64BitObjectIDs = true
        c.caseFormat = .sensitive
        return c
    }

    var volumeStatistics: FSStatFSResult {
        var st = nfsk_statfs()
        _ = extfsk_fsstat(raw, &st)
        let r = FSStatFSResult(fileSystemTypeName: "ext4")
        r.blockSize = Int(st.bsize)
        r.ioSize = 4096
        let bsize = UInt64(max(st.bsize, 1))
        r.totalBlocks = st.total_bytes / bsize
        r.availableBlocks = st.free_bytes / bsize
        r.freeBlocks = st.free_bytes / bsize
        r.totalFiles = st.total_files
        r.freeFiles = st.free_files
        return r
    }

    func activate(options: FSTaskOptions) async throws -> FSItem {
        EXTItem(ino: Self.rootIno, parent: Self.rootIno)
    }

    func deactivate(options: FSDeactivateOptions = []) async throws {}

    func mount(options: FSTaskOptions) async throws {}

    func unmount() async {
        _ = extfsk_fsync(raw)
    }

    func synchronize(flags: FSSyncFlags) async throws {
        if extfsk_fsync(raw) != 0 { throw posixError(EIO) }
    }
}

// MARK: - item

final class EXTItem: FSItem {
    let ino: UInt64
    let parentIno: UInt64
    init(ino: UInt64, parent: UInt64) {
        self.ino = ino
        self.parentIno = parent
        super.init()
    }
}

// MARK: - 属性

extension EXTVolume {

    func attributes(_ desiredAttributes: FSItem.GetAttributesRequest,
                    of item: FSItem) async throws -> FSItem.Attributes {
        guard let it = item as? EXTItem else { throw posixError(EINVAL) }
        var st = nfsk_stat()
        let rc = extfsk_getattr(raw, it.ino, &st)
        if rc != 0 { throw posixError(-rc) }
        return Self.attributes(from: st, parentIno: it.parentIno)
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest,
                       on item: FSItem) async throws -> FSItem.Attributes {
        guard let it = item as? EXTItem else { throw posixError(EINVAL) }
        var st = nfsk_stat()
        _ = extfsk_getattr(raw, it.ino, &st)
        if newAttributes.isValid(FSItem.Attribute.size) {
            // truncate 经桥接 extfsk_truncate
            let rc = extfsk_truncate(raw, it.ino, newAttributes.size)
            if rc != 0 { throw posixError(-rc) }
        }
        // mode/uid/gid/时间:ext 首版静默接受(可移动盘语义,与 NTFS 桥一致)
        _ = extfsk_getattr(raw, it.ino, &st)
        return Self.attributes(from: st, parentIno: it.parentIno)
    }

    static func attributes(from st: nfsk_stat, parentIno: UInt64 = rootIno) -> FSItem.Attributes {
        var st = st
        return attributes(from: &st, parentIno: parentIno)
    }

    static func attributes(from st: UnsafeMutablePointer<nfsk_stat>, parentIno: UInt64 = rootIno) -> FSItem.Attributes {
        let v = st.pointee
        let a = FSItem.Attributes()
        a.type = itemType(fromMode: v.mode)
        a.fileID = itemID(v.mft)
        a.parentID = v.mft == rootIno ? .parentOfRoot : itemID(parentIno)
        a.mode = v.mode
        a.uid = v.uid
        a.gid = v.gid
        a.size = v.size
        a.allocSize = v.alloc_size
        a.linkCount = UInt32(clamping: v.links)
        var t = timespec()
        t.tv_sec = Int(v.atime); a.accessTime = t
        t.tv_sec = Int(v.mtime); a.modifyTime = t
        t.tv_sec = Int(v.ctime); a.changeTime = t
        t.tv_sec = Int(v.btime); a.addedTime = t
        return a
    }

    static func itemType(fromMode mode: UInt32) -> FSItem.ItemType {
        switch mode & 0xF000 {
        case 0x4000: return .directory
        case 0x8000: return .file
        case 0xA000: return .symlink
        case 0x1000: return .fifo
        case 0x2000: return .charDevice
        case 0x6000: return .blockDevice
        case 0xC000: return .socket
        default: return .unknown
        }
    }
}

// MARK: - 目录

extension EXTVolume {

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem)
        async throws -> (FSItem, FSFileName) {
        guard let dir = directory as? EXTItem,
              let cname = name.string?.utf8CString else { throw posixError(EINVAL) }
        var ino: UInt64 = 0
        let rc = cname.withUnsafeBufferPointer { ptr in
            extfsk_lookup(raw, dir.ino, ptr.baseAddress, &ino, nil)
        }
        if rc != 0 { throw posixError(-rc) }
        return (EXTItem(ino: ino, parent: dir.ino), name)
    }

    func enumerateDirectory(
        _ directory: FSItem, startingAt cookie: FSDirectoryCookie,
        verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?,
        packer: FSDirectoryEntryPacker
    ) async throws -> FSDirectoryVerifier {
        guard let dir = directory as? EXTItem else { throw posixError(EINVAL) }
        var cookieValue = cookie.rawValue
        final class PackCtx {
            let packer: FSDirectoryEntryPacker
            let dirIno: UInt64
            init(_ p: FSDirectoryEntryPacker, _ d: UInt64) { packer = p; dirIno = d }
        }
        let ctx = PackCtx(packer, dir.ino)
        let opaque = Unmanaged.passUnretained(ctx).toOpaque()
        let rc = extfsk_readdir(raw, dir.ino, &cookieValue, { c, ino, name, st, next in
            let me = Unmanaged<PackCtx>.fromOpaque(c!).takeUnretainedValue()
            guard let name else { return 0 }
            let fname = FSFileName(string: String(cString: name))
            var attrs: FSItem.Attributes? = nil
            if let st {
                var statCopy = st.pointee
                attrs = EXTVolume.attributes(from: &statCopy, parentIno: me.dirIno)
            }
            let packed = me.packer.packEntry(
                name: fname,
                itemType: attrs?.type ?? .file,
                itemID: EXTVolume.itemID(ino),
                nextCookie: FSDirectoryCookie(next),
                attributes: attrs)
            return packed ? 0 : 1
        }, opaque)
        if rc != 0 { throw posixError(-rc) }
        return verifier
    }
}

// MARK: - 创建/删除/改名

extension EXTVolume {

    func createItem(named name: FSFileName, type: FSItem.ItemType,
                    inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest)
        async throws -> (FSItem, FSFileName) {
        guard let dir = directory as? EXTItem,
              let cname = name.string?.utf8CString else { throw posixError(EINVAL) }
        var ino: UInt64 = 0
        let kind: CChar = type == .directory ? 0x64 : 0x66
        let rc = cname.withUnsafeBufferPointer { ptr in
            extfsk_create(raw, dir.ino, ptr.baseAddress, kind,
                          newAttributes.mode, newAttributes.uid, newAttributes.gid, &ino)
        }
        if rc != 0 { throw posixError(-rc) }
        return (EXTItem(ino: ino, parent: dir.ino), name)
    }

    func removeItem(_ item: FSItem, named name: FSFileName,
                    fromDirectory directory: FSItem) async throws {
        guard let it = item as? EXTItem, let dir = directory as? EXTItem,
              let cname = name.string?.utf8CString else { throw posixError(EINVAL) }
        let rc = cname.withUnsafeBufferPointer { ptr in
            extfsk_unlink(raw, dir.ino, ptr.baseAddress, it.ino)
        }
        if rc != 0 { throw posixError(-rc) }
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem,
                    named sourceName: FSFileName, to destinationName: FSFileName,
                    inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?) async throws -> FSFileName {
        guard let srcDir = sourceDirectory as? EXTItem,
              let dstDir = destinationDirectory as? EXTItem,
              let cold = sourceName.string?.utf8CString,
              let cnew = destinationName.string?.utf8CString else {
            throw posixError(EINVAL)
        }
        let rc = cold.withUnsafeBufferPointer { op in
            cnew.withUnsafeBufferPointer { np in
                extfsk_rename(raw, srcDir.ino, op.baseAddress,
                              dstDir.ino, np.baseAddress)
            }
        }
        if rc != 0 { throw posixError(-rc) }
        return destinationName
    }
}

// MARK: - 读写

extension EXTVolume: FSVolume.OpenCloseOperations {
    func openItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {}
    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes) async throws {}
}

extension EXTVolume: FSVolume.ReadWriteOperations {

    func read(
        from item: FSItem, at offset: off_t, length: Int,
        into buffer: FSMutableFileDataBuffer
    ) async throws -> Int {
        guard let it = item as? EXTItem else { throw posixError(EINVAL) }
        let n = buffer.withUnsafeMutableBytes { dst -> Int64 in
            guard let base = dst.baseAddress else { return -Int64(EINVAL) }
            return extfsk_read(raw, it.ino, UInt64(offset),
                               min(length, dst.count), base)
        }
        if n < 0 { throw posixError(Int32(-n)) }
        return Int(n)
    }

    func write(
        contents: Data, to item: FSItem, at offset: off_t
    ) async throws -> Int {
        guard writableFlag else { throw posixError(EROFS) }
        guard let it = item as? EXTItem else { throw posixError(EINVAL) }
        let n: Int64 = contents.withUnsafeBytes { src in
            guard let base = src.baseAddress else { return -Int64(EINVAL) }
            return extfsk_write(raw, it.ino, UInt64(offset), src.count, base)
        }
        if n < 0 { throw posixError(Int32(-n)) }
        return Int(n)
    }
}

// MARK: - 协议余项(链接类,首版 stub;capabilities 已声明不支持建 symlink)

extension EXTVolume {

    func reclaimItem(_ item: FSItem) async throws {}

    func readSymbolicLink(_ item: FSItem) async throws -> FSFileName {
        guard let it = item as? EXTItem else { throw posixError(EINVAL) }
        var buf = [CChar](repeating: 0, count: 4096)
        let rc = extfsk_readlink(raw, it.ino, &buf, buf.count)
        if rc != 0 { throw posixError(-rc) }
        return FSFileName(string: String(cString: buf))
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes: FSItem.SetAttributesRequest,
                            linkContents: FSFileName) async throws -> (FSItem, FSFileName) {
        guard let dir = directory as? EXTItem,
              let cname = name.string?.utf8CString,
              let ctarget = linkContents.string?.utf8CString else { throw posixError(EINVAL) }
        var ino: UInt64 = 0
        let rc = cname.withUnsafeBufferPointer { np in
            ctarget.withUnsafeBufferPointer { tp in
                extfsk_symlink(raw, dir.ino, np.baseAddress, tp.baseAddress, &ino)
            }
        }
        if rc != 0 { throw posixError(-rc) }
        return (EXTItem(ino: ino, parent: dir.ino), name)
    }

    func createLink(to item: FSItem, named name: FSFileName,
                    inDirectory directory: FSItem) async throws -> FSFileName {
        throw posixError(Int32(POSIXError.ENOTSUP.rawValue))
    }
}
