#include "data_protection.h"

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <Foundation/Foundation.h>
#endif

void mars::comm::SetLogDirectoryProtection(const char* path) {
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSString* directory = [NSString stringWithUTF8String:path];
        if (directory) {
            [[NSFileManager defaultManager] setAttributes:@{NSFileProtectionKey: NSFileProtectionNone}
                                            ofItemAtPath:directory
                                                   error:nullptr];
        }
    }
#else
    (void)path;
#endif
}
