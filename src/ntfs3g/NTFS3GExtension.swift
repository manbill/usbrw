//
//  NTFS3GExtension.swift
//  USBRW — FSKit 文件系统模块入口
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//

import Foundation
import FSKit

@main
struct NTFS3GExtension: UnaryFileSystemExtension {

    var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {
        NTFSFileSystem()
    }
}
