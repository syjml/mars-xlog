
#include "xlogger_category.h"

#include <cstdio>
#include <utility>

#include "xlog/xlogger_appender.h"

namespace mars {
namespace comm {

XloggerCategory::XloggerCategory(std::shared_ptr<xlog::XloggerAppender> appender)
: appender_(std::move(appender)) {}

xlog::XloggerAppender& XloggerCategory::GetAppender() const {
    return *appender_;
}

TLogLevel XloggerCategory::GetLevel() {
    return level_;
}

void XloggerCategory::SetLevel(TLogLevel _level) {
    level_ = _level;
}

bool XloggerCategory::IsEnabledFor(TLogLevel _level) {
    return level_ <= _level;
}

void XloggerCategory::VPrint(const XLoggerInfo* _info, const char* _format, va_list _list) {
    if (NULL == _format) {
        XLoggerInfo* info = (XLoggerInfo*)_info;
        info->level = kLevelFatal;
        __WriteImpl(_info, "NULL == _format");
    } else {
        char temp[4096] = {'\0'};
        std::vsnprintf(temp, sizeof(temp), _format, _list);
        __WriteImpl(_info, temp);
    }
}

void XloggerCategory::Print(const XLoggerInfo* _info, const char* _format, ...) {
    va_list valist;
    va_start(valist, _format);
    VPrint(_info, _format, valist);
    va_end(valist);
}

void XloggerCategory::Write(const XLoggerInfo* _info, const char* _log) {
    __WriteImpl(_info, _log);
}

void XloggerCategory::__WriteImpl(const XLoggerInfo* _info, const char* _log) {
    if (_info && _info->level < level_) {
        return;
    }

    if (_info && -1 == _info->pid && -1 == _info->tid && -1 == _info->maintid) {
        XLoggerInfo* info = (XLoggerInfo*)_info;
        info->pid = xlogger_pid();
        info->tid = xlogger_tid();
        info->maintid = xlogger_maintid();
    }

    if (NULL == _log) {
        if (_info) {
            XLoggerInfo* info = (XLoggerInfo*)_info;
            info->level = kLevelFatal;
        }
        appender_->Write(_info, "NULL == _log");
    } else {
        appender_->Write(_info, _log);
    }
}

}  // namespace comm
}  // namespace mars
