/*  Copyright (c) 2013-2015 Tencent. All rights reserved.  */
/**
 * created on : 2012-11-28
 * author : 叶润桂
 */
#include <stdio.h>

#include <functional>

#include "xlog/xlogger/loginfo_extract.h"
#include "mars/xlog/xloggerbase.h"

namespace mars {
namespace xlog {

#ifdef NDEBUG
std::function<void(char* _log)> g_console_log_fun = nullptr;
#else
std::function<void(char* _log)> g_console_log_fun = [](char* _log) {
    ::OutputDebugStringA(_log);
};
#endif
void ConsoleLog(const XLoggerInfo* _info, const char* _log) {
    if (NULL == _info || NULL == _log)
        return;
    static const char* levelStrings[] = {
        "V",
        "D",  // debug
        "I",  // info
        "W",  // warn
        "E",  // error
        "F"   // fatal
    };
    char strFuncName[128] = {0};
    ExtractFunctionName(_info->func_name, strFuncName, sizeof(strFuncName));
    const char* file_name = ExtractFileName(_info->filename);
    char log[16 * 1024] = {0};
    snprintf(log,
             sizeof(log),
             "[%s][%s][%s, %s, %d][%s\n",
             levelStrings[_info->level],
             NULL == _info->tag ? "" : _info->tag,
             file_name,
             strFuncName,
             _info->line,
             _log);
    if (nullptr != g_console_log_fun) {
        g_console_log_fun(log);
    }
}
}  // namespace xlog
}  // namespace mars