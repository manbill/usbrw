//
//  SharedSettings.swift
//  USBRW — app 与扩展共享的设置(App Group UserDefaults)
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  通道:87LTRAZ45P.group.com.usbrw.shared(两侧 entitlements 均已声明)。
//  app 侧写(SettingsView),扩展挂载时读(NTFSBlockDevice)。
//
//  ⚠️ 通道生效前提:App Group 必须出现在 provisioning profile 中——
//  沙盒扩展除自身容器与组容器外不可读任何路径(2026-09-01 实测
//  /Users/Shared 亦被拒,isReadableFile=false);而组进 profile 的唯一
//  已知路径是 Xcode UI 的 App Groups capability(触发门户注册)。
//  手写 entitlement 只进签名、不进 profile(实测三种重建均无效)。
//

import Foundation

enum SharedSettings {

    static let suiteName = "87LTRAZ45P.group.com.usbrw.shared"
    static let forceReadOnlyKey = "forceReadOnly"
    /// app 本地镜像(group 未注册时的回退;仅 app 侧有意义)。
    private static let localMirrorKey = "forceReadOnly.localMirror"

    static var defaults: UserDefaults {
        UserDefaults(suiteName: suiteName) ?? .standard
    }

    /// 用户偏好:新盘默认只读挂载(写保护优先)。
    static var forceReadOnly: Bool {
        get {
            if let d = UserDefaults(suiteName: suiteName),
               d.object(forKey: forceReadOnlyKey) != nil {
                return d.bool(forKey: forceReadOnlyKey)
            }
            return UserDefaults.standard.bool(forKey: localMirrorKey)
        }
        set {
            defaults.set(newValue, forKey: forceReadOnlyKey)
            UserDefaults.standard.set(newValue, forKey: localMirrorKey)
        }
    }

    // MARK: - 按设备例外(吸收 Nigate"尊重你的选择"语义)

    /// 按 NTFS 卷序列号(引导扇区 0x48,8 字节小端 → 16 位 hex)的
    /// 只读例外:返回 nil = 跟随全局,true = 此盘只读,false = 此盘读写。
    /// 键空间与全局项同套件,扩展挂载时同样可读。
    static func roOverride(serial: String) -> Bool? {
        let key = "ro.\(serial)"
        if let d = UserDefaults(suiteName: suiteName),
           d.object(forKey: key) != nil {
            return d.bool(forKey: key)
        }
        if UserDefaults.standard.object(forKey: "mirror.\(key)") != nil {
            return UserDefaults.standard.bool(forKey: "mirror.\(key)")
        }
        return nil
    }

    static func setROOverride(serial: String, value: Bool?) {
        let key = "ro.\(serial)"
        if let value {
            defaults.set(value, forKey: key)
            UserDefaults.standard.set(value, forKey: "mirror.\(key)")
        } else {
            defaults.removeObject(forKey: key)
            UserDefaults.standard.removeObject(forKey: "mirror.\(key)")
        }
    }

    /// 该卷的最终只读决策:按盘例外 ?? 全局默认。
    static func shouldMountReadOnly(serial: String) -> Bool {
        roOverride(serial: serial) ?? forceReadOnly
    }

    // MARK: - 分层挂载选项(组通道下发,扩展挂载时读)

    /// 选项键(与桥接 NFSK_OPEN_* 位对应;值语义同 ntfs-3g 同名选项)。
    static let optKeys: [(key: String, bit: Int32)] = [
        ("opt.windowsNames",  0x2),
        ("opt.hideHidFiles",  0x4),
        ("opt.hideDotFiles",  0x8),
        ("opt.noatime",       0x10),
        ("opt.noRecover",     0x20),
        ("opt.noCompression", 0x40),  // Tier2:禁用压缩读写(默认开)
        ("opt.showSysFiles",  0x80),  // Tier2:显示 $ 系统元数据文件
    ]

    static func mountOpt(_ key: String) -> Bool {
        if let d = UserDefaults(suiteName: suiteName),
           d.object(forKey: key) != nil {
            return d.bool(forKey: key)
        }
        return UserDefaults.standard.bool(forKey: "mirror.\(key)")
    }

    static func setMountOpt(_ key: String, value: Bool) {
        defaults.set(value, forKey: key)
        UserDefaults.standard.set(value, forKey: "mirror.\(key)")
    }

    /// 当前选项合成的桥接 opts 位掩码。
    static var mountOptsMask: Int32 {
        optKeys.reduce(0) { $0 | (mountOpt($1.key) ? $1.bit : 0) }
    }

    // MARK: - 休眠卷救援(一次性)

    /// 一次性救援标记:app 侧"删除休眠文件并读写挂载"按钮写入;
    /// 扩展挂载时读取,若置位则以 IGNORE_HIBERFILE 挂载并删除
    /// hiberfil.sys,随后清除标记(一次性)。Windows 未保存会话将丢失,
    /// 由 app 侧在确认弹窗里明确告知。
    static func consumeHiberfileRescue(serial: String) -> Bool {
        let key = "rescue.\(serial)"
        var hit = false
        if let d = UserDefaults(suiteName: suiteName), d.bool(forKey: key) {
            hit = true
            d.removeObject(forKey: key)
        }
        if UserDefaults.standard.bool(forKey: "mirror.\(key)") {
            hit = true
            UserDefaults.standard.removeObject(forKey: "mirror.\(key)")
        }
        return hit
    }

    static func setHiberfileRescue(serial: String) {
        let key = "rescue.\(serial)"
        defaults.set(true, forKey: key)
        UserDefaults.standard.set(true, forKey: "mirror.\(key)")
    }
}
