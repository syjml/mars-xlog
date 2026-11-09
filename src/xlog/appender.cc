// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

/*
 * appender.h
 *
 *  Created on: 2013-3-7
 *      Author: yerungui
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <inttypes.h>
#include <stdio.h>

#include "mars/xlog/appender.h"

#ifdef _WIN32
#ifndef __MINGW32__
#define snprintf _snprintf
#undef S_ISDIR
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif
#define strcasecmp _stricmp
#define fileno _fileno
#endif

#include <assert.h>
#include <ctype.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "comm/assert/__assert.h"
#include "comm/autobuffer.h"
#include "comm/delayed_task.h"
#include "comm/mmap_util.h"
#include "comm/ptrbuffer.h"
#include "mars/comm/strutil.h"
#include "comm/time_utils.h"
#include "comm/verinfo.h"
#include "mars/xlog/xloggerbase.h"

#ifdef __APPLE__
#include "comm/apple/data_protection.h"
#endif

#include "log_base_buffer.h"
#include "log_zlib_buffer.h"
#ifdef XLOG_ENABLE_ZSTD
#include "log_zstd_buffer.h"
#endif
#include "xlogger_appender.h"

#define LOG_EXT "xlog"

using namespace mars::comm;

namespace mars {
namespace xlog {

extern void log_formater(const XLoggerInfo* _info, const char* _logbody, PtrBuffer& _log);
extern void ConsoleLog(const XLoggerInfo* _info, const char* _log);

static const int kMaxDumpLength = 4096;

static const unsigned int kBufferBlockLength = 150 * 1024;
static const long kMinLogAliveTime = 24 * 60 * 60;  // 1 days in second

static std::mutex sg_mutex_dir_attr;

void (*g_log_write_callback)(const XLoggerInfo*, const char*) = nullptr;

namespace {
namespace fs = std::filesystem;

fs::path PathFromUtf8(std::string_view bytes) {
#ifdef _WIN32
    return fs::path(std::u8string(bytes.begin(), bytes.end()));
#else
    return fs::path(bytes);
#endif
}

std::string PathToUtf8(const fs::path& path) {
#ifdef _WIN32
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
#else
    return path.native();
#endif
}

std::string MmapFilePath(const XLogConfig& config) {
    return (config.cachedir_.empty() ? config.logdir_ : config.cachedir_) + "/" + config.nameprefix_ + ".mmap3";
}

std::chrono::milliseconds LogClockNow() {
#ifdef __ANDROID__
    // Preserve elapsed time across device suspend for clock-change detection.
    return std::chrono::milliseconds(gettickcount());
#else
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
#endif
}

std::unique_ptr<LogBaseBuffer> MakeLogBuffer(void* data, const XLogConfig& config) {
#ifdef XLOG_ENABLE_ZSTD
    if (config.compress_mode_ == kZstd)
        return std::make_unique<LogZstdBuffer>(data, kBufferBlockLength, true, config.pub_key_.c_str(),
                                               config.compress_level_);
#endif
    return std::make_unique<LogZlibBuffer>(data, kBufferBlockLength, true, config.pub_key_.c_str());
}

class ScopeErrno {
 public:
    ScopeErrno() {
        m_errno = errno;
    }
    ~ScopeErrno() {
        errno = m_errno;
    }

 private:
    ScopeErrno(const ScopeErrno&);
    const ScopeErrno& operator=(const ScopeErrno&);

 private:
    int m_errno;
};

#define SCOPE_ERRNO() SCOPE_ERRNO_I(__LINE__)
#define SCOPE_ERRNO_I(line) SCOPE_ERRNO_II(line)
#define SCOPE_ERRNO_II(line) ScopeErrno __scope_errno_##line

}  // namespace

std::shared_ptr<XloggerAppender> XloggerAppender::NewInstance(const XLogConfig& config, uint64_t max_byte_size) {
    auto appender = std::shared_ptr<XloggerAppender>(new XloggerAppender(config, max_byte_size));
    appender->Open(config);
    return appender;
}

std::unique_ptr<XloggerAppender> XloggerAppender::NewOneShot(const XLogConfig& config, uint64_t max_byte_size) {
    return std::unique_ptr<XloggerAppender>(new XloggerAppender(config, max_byte_size));
}

XloggerAppender::XloggerAppender(const XLogConfig& config, uint64_t max_byte_size)
: config_(config), mode_(config.mode_), max_file_size_(max_byte_size) {}

XloggerAppender::~XloggerAppender() {
    StopBackgroundTasks();
}

void XloggerAppender::Write(const XLoggerInfo* _info, const char* _log) {
    if (log_close_)
        return;

    SCOPE_ERRNO();

    thread_local uint32_t recursion_count = 0;
    thread_local std::string recursion_str;
    if (recursion_count >= 10)
        return;
    ++recursion_count;
    struct RecursionGuard {
        uint32_t& depth;
        ~RecursionGuard() { --depth; }
    } guard{recursion_count};

    if (consolelog_open_)
        ConsoleLog(_info, _log);
#ifdef __ANDROID__
    else if (_info && _info->traceLog == 1)
        ConsoleLog(_info, _log);
#endif
    if (g_log_write_callback) {
        g_log_write_callback(_info, _log);
    }

    if (2 <= recursion_count && recursion_str.empty()) {
        recursion_str.resize(kMaxDumpLength);
        XLoggerInfo info = _info ? *_info : XLoggerInfo{};
        info.level = kLevelFatal;

        char recursive_log[256] = {0};
        snprintf(recursive_log,
                 sizeof(recursive_log),
                 "ERROR!!! xlogger_appender Recursive calls!!!, count:%u",
                 recursion_count);

        PtrBuffer tmp((void*)recursion_str.data(), 0, kMaxDumpLength);
        log_formater(&info, recursive_log, tmp);

        if (recursion_str.capacity() >= strnlen(_log, kMaxDumpLength)) {
            recursion_str += _log;
        }

        ConsoleLog(&info, recursion_str.c_str());
    } else {
        if (!recursion_str.empty()) {
            WriteTips2File(recursion_str.c_str());
            recursion_str.clear();
        }

        WriteRecord(_info, _log);
    }
}

void XloggerAppender::WriteRecord(const XLoggerInfo* info, const char* log) {
    if (mode_ == kAppenderSync)
        __WriteSync(info, log);
    else
        __WriteAsync(info, log);
}

void XloggerAppender::SetMode(TAppenderMode _mode) {
    std::lock_guard lock(mutex_buffer_async_);
    if (log_close_)
        return;
    if (_mode == kAppenderAsync && !thread_async_.joinable()) {
        thread_async_ = std::thread(&XloggerAppender::__AsyncLogThread, this);
    }
    mode_ = _mode;
    flush_requested_ = true;
    cond_buffer_async_.notify_one();
}

void XloggerAppender::Flush() {
    std::lock_guard lock(mutex_buffer_async_);
    flush_requested_ = true;
    cond_buffer_async_.notify_one();
}

void XloggerAppender::FlushSync() {
    if (kAppenderSync == mode_) {
        return;
    }

    std::lock_guard sync_write(mutex_sync_write_);
    std::unique_lock lock_buffer(mutex_buffer_async_);

    if (log_close_ || !log_buff_)
        return;

    AutoBuffer tmp;
    log_buff_->Flush(tmp);

    lock_buffer.unlock();

    if (tmp.Ptr())
        __Log2File(tmp.Ptr(), tmp.Length(), false);
}

void XloggerAppender::StopBackgroundTasks() {
    {
        std::lock_guard lock(mutex_buffer_async_);
        log_close_ = true;
    }
    cond_buffer_async_.notify_one();

    for (auto* task : {thread_timeout_cache_.get(), thread_moveold_.get(), thread_timeout_log_.get()}) {
        if (task)
            task->RequestCancel();
    }
    thread_timeout_cache_.reset();
    thread_moveold_.reset();
    thread_timeout_log_.reset();

    if (thread_async_.joinable())
        thread_async_.join();
}

void XloggerAppender::Close() {
    if (!log_close_) {
        char mark_info[512] = {0};
        __GetMarkInfo(mark_info, sizeof(mark_info));
        char appender_info[728] = {0};
        snprintf(appender_info, sizeof(appender_info), "$$$$$$$$$$" __DATE__ "$$$" __TIME__ "$$$$$$$$$$%s\n", mark_info);
        WriteRecord(nullptr, appender_info);
    }

    StopBackgroundTasks();

    // A foreground flush may already have taken ownership of pending records.
    std::lock_guard sync_write(mutex_sync_write_);
    // Retire the buffer after both encoding and file error reporting have finished.
    std::scoped_lock lock(mutex_buffer_async_, mutex_log_file_);
    log_buff_.reset();
    if (mmap_file_.data()) {
        memset(mmap_file_.data(), 0, kBufferBlockLength);
        mmap_file_.Close();
    }
    heap_buffer_.reset();
    __CloseLogFile();
}

void XloggerAppender::Open(const XLogConfig& _config) {
    std::unique_lock dir_attr_lock(sg_mutex_dir_attr);
    if (!config_.cachedir_.empty()) {
        fs::create_directories(PathFromUtf8(config_.cachedir_));

        thread_timeout_cache_ = std::make_unique<DelayedTask>(std::chrono::minutes(2), [this] {
            __DelTimeoutFile(config_.cachedir_);
        });
        thread_moveold_ = std::make_unique<DelayedTask>(std::chrono::minutes(3), [this] {
            __MoveOldFiles(config_.cachedir_, config_.logdir_, config_.nameprefix_);
        });
#ifdef __APPLE__
        comm::SetLogDirectoryProtection(config_.cachedir_.c_str());
#endif
    }

    thread_timeout_log_ = std::make_unique<DelayedTask>(std::chrono::minutes(2), [this] {
        __DelTimeoutFile(config_.logdir_);
    });
    fs::create_directories(PathFromUtf8(config_.logdir_));
#ifdef __APPLE__
    comm::SetLogDirectoryProtection(config_.logdir_.c_str());
#endif
    dir_attr_lock.unlock();

    const auto tick = LogClockNow();

    const bool use_mmap = mmap_file_.Open(PathFromUtf8(MmapFilePath(config_)), kBufferBlockLength);
    if (!use_mmap)
        heap_buffer_ = std::make_unique<char[]>(kBufferBlockLength);
    log_buff_ = MakeLogBuffer(use_mmap ? mmap_file_.data() : heap_buffer_.get(), config_);

    AutoBuffer buffer;
    log_buff_->Flush(buffer);

    mode_ = _config.mode_;
    log_close_ = false;

    char mark_info[512] = {0};
    __GetMarkInfo(mark_info, sizeof(mark_info));

    if (buffer.Ptr()) {
        WriteTips2File("~~~~~ begin of mmap ~~~~~\n");
        __Log2File(buffer.Ptr(), buffer.Length(), false);
        WriteTips2File("~~~~~ end of mmap ~~~~~%s\n", mark_info);
    }

    const auto get_mmap_time = LogClockNow() - tick;

    char appender_info[728] = {0};
    snprintf(appender_info, sizeof(appender_info), "^^^^^^^^^^" __DATE__ "^^^" __TIME__ "^^^^^^^^^^%s", mark_info);

    WriteRecord(nullptr, appender_info);
    char logmsg[256] = {0};
    snprintf(logmsg, sizeof(logmsg), "get mmap time: %" PRId64, static_cast<int64_t>(get_mmap_time.count()));
    WriteRecord(nullptr, logmsg);

    WriteRecord(nullptr, "MARS_URL: " MARS_URL);
    WriteRecord(nullptr, "MARS_PATH: " MARS_PATH);
    WriteRecord(nullptr, "MARS_REVISION: " MARS_REVISION);
    WriteRecord(nullptr, "MARS_BUILD_TIME: " MARS_BUILD_TIME);
    WriteRecord(nullptr, "MARS_BUILD_JOB: " MARS_TAG);

    snprintf(logmsg, sizeof(logmsg), "log appender mode:%d, use mmap:%d", static_cast<int>(mode_.load()), use_mmap);
    WriteRecord(nullptr, logmsg);

    if (!config_.cachedir_.empty()) {
        fs::space_info info = fs::space(PathFromUtf8(config_.cachedir_));
        snprintf(logmsg,
                 sizeof(logmsg),
                 "cache dir space info, capacity:%" PRIuMAX " free:%" PRIuMAX " available:%" PRIuMAX,
                 info.capacity,
                 info.free,
                 info.available);
        WriteRecord(nullptr, logmsg);
    }

    fs::space_info info = fs::space(PathFromUtf8(config_.logdir_));
    snprintf(logmsg,
             sizeof(logmsg),
             "log dir space info, capacity:%" PRIuMAX " free:%" PRIuMAX " available:%" PRIuMAX,
             info.capacity,
             info.free,
             info.available);
    WriteRecord(nullptr, logmsg);
    SetMode(_config.mode_);
}

std::string XloggerAppender::__MakeLogFileNamePrefix(const timeval& _tv, const char* _prefix) {
    time_t sec = _tv.tv_sec;
    tm tcur = mars_local_time(sec);

    char temp[64] = {0};
    snprintf(temp, 64, "_%d%02d%02d", 1900 + tcur.tm_year, 1 + tcur.tm_mon, tcur.tm_mday);

    std::string filenameprefix = _prefix;
    filenameprefix += temp;

    return filenameprefix;
}

void XloggerAppender::__GetFileNamesByPrefix(const std::string& _logdir,
                                             const std::string& _fileprefix,
                                             const std::string& _fileext,
                                             std::vector<std::string>& _filename_vec) {
    const auto path = PathFromUtf8(_logdir);
    if (!fs::is_directory(path)) {
        return;
    }

    fs::directory_iterator end_iter;
    std::string filename;

    for (fs::directory_iterator iter(path); iter != end_iter; ++iter) {
        if (fs::is_regular_file(iter->status())) {
            filename = PathToUtf8(iter->path().filename());
            if (strutil::StartsWith(filename, _fileprefix) && strutil::EndsWith(filename, _fileext)) {
                _filename_vec.push_back(filename);
            }
        }
    }
}

void XloggerAppender::__GetFilePathsFromTimeval(const timeval& _tv,
                                                const std::string& _logdir,
                                                const char* _prefix,
                                                const std::string& _fileext,
                                                std::vector<std::string>& _filepath_vec) {
    std::string fileprefix = __MakeLogFileNamePrefix(_tv, _prefix);
    std::vector<std::string> filename_vec;
    __GetFileNamesByPrefix(_logdir, fileprefix, _fileext, filename_vec);

    for (std::vector<std::string>::iterator iter = filename_vec.begin(); iter != filename_vec.end(); ++iter) {
        _filepath_vec.push_back(_logdir + "/" + (*iter));
    }
}

static bool __string_compare_greater(const std::string& s1, const std::string& s2) {
    if (s1.length() == s2.length()) {
        return s1 > s2;
    }
    return s1.length() > s2.length();
}

long XloggerAppender::__GetNextFileIndex(const std::string& _fileprefix, const std::string& _fileext) {
    std::vector<std::string> filename_vec;
    __GetFileNamesByPrefix(config_.logdir_, _fileprefix, _fileext, filename_vec);
    if (!config_.cachedir_.empty()) {
        __GetFileNamesByPrefix(config_.cachedir_, _fileprefix, _fileext, filename_vec);
    }

    long index = 0;  // long is enought to hold all indexes in one day.
    if (filename_vec.empty()) {
        return index;
    }
    // high -> low
    std::sort(filename_vec.begin(), filename_vec.end(), __string_compare_greater);
    std::string last_filename = *(filename_vec.begin());
    std::size_t ext_pos = last_filename.rfind("." + _fileext);
    std::size_t index_len = ext_pos - _fileprefix.length();
    if (index_len > 0) {
        std::string index_str = last_filename.substr(_fileprefix.length(), index_len);
        if (strutil::StartsWith(index_str, "_")) {
            index_str = index_str.substr(1);
        }
        index = atol(index_str.c_str());
    }

    uint64_t filesize = 0;
    const auto logfilepath = PathFromUtf8(config_.logdir_ + "/" + last_filename);
    if (fs::exists(logfilepath)) {
        filesize += fs::file_size(logfilepath);
    }
    if (!config_.cachedir_.empty()) {
        const auto cachefilepath = PathFromUtf8(config_.cachedir_ + "/" + last_filename);
        if (fs::exists(cachefilepath)) {
            filesize += fs::file_size(cachefilepath);
        }
    }
    return (filesize > max_file_size_) ? index + 1 : index;
}

void XloggerAppender::__MakeLogFileName(const timeval& _tv,
                                        const std::string& _logdir,
                                        const char* _prefix,
                                        const std::string& _fileext,
                                        char* _filepath,
                                        unsigned int _len) {
    long index = 0;
    std::string logfilenameprefix = __MakeLogFileNamePrefix(_tv, _prefix);
    if (max_file_size_ > 0) {
        index = __GetNextFileIndex(logfilenameprefix, _fileext);
    }

    std::string logfilepath = _logdir;
    logfilepath += "/";
    logfilepath += logfilenameprefix;

    if (index > 0) {
        char temp[24] = {0};
        snprintf(temp, 24, "_%ld", index);
        logfilepath += temp;
    }

    logfilepath += ".";
    logfilepath += _fileext;

    strncpy(_filepath, logfilepath.c_str(), _len - 1);
    _filepath[_len - 1] = '\0';
}

void XloggerAppender::__DelTimeoutFile(const std::string& _log_path) {
    std::unique_lock dir_attr_lock(sg_mutex_dir_attr);
    const auto now = fs::file_time_type::clock::now();

    const auto path = PathFromUtf8(_log_path);

    if (fs::exists(path) && fs::is_directory(path)) {
        fs::directory_iterator end_iter;
        for (fs::directory_iterator iter(path); iter != end_iter; ++iter) {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - fs::last_write_time(iter->path()));

            if (age > std::chrono::seconds(max_alive_time_.load())) {
                if (fs::is_regular_file(iter->status())
                    && iter->path().extension() == (std::string(".") + LOG_EXT)) {
                    fs::remove(iter->path());
                }
                if (fs::is_directory(iter->status())) {
                    std::string filename = PathToUtf8(iter->path().filename());
                    if (filename.size() == 8 && filename.find_first_not_of("0123456789") == std::string::npos) {
                        fs::remove_all(iter->path());
                    }
                }
            }
        }
    }
}

bool XloggerAppender::__AppendFile(const std::string& _src_file, const std::string& _dst_file) {
    if (_src_file == _dst_file) {
        return false;
    }

    const auto source = PathFromUtf8(_src_file);
    if (!fs::exists(source)) {
        return false;
    }

    if (0 == fs::file_size(source)) {
        return true;
    }

    File src_file(fopen(_src_file.c_str(), "rb"));

    if (nullptr == src_file) {
        return false;
    }

    File dest_file(fopen(_dst_file.c_str(), "ab"));
    if (!dest_file)
        return false;

    fseek(src_file.get(), 0, SEEK_END);
    long src_file_len = ftell(src_file.get());
    long dst_file_len = ftell(dest_file.get());
    fseek(src_file.get(), 0, SEEK_SET);

    char buffer[4096] = {0};

    while (true) {
        if (feof(src_file.get()))
            break;

        size_t read_ret = fread(buffer, 1, sizeof(buffer), src_file.get());

        if (read_ret == 0)
            break;

        if (ferror(src_file.get()))
            break;

        fwrite(buffer, 1, read_ret, dest_file.get());

        if (ferror(dest_file.get()))
            break;
    }

    if (dst_file_len + src_file_len > ftell(dest_file.get())) {
        ftruncate(fileno(dest_file.get()), dst_file_len);
        return false;
    }


    return true;
}

void XloggerAppender::__MoveOldFiles(const std::string& _src_path,
                                     const std::string& _dest_path,
                                     const std::string& _nameprefix) {
    std::unique_lock dir_attr_lock(sg_mutex_dir_attr);
    if (_src_path == _dest_path) {
        return;
    }

    const auto path = PathFromUtf8(_src_path);
    if (!fs::is_directory(path)) {
        return;
    }

    std::unique_lock lock_file(mutex_log_file_);
    const auto now = fs::file_time_type::clock::now();

    fs::directory_iterator end_iter;
    for (fs::directory_iterator iter(path); iter != end_iter; ++iter) {
        const auto filename = PathToUtf8(iter->path().filename());
        if (!strutil::StartsWith(filename, _nameprefix) || !strutil::EndsWith(filename, LOG_EXT)) {
            continue;
        }

        if (config_.cache_days_ > 0) {
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - fs::last_write_time(iter->path()));
            if (age > std::chrono::seconds::zero() && age < std::chrono::days(config_.cache_days_)) {
                continue;
            }
        }

        if (!__AppendFile(PathToUtf8(iter->path()), config_.logdir_ + "/" + filename)) {
            break;
        }

        fs::remove(iter->path());
    }
}

void XloggerAppender::__GetMarkInfo(char* _info, size_t _info_len) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    time_t sec = tv.tv_sec;
    struct tm tm_tmp = mars_local_time(sec);
    char tmp_time[64] = {0};
    strftime(tmp_time, sizeof(tmp_time), "%Y-%m-%d %z %H:%M:%S", &tm_tmp);
    snprintf(_info, _info_len, "[%" PRIdMAX ",%" PRIdMAX "][%s]", xlogger_pid(), xlogger_tid(), tmp_time);
}

void XloggerAppender::__WriteTips2Console(const char* _tips_format, ...) {
    if (nullptr == _tips_format) {
        return;
    }

    XLoggerInfo info = XLOGGER_INFO_INITIALIZER;
    info.level = kLevelError;

    char tips_info[4096] = {0};
    va_list ap;
    va_start(ap, _tips_format);
    vsnprintf(tips_info, sizeof(tips_info), _tips_format, ap);
    va_end(ap);
    ConsoleLog(&info, tips_info);
}

bool XloggerAppender::__WriteFile(const void* _data, size_t _len, FILE* _file) {
    if (nullptr == _file) {
        assert(false);
        return false;
    }

    long before_len = ftell(_file);
    if (before_len < 0)
        return false;

    if (1 != fwrite(_data, _len, 1, _file)) {
        int err = ferror(_file);

        __WriteTips2Console("write file error:%d", err);

        ftruncate(fileno(_file), before_len);
        fseek(_file, 0, SEEK_END);

        char err_log[256] = {0};
        snprintf(err_log, sizeof(err_log), "\nwrite file error:%d\n", err);

        AutoBuffer tmp_buff;
        log_buff_->Write(err_log, strnlen(err_log, sizeof(err_log)), tmp_buff);

        fwrite(tmp_buff.Ptr(), tmp_buff.Length(), 1, _file);

        return false;
    }

    return true;
}

bool XloggerAppender::__OpenLogFile(const std::string& _log_dir) {
    if (config_.logdir_.empty())
        return false;

    struct timeval tv;
    gettimeofday(&tv, nullptr);

    if (nullptr != logfile_) {
        time_t sec = tv.tv_sec;
        tm tcur = mars_local_time(sec);
        tm filetm = mars_local_time(openfiletime_);

        if (filetm.tm_year == tcur.tm_year && filetm.tm_mon == tcur.tm_mon && filetm.tm_mday == tcur.tm_mday) {
            return true;
        }

        logfile_.reset();
    }

    const auto now_tick = LogClockNow();
    time_t now_time = tv.tv_sec;

    openfiletime_ = tv.tv_sec;

    char logfilepath[1024] = {0};
    __MakeLogFileName(tv, _log_dir, config_.nameprefix_.c_str(), LOG_EXT, logfilepath, 1024);

    if (now_time < last_time_) {
        logfile_.reset(fopen(last_file_path_, "ab"));

        if (nullptr == logfile_) {
            __WriteTips2Console("open file error:%d %s, path:%s", errno, strerror(errno), last_file_path_);
        }

#ifdef __APPLE__
        assert(logfile_);
#endif
        return nullptr != logfile_;
    }

    logfile_.reset(fopen(logfilepath, "ab"));

    if (nullptr == logfile_) {
        __WriteTips2Console("open file error:%d %s, path:%s", errno, strerror(errno), logfilepath);
    }

    const auto elapsed = now_tick - last_tick_;
    if (last_time_ != 0 && std::chrono::seconds(now_time - last_time_)
                               > std::chrono::duration_cast<std::chrono::seconds>(elapsed) + std::chrono::minutes(5)) {
        struct tm tm_tmp = mars_local_time(last_time_);
        char last_time_str[64] = {0};
        strftime(last_time_str, sizeof(last_time_str), "%Y-%m-%d %z %H:%M:%S", &tm_tmp);

        tm_tmp = mars_local_time(now_time);
        char now_time_str[64] = {0};
        strftime(now_time_str, sizeof(now_time_str), "%Y-%m-%d %z %H:%M:%S", &tm_tmp);

        char log[1024] = {0};
        snprintf(log,
                 sizeof(log),
                 "[F][ last log file:%s from %s to %s, time_diff:%" PRIdMAX ", tick_diff:%" PRId64 "\n",
                 last_file_path_,
                 last_time_str,
                 now_time_str,
                 static_cast<intmax_t>(now_time - last_time_),
                 static_cast<int64_t>(elapsed.count()));

        AutoBuffer tmp_buff;
        log_buff_->Write(log, strnlen(log, sizeof(log)), tmp_buff);
        __WriteFile(tmp_buff.Ptr(), tmp_buff.Length(), logfile_.get());
    }

    memcpy(last_file_path_, logfilepath, sizeof(last_file_path_));
    last_tick_ = now_tick;
    last_time_ = now_time;

#ifdef __APPLE__
    assert(logfile_);
#endif
    return nullptr != logfile_;
}

void XloggerAppender::__CloseLogFile() {
    if (nullptr == logfile_)
        return;

    openfiletime_ = 0;
    logfile_.reset();
}

bool XloggerAppender::__CacheLogs() {
    if (config_.cachedir_.empty() || config_.cache_days_ <= 0) {
        return false;
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    char logfilepath[1024] = {0};
    __MakeLogFileName(tv, config_.logdir_, config_.nameprefix_.c_str(), LOG_EXT, logfilepath, 1024);
    if (fs::exists(PathFromUtf8(logfilepath))) {
        return false;
    }

    static const uintmax_t kAvailableSizeThreshold = (uintmax_t)1 * 1024 * 1024 * 1024;  // 1G
    fs::space_info info = fs::space(PathFromUtf8(config_.cachedir_));
    if (info.available < kAvailableSizeThreshold) {
        return false;
    }

    return true;
}

void XloggerAppender::__Log2File(const void* _data, size_t _len, bool _move_file) {
    if (nullptr == _data || 0 == _len || config_.logdir_.empty()) {
        return;
    }

    std::unique_lock lock_file(mutex_log_file_);

    if (!log_buff_)
        return;

    if (config_.cachedir_.empty()) {
        if (__OpenLogFile(config_.logdir_)) {
            __WriteFile(_data, _len, logfile_.get());
            if (kAppenderAsync == mode_) {
                __CloseLogFile();
            }
        }
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    char logcachefilepath[1024] = {0};

    __MakeLogFileName(tv, config_.cachedir_, config_.nameprefix_.c_str(), LOG_EXT, logcachefilepath, 1024);

    bool cache_logs = __CacheLogs();
    if ((cache_logs || fs::exists(PathFromUtf8(logcachefilepath))) && __OpenLogFile(config_.cachedir_)) {
        __WriteFile(_data, _len, logfile_.get());
        if (kAppenderAsync == mode_) {
            __CloseLogFile();
        }

        if (cache_logs || !_move_file) {
            return;
        }

        char logfilepath[1024] = {0};
        __MakeLogFileName(tv, config_.logdir_, config_.nameprefix_.c_str(), LOG_EXT, logfilepath, 1024);
        if (__AppendFile(logcachefilepath, logfilepath)) {
            if (kAppenderSync == mode_) {
                __CloseLogFile();
            }
            fs::remove(PathFromUtf8(logcachefilepath));
        }
        return;
    }

    bool write_success = false;
    bool open_success = __OpenLogFile(config_.logdir_);
    if (open_success) {
        write_success = __WriteFile(_data, _len, logfile_.get());
        if (kAppenderAsync == mode_) {
            __CloseLogFile();
        }
    }

    if (!write_success) {
        if (open_success && kAppenderSync == mode_) {
            __CloseLogFile();
        }

        if (__OpenLogFile(config_.cachedir_)) {
            __WriteFile(_data, _len, logfile_.get());
            if (kAppenderAsync == mode_) {
                __CloseLogFile();
            }
        }
    }
}

void XloggerAppender::WriteTips2File(const char* _tips_format, ...) {
    if (nullptr == _tips_format) {
        return;
    }

    char tips_info[4096] = {0};
    va_list ap;
    va_start(ap, _tips_format);
    vsnprintf(tips_info, sizeof(tips_info), _tips_format, ap);
    va_end(ap);

    std::lock_guard sync_write(mutex_sync_write_);
    AutoBuffer tmp_buff;
    {
        std::lock_guard lock(mutex_buffer_async_);
        if (log_close_ || !log_buff_)
            return;
        log_buff_->Write(tips_info, strnlen(tips_info, sizeof(tips_info)), tmp_buff);
    }

    __Log2File(tmp_buff.Ptr(), tmp_buff.Length(), false);
}

void XloggerAppender::__AsyncLogThread() {
    while (true) {
        std::unique_lock lock_buffer(mutex_buffer_async_);
        cond_buffer_async_.wait_for(lock_buffer, std::chrono::minutes(15),
                                    [this] { return log_close_ || flush_requested_; });
        if (nullptr == log_buff_)
            break;

        flush_requested_ = false;
        const bool closing = log_close_;
        AutoBuffer tmp;
        log_buff_->Flush(tmp);
        lock_buffer.unlock();

        if (nullptr != tmp.Ptr())
            __Log2File(tmp.Ptr(), tmp.Length(), true);

        if (closing)
            break;
    }
}

void XloggerAppender::__WriteSync(const XLoggerInfo* _info, const char* _log) {
    char temp[16 * 1024] = {0};  // tell perry,ray if you want modify size.
    PtrBuffer log(temp, 0, sizeof(temp));
    log_formater(_info, _log, log);

    std::lock_guard sync_write(mutex_sync_write_);
    std::unique_lock lock(mutex_buffer_async_);
    if (log_close_ || nullptr == log_buff_)
        return;
    AutoBuffer tmp_buff;
    if (!log_buff_->Write(log.Ptr(), log.Length(), tmp_buff))
        return;
    lock.unlock();

    __Log2File(tmp_buff.Ptr(), tmp_buff.Length(), false);
}

void XloggerAppender::__WriteAsync(const XLoggerInfo* _info, const char* _log) {
    char temp[16 * 1024] = {0};  // tell perry,ray if you want modify size.
    PtrBuffer log_buff(temp, 0, sizeof(temp));
    log_formater(_info, _log, log_buff);

    std::unique_lock lock(mutex_buffer_async_);
    if (log_close_ || nullptr == log_buff_)
        return;

    if (log_buff_->GetData().Length() >= kBufferBlockLength * 4 / 5) {
        int ret = snprintf(temp,
                           sizeof(temp),
                           "[F][ sg_buffer_async.Length() >= BUFFER_BLOCK_LENTH*4/5, len: %d\n",
                           (int)log_buff_->GetData().Length());
        log_buff.Length(ret, ret);
    }

    if (!log_buff_->Write(log_buff.Ptr(), (unsigned int)log_buff.Length()))
        return;

    if (log_buff_->GetData().Length() >= kBufferBlockLength * 1 / 3
        || (nullptr != _info && kLevelFatal == _info->level)) {
        flush_requested_ = true;
        cond_buffer_async_.notify_one();
    }
}

#define HEX_STRING "0123456789abcdef"
static unsigned int to_string(const void* signature, int len, char* str) {
    char* str_p = str;
    const unsigned char* sig_p;

    for (sig_p = (const unsigned char*)signature; sig_p - (const unsigned char*)signature < len; sig_p++) {
        char high, low;
        high = *sig_p / 16;
        low = *sig_p % 16;

        *str_p++ = HEX_STRING[(unsigned char)high];
        *str_p++ = HEX_STRING[(unsigned char)low];
        *str_p++ = ' ';
    }

    *str_p++ = '\n';

    for (sig_p = (const unsigned char*)signature; sig_p - (const unsigned char*)signature < len; sig_p++) {
        *str_p++ = char(isgraph(*sig_p) ? *sig_p : ' ');
        *str_p++ = ' ';
        *str_p++ = ' ';
    }

    return (unsigned int)(str_p - str);
}

const char* XloggerAppender::Dump(const void* _dumpbuffer, size_t _len) {
    if (nullptr == _dumpbuffer || 0 == _len) {
        return "";
    }

    if (config_.logdir_.empty()) {
        return "";
    }

    SCOPE_ERRNO();

    thread_local std::string buffer;
    if (!buffer.empty()) {
        buffer.clear();
    }

    struct timeval tv = {0};
    gettimeofday(&tv, nullptr);
    time_t sec = tv.tv_sec;
    tm tcur = mars_local_time(sec);

    char forder_name[128] = {0};
    snprintf(forder_name, sizeof(forder_name), "%d%02d%02d", 1900 + tcur.tm_year, 1 + tcur.tm_mon, tcur.tm_mday);

    std::string filepath = config_.logdir_ + "/" + forder_name + "/";

    if (!fs::exists(PathFromUtf8(filepath)))
        fs::create_directory(PathFromUtf8(filepath));

    char file_name[128] = {0};
    snprintf(file_name,
             sizeof(file_name),
             "%d%02d%02d%02d%02d%02d_%d.dump",
             1900 + tcur.tm_year,
             1 + tcur.tm_mon,
             tcur.tm_mday,
             tcur.tm_hour,
             tcur.tm_min,
             tcur.tm_sec,
             (int)_len);
    filepath += file_name;

    File fileid(fopen(filepath.c_str(), "wb"));

    if (nullptr == fileid) {
        ASSERT2(nullptr != fileid, "%s, errno:(%d, %s)", filepath.c_str(), errno, strerror(errno));
        return "";
    }

    fwrite(_dumpbuffer, _len, 1, fileid.get());
    fileid.reset();

    buffer += "\n dump file to ";
    buffer += filepath + " :\n";

    int dump_len = 0;

    for (int x = 0; x < 32 && dump_len < (int)_len; ++x) {
        char dump_log[128] = {0};
        to_string((const char*)_dumpbuffer + dump_len, std::min(int(_len) - dump_len, 16), dump_log);
        dump_len += std::min((int)_len - dump_len, 16);
        buffer += dump_log;
        buffer += "\n";
    }

    return buffer.c_str();
}

static int calc_dump_required_length(int srcbytes) {
    // MUST CHANGE THIS IF YOU CHANGE `to_string` function.
    return srcbytes * 6 + 1;
}

bool XloggerAppender::GetCurrentLogPath(char* _log_path, unsigned int _len) {
    if (nullptr == _log_path || 0 == _len)
        return false;

    if (config_.logdir_.empty())
        return false;
    strncpy(_log_path, config_.logdir_.c_str(), _len - 1);
    _log_path[_len - 1] = '\0';
    return true;
}

bool XloggerAppender::GetCurrentLogCachePath(char* _logPath, unsigned int _len) {
    if (nullptr == _logPath || 0 == _len)
        return false;

    if (config_.cachedir_.empty())
        return false;
    strncpy(_logPath, config_.cachedir_.c_str(), _len - 1);
    _logPath[_len - 1] = '\0';
    return true;
}

void XloggerAppender::SetConsoleLog(bool _is_open) {
    consolelog_open_ = _is_open;
}

void XloggerAppender::SetMaxFileSize(uint64_t _max_byte_size) {
    max_file_size_ = _max_byte_size;
}

void XloggerAppender::SetMaxAliveDuration(long _max_time) {
    if (_max_time >= kMinLogAliveTime) {
        max_alive_time_ = _max_time;
    }
}

bool XloggerAppender::GetfilepathFromTimespan(int _timespan,
                                              const char* _prefix,
                                              std::vector<std::string>& _filepath_vec) {
    if (config_.logdir_.empty())
        return false;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    tv.tv_sec -= _timespan * (24 * 60 * 60);

    __GetFilePathsFromTimeval(tv, config_.logdir_, _prefix, LOG_EXT, _filepath_vec);
    if (!config_.cachedir_.empty()) {
        __GetFilePathsFromTimeval(tv, config_.cachedir_, _prefix, LOG_EXT, _filepath_vec);
    }
    return true;
}

bool XloggerAppender::MakeLogfileName(int _timespan, const char* _prefix, std::vector<std::string>& _filepath_vec) {
    if (config_.logdir_.empty())
        return false;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    tv.tv_sec -= _timespan * (24 * 60 * 60);

    char log_path[2048] = {0};
    __MakeLogFileName(tv, config_.logdir_, _prefix, LOG_EXT, log_path, sizeof(log_path));

    if (config_.cachedir_.empty()) {
        _filepath_vec.push_back(log_path);
        return true;
    }

    char cache_log_path[2048] = {0};
    __MakeLogFileName(tv, config_.cachedir_, _prefix, LOG_EXT, cache_log_path, sizeof(cache_log_path));

    if (fs::exists(PathFromUtf8(log_path))) {
        _filepath_vec.push_back(log_path);
    }

    if (fs::exists(PathFromUtf8(cache_log_path))) {
        _filepath_vec.push_back(cache_log_path);
    }

    if (!fs::exists(PathFromUtf8(log_path)) && !fs::exists(PathFromUtf8(cache_log_path))) {
        _filepath_vec.push_back(log_path);
    }

    return true;
}

void XloggerAppender::TreatMappingAsFileAndFlush(TFileIOAction* _result) {
    const auto mmap_file_path = MmapFilePath(config_);

    // exist
    if (!fs::exists(PathFromUtf8(mmap_file_path))) {
        if (_result)
            *_result = kActionUnnecessary;
        return;
    }

    // read buffer
    File treat_mapping_as_file(fopen(mmap_file_path.c_str(), "rb"));
    if (!treat_mapping_as_file) {
        if (_result)
            *_result = kActionOpenFailed;
        return;
    }

    auto data = std::make_unique<char[]>(kBufferBlockLength);
    size_t bytes_read = fread(data.get(), 1, kBufferBlockLength, treat_mapping_as_file.get());
    if (kBufferBlockLength != bytes_read || ferror(treat_mapping_as_file.get())) {
        if (_result)
            *_result = kActionReadFailed;
        return;
    }
    treat_mapping_as_file.reset();

    // init and set flag
    log_buff_ = MakeLogBuffer(data.get(), config_);
    heap_buffer_ = std::move(data);

    log_close_ = false;

    // try write mapping to logfile
    AutoBuffer buffer;
    log_buff_->Flush(buffer);

    // invalid data or empty
    if (!buffer.Ptr() || !buffer.Length()) {
        if (_result)
            *_result = kActionUnnecessary;
        return;
    }

    char mark_info[512] = {0};
    __GetMarkInfo(mark_info, sizeof(mark_info));

    WriteTips2File("~~~~~ begin of mmap from other process ~~~~~\n");
    __Log2File(buffer.Ptr(), buffer.Length(), false);
    WriteTips2File("~~~~~ end of mmap from other process ~~~~~%s\n", mark_info);

    // clean mmaping
    if (!fs::remove(PathFromUtf8(mmap_file_path))) {
        if (_result)
            *_result = kActionRemoveFailed;
        return;
    }

    if (_result)
        *_result = kActionSuccess;
}

////////////////////////////////////////////////////////////////////////////////////
static std::mutex sg_lifecycle_mutex;
static std::mutex sg_default_mutex;
static std::shared_ptr<XloggerAppender> sg_default_appender;
static bool sg_default_console_log_open = false;
static uint64_t sg_max_byte_size = 0;
static long sg_max_alive_time = 0;

static std::shared_ptr<XloggerAppender> GetDefaultAppender() {
    std::lock_guard lock(sg_default_mutex);
    return sg_default_appender;
}

void xlogger_appender(const XLoggerInfo* _info, const char* _log) {
    if (auto appender = GetDefaultAppender())
        appender->Write(_info, _log);
}

void appender_open(const XLogConfig& _config) {
    assert(!_config.logdir_.empty());
    std::lock_guard lifecycle_lock(sg_lifecycle_mutex);
    if (auto appender = GetDefaultAppender()) {
        appender->WriteTips2File("appender has already been opened. _dir:%s _nameprefix:%s",
                                _config.logdir_.c_str(), _config.nameprefix_.c_str());
        return;
    }

    uint64_t max_byte_size;
    {
        std::lock_guard lock(sg_default_mutex);
        max_byte_size = sg_max_byte_size;
    }
    auto appender = XloggerAppender::NewInstance(_config, max_byte_size);
    {
        std::lock_guard lock(sg_default_mutex);
        appender->SetConsoleLog(sg_default_console_log_open);
        appender->SetMaxFileSize(sg_max_byte_size);
        if (sg_max_alive_time > 0)
            appender->SetMaxAliveDuration(sg_max_alive_time);
        sg_default_appender = std::move(appender);
    }
    xlogger_SetAppender(&xlogger_appender);
    [[maybe_unused]] static const int close_registered = std::atexit(appender_close);
}

void appender_flush() {
    if (auto appender = GetDefaultAppender())
        appender->Flush();
}

void appender_flush_sync() {
    if (auto appender = GetDefaultAppender())
        appender->FlushSync();
}

void appender_close() {
    std::lock_guard lifecycle_lock(sg_lifecycle_mutex);
    std::shared_ptr<XloggerAppender> appender;
    {
        std::lock_guard lock(sg_default_mutex);
        appender = std::move(sg_default_appender);
    }
    if (appender)
        appender->Close();
}

void appender_setmode(TAppenderMode _mode) {
    if (auto appender = GetDefaultAppender())
        appender->SetMode(_mode);
}

bool appender_get_current_log_path(char* _log_path, unsigned int _len) {
    auto appender = GetDefaultAppender();
    return appender && appender->GetCurrentLogPath(_log_path, _len);
}

bool appender_get_current_log_cache_path(char* _logPath, unsigned int _len) {
    auto appender = GetDefaultAppender();
    return appender && appender->GetCurrentLogCachePath(_logPath, _len);
}

void appender_set_console_log(bool _is_open) {
    std::lock_guard lock(sg_default_mutex);
    sg_default_console_log_open = _is_open;
    if (sg_default_appender)
        sg_default_appender->SetConsoleLog(_is_open);
}

void appender_set_max_file_size(uint64_t _max_byte_size) {
    std::lock_guard lock(sg_default_mutex);
    sg_max_byte_size = _max_byte_size;
    if (sg_default_appender)
        sg_default_appender->SetMaxFileSize(_max_byte_size);
}

void appender_set_max_alive_duration(long _max_time) {
    std::lock_guard lock(sg_default_mutex);
    sg_max_alive_time = _max_time;
    if (sg_default_appender)
        sg_default_appender->SetMaxAliveDuration(_max_time);
}

bool appender_getfilepath_from_timespan(int _timespan, const char* _prefix, std::vector<std::string>& _filepath_vec) {
    auto appender = GetDefaultAppender();
    return appender && appender->GetfilepathFromTimespan(_timespan, _prefix, _filepath_vec);
}

bool appender_make_logfile_name(int _timespan, const char* _prefix, std::vector<std::string>& _filepath_vec) {
    auto appender = GetDefaultAppender();
    return appender && appender->MakeLogfileName(_timespan, _prefix, _filepath_vec);
}

void appender_oneshot_flush(const XLogConfig& _config, TFileIOAction* _result) {
    uint64_t max_byte_size;
    {
        std::lock_guard lock(sg_default_mutex);
        max_byte_size = sg_max_byte_size;
    }
    auto appender = XloggerAppender::NewOneShot(_config, max_byte_size);
    appender->TreatMappingAsFileAndFlush(_result);
    appender->Close();
}

}  // namespace xlog
}  // namespace mars

using namespace mars::xlog;

const char* xlogger_dump(const void* _dumpbuffer, size_t _len) {
    auto appender = GetDefaultAppender();
    return appender ? appender->Dump(_dumpbuffer, _len) : "";
}

const char* xlogger_memory_dump(const void* _dumpbuffer, size_t _len) {
    if (NULL == _dumpbuffer || 0 == _len) {
        //        ASSERT(NULL!=_dumpbuffer);
        //        ASSERT(0!=_len);
        return "";
    }

    SCOPE_ERRNO();

    thread_local std::string buffer;
    if (!buffer.empty()) {
        buffer.clear();
    }

    const char* src_buffer = reinterpret_cast<const char*>(_dumpbuffer);
    buffer += "\n";
    buffer += std::to_string(_len) + " bytes:\n";

    int calc_dst_buffer_len = calc_dump_required_length(32) + 1;
    auto dst_buffer = std::make_unique_for_overwrite<char[]>(calc_dst_buffer_len);

    for (int src_offset = 0; src_offset < (int)_len && buffer.size() < kMaxDumpLength;) {
        int dst_leftbytes = kMaxDumpLength - buffer.size();
        int bytes = std::min((int)_len - src_offset, 32);

        while (bytes > 0 && calc_dump_required_length(bytes) >= dst_leftbytes) {
            --bytes;
        }
        if (bytes <= 0) {
            break;
        }

        memset(dst_buffer.get(), 0, calc_dst_buffer_len);
        to_string(src_buffer + src_offset, bytes, dst_buffer.get());
        buffer += dst_buffer.get();

        src_offset += bytes;

        // next line
        buffer += "\n";
    }

    return buffer.c_str();
}
