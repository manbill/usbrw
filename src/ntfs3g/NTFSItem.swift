//
//  NTFSItem.swift
//  USBRW — FSItem 子类:MFT 记录号即文件标识
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//

import Foundation
import FSKit

final class NTFSItem: FSItem {

    /// NTFS MFT 记录号(5 = 根目录)。
    let mft: UInt64
    /// 父目录 MFT(根目录自身用 parentOfRoot 语义)。
    let parentMFT: UInt64

    init(mft: UInt64, parent: UInt64 = 5) {
        self.mft = mft
        self.parentMFT = parent
        super.init()
        // fileID 不在这里设置:FSItem 是不透明对象,
        // 标识通过 attributes(of:)/packEntry(itemID:) 按需上报。
    }
}
