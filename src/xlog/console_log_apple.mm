#import <Foundation/Foundation.h>
#include <os/log.h>

#include <atomic>
#include <cstdio>

#include <mars/xlog/appender.h>
#include <mars/xlog/xloggerbase.h>

namespace mars::xlog {
namespace {
std::atomic<TConsoleFun> console_fun{kConsoleOSLog};
}

void appender_set_console_fun(TConsoleFun fun) {
    console_fun = fun;
}

void ConsoleLog(const XLoggerInfo* info, const char* log) {
    if (!info || !log || info->level < kLevelVerbose || info->level >= kLevelNone)
        return;

    const char* tag = info->tag ? info->tag : "";
    constexpr char levels[] = "VDIWEF";
    const char level = levels[info->level];
    switch (console_fun.load()) {
        case kConsolePrintf:
            std::printf("[%c][%s] %s\n", level, tag, log);
            break;
        case kConsoleNSLog:
            @autoreleasepool {
                NSLog(@"[%c][%s] %s", level, tag, log);
            }
            break;
        case kConsoleOSLog: {
            constexpr os_log_type_t types[] = {OS_LOG_TYPE_DEBUG, OS_LOG_TYPE_DEBUG, OS_LOG_TYPE_INFO,
                                                OS_LOG_TYPE_INFO, OS_LOG_TYPE_ERROR, OS_LOG_TYPE_FAULT};
            os_log_with_type(OS_LOG_DEFAULT, types[info->level], "[%{public}s] %{private}s", tag, log);
            break;
        }
    }
}

}  // namespace mars::xlog
