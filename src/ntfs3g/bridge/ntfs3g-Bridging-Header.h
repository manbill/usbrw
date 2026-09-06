//
//  ntfs3g-Bridging-Header.h
//  USBRW ntfs3g FSKit extension
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//

#ifndef USBRW_NTFS3G_BRIDGING_HEADER_H
#define USBRW_NTFS3G_BRIDGING_HEADER_H

#include "ntfs_fskit.h"
#include "ext_fskit.h"

#import <Foundation/Foundation.h>

/* FSKCompat.m:FSKit ObjC-only API(metadataFlush)的 Swift 桥 */
@class FSBlockDeviceResource;
@interface USBRWFSKCompat : NSObject
+ (BOOL)flushResource:(FSBlockDeviceResource *)res
                error:(NSError **)error;
@end

#endif
