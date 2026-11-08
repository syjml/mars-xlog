#include <mars/xlog/xloggerbase.h>

#if defined(__ANDROID__)
#include <android/log.h>
#else
#include <cstdio>
#endif

namespace mars::xlog {

void ConsoleLog(const XLoggerInfo* info, const char* log) {
    if (!info || !log || info->level < kLevelVerbose || info->level >= kLevelNone)
        return;

    const char* tag = info->tag ? info->tag : "";
#if defined(__ANDROID__)
    constexpr int priority[] = {ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
                                ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL};
    __android_log_write(priority[info->level], tag, log);
#else
    constexpr char levels[] = "VDIWEF";
    std::fprintf(stderr, "[%c][%s] %s\n", levels[info->level], tag, log);
#endif
}

}  // namespace mars::xlog
