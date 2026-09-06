//
//  FSKCompat.m
//  USBRW — FSKit ObjC-only API 的 Swift 可达封装
//
//  SPDX-License-Identifier: GPL-2.0-or-later
//
//  metadataFlushWithError: 在 FSResource.h(ObjC)里声明,但当前 SDK 的
//  swiftinterface 未导出,Swift 侧不可见。经此桥调用。
//

#import <Foundation/Foundation.h>
#import <FSKit/FSKit.h>

@interface USBRWFSKCompat : NSObject
/* 冲刷资源缓冲缓存到物理设备;旧系统无此 API 时视为成功
 * (plain writeFrom: 本就直达设备,刷的仅是 delayedMetadata 缓存)。 */
+ (BOOL)flushResource:(FSBlockDeviceResource *)res
                error:(NSError **)error;
@end

@implementation USBRWFSKCompat

+ (BOOL)flushResource:(FSBlockDeviceResource *)res
                error:(NSError **)error
{
    if (![res respondsToSelector:@selector(metadataFlushWithError:)]) {
        return YES;
    }
    return [res metadataFlushWithError:error];
}

@end
