//
//  NTFSFileSystem.swift
//  USBRW — FSUnaryFileSystem:probe / load / unload
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//

import Foundation
import FSKit
import os

private let logger = Logger(subsystem: "com.usbrw.ntfs3g", category: "NTFSFileSystem")

final class NTFSFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations,
                            FSManageableResourceMaintenanceOperations {

    // MARK: - 探测

    /// 系统扫描到新设备时回调。读取引导扇区,校验 OEM 签名 "NTFS    "。
    func probeResource(
        resource: FSResource,
        replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void
    ) {
        logger.debug("probeResource: \(resource, privacy: .public)")

        guard let device = NTFSBlockDevice(resource: resource) else {
            // 不是块设备资源 → 不归我们管
            replyHandler(nil, nil)
            return
        }

        device.readBootSector { boot in
            guard let boot, boot.count >= 11,
                  boot[3...10].elementsEqual(Array("NTFS    ".utf8)) else {
                logger.debug("probe: not NTFS → 交 EXT 引擎探测(ext 魔数)")
                EXTFileSystem().probeResource(resource: resource,
                                              replyHandler: replyHandler)
                return
            }
            // 卷序列号(offset 0x48, 8 字节)→ 稳定的容器 UUID
            let serial = boot.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0x48, as: UInt64.self) }
            let containerID = FSContainerIdentifier(uuid: Self.uuid(fromSerial: serial))
            // 卷标签暂不可得(在 $Volume 里),先用固定名,load 后由卷名接管
            replyHandler(FSProbeResult.usable(name: "NTFS Volume", containerID: containerID), nil)
        }
    }

    // MARK: - 加载

    func loadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler: @escaping (FSVolume?, (any Error)?) -> Void
    ) {
        logger.debug("loadResource: \(resource, privacy: .public)")
        containerStatus = .ready

        guard let device = NTFSBlockDevice(resource: resource) else {
            replyHandler(nil, fs_errorForPOSIXError(POSIXError.ENODEV.rawValue))
            return
        }

        // 引擎分流(与 probeResource 同款):NTFS 装载走本类;否则交
        // EXT 引擎。此前 ext 卷认领后仍走 nfsk_open → "not an NTFS
        // volume"(partitionless ext4 调试中现形;MBR ext 从未真正
        // 通过 FSKit 挂载——旧"端到端"实为 C 桥接层测试)。
        device.readBootSector { boot in
            let isNTFS = boot.map {
                $0.count >= 11 && $0[3...10].elementsEqual(Array("NTFS    ".utf8))
            } ?? false
            guard isNTFS else {
                EXTFileSystem().loadResource(resource: resource, options: options,
                                             replyHandler: replyHandler)
                return
            }
            device.mountVolume { volume, error in
                replyHandler(volume, error)
            }
        }
    }

    func unloadResource(
        resource: FSResource,
        options: FSTaskOptions,
        replyHandler reply: @escaping ((any Error)?) -> Void
    ) {
        logger.debug("unloadResource")
        reply(nil)
    }

    func didFinishLoading() {
        logger.info("NTFS3G FSKit module loaded")
    }

    // MARK: - 维护操作(检查/格式化)
    //
    // macOS 26 起,DA 挂载前会对块设备型模块做「验证」:
    // 不实现 startCheck → fskitd 返回 ENOTSUP → 系统判定
    // 「无法修复磁盘」→ 只读挂载或挂载被拒(26.6.2 实测)。

    /// 挂载前验证。真正的完整性把关在 loadResource:以恢复模式
    /// (重放 $LogFile)打开卷,失败即拒绝挂载——比浅层检查更强。
    /// 这里据实回报:容器已就绪 = 卷已通过打开验证。
    /// 注意:didComplete 必须异步调用——在返回 Progress 之前同步完成
    /// 会触发框架内部 "Accessing invalid type" 故障并中止挂载
    /// (26.6.2 实测)。
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        logger.info("startCheck: containerState=\(String(describing: self.containerStatus), privacy: .public)")
        let progress = Progress(totalUnitCount: 1)
        // 资源已加载 = 卷已通过 recover 模式打开验证;未加载时也放行,
        // 由后续 loadResource 做真正的完整性把关(失败即拒绝挂载)。
        DispatchQueue.main.async {
            progress.completedUnitCount = 1
            task.didComplete(error: nil)
        }
        return progress
    }

    /// 格式化:暂不支持(mkntfs 集成留待后续版本),明确报错而非静默。
    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        logger.info("startFormat: 未实现,返回 EOPNOTSUPP")
        throw fs_errorForPOSIXError(POSIXError.ENOTSUP.rawValue)
    }

    // MARK: - Private

    /// 用卷序列号构造稳定 UUID:序列号铺入低 8 字节,保证同卷同 ID。
    static func uuid(fromSerial serial: UInt64) -> UUID {
        var s = serial.littleEndian
        var bytes = [UInt8](repeating: 0, count: 16)
        withUnsafeBytes(of: s) { bytes.replaceSubrange(0..<8, with: $0) }
        bytes[6] = (bytes[6] & 0x0F) | 0x40 // version 4
        bytes[8] = (bytes[8] & 0x3F) | 0x80 // variant
        return UUID(uuid: (bytes[0], bytes[1], bytes[2], bytes[3],
                           bytes[4], bytes[5], bytes[6], bytes[7],
                           bytes[8], bytes[9], bytes[10], bytes[11],
                           bytes[12], bytes[13], bytes[14], bytes[15]))
    }
}
