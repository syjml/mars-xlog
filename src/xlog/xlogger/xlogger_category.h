
#ifndef MARS_COMM_XLOGGER_CATEGORY_H_
#define MARS_COMM_XLOGGER_CATEGORY_H_

#include <atomic>
#include <memory>

#include "mars/xlog/xloggerbase.h"

namespace mars {
namespace xlog {
class XloggerAppender;
}
namespace comm {

class XloggerCategory {
 public:
    explicit XloggerCategory(std::shared_ptr<xlog::XloggerAppender> appender);
    xlog::XloggerAppender& GetAppender() const;
    TLogLevel GetLevel();
    void SetLevel(TLogLevel _level);
    bool IsEnabledFor(TLogLevel _level);
    void VPrint(const XLoggerInfo* _info, const char* _format, va_list _list);
    void Print(const XLoggerInfo* _info, const char* _format, ...);
    void Write(const XLoggerInfo* _info, const char* _log);

 private:
    void __WriteImpl(const XLoggerInfo* _info, const char* _log);

 private:
    std::atomic<TLogLevel> level_{kLevelNone};
    std::shared_ptr<xlog::XloggerAppender> appender_;
};

}  // namespace comm
}  // namespace mars

#endif  // MARS_COMM_XLOGGER_CATEGORY_H_
