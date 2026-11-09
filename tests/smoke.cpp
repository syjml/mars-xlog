#include <mars/xlog/appender.h>
#include <mars/xlog/xlogger.h>
#include <mars/xlog/xlogger_interface.h>
#ifdef XLOG_ENABLE_ZSTD
#include <zstd.h>
#else
#include <zlib.h>
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <latch>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "comm/delayed_task.h"
#include "comm/mmap_util.h"
#ifdef XLOG_ENABLE_ZSTD
#include "xlog/log_zstd_buffer.h"
#else
#include "xlog/log_zlib_buffer.h"
#endif

extern "C" void xlog_smoke_write_from_c();

namespace {
using namespace mars::xlog;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
#ifdef XLOG_ENABLE_ZSTD
constexpr unsigned char async_magic = 0x0d;
constexpr const char* compression_name = "zstd";
#else
constexpr unsigned char async_magic = 0x09;
constexpr const char* compression_name = "zlib";
#endif

bool contains_compressed_marker(const std::string& data, std::string_view marker) {
    constexpr std::size_t header_size = 73;
    for (std::size_t offset = 0; data.size() - offset > header_size;) {
        std::size_t length = 0;
        for (unsigned i = 0; i < 4; ++i) {
            length |= static_cast<std::size_t>(static_cast<unsigned char>(data[offset + 5 + i])) << (8 * i);
        }
        if (length >= data.size() - offset - header_size)
            return false;
        if (static_cast<unsigned char>(data[offset]) == async_magic) {
            std::string decoded;
#ifdef XLOG_ENABLE_ZSTD
            std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
            if (!context)
                return false;
            ZSTD_inBuffer input{data.data() + offset + header_size, length, 0};
            while (input.pos < input.size) {
                char buffer[4096];
                ZSTD_outBuffer output{buffer, sizeof(buffer), 0};
                if (ZSTD_isError(ZSTD_decompressStream(context.get(), &output, &input)))
                    return false;
                decoded.append(buffer, output.pos);
            }
#else
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                return false;
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + offset + header_size));
            stream.avail_in = static_cast<uInt>(length);
            do {
                char buffer[4096];
                stream.next_out = reinterpret_cast<Bytef*>(buffer);
                stream.avail_out = sizeof(buffer);
                const int result = inflate(&stream, Z_NO_FLUSH);
                if (result != Z_OK && result != Z_STREAM_END)
                    break;
                decoded.append(buffer, sizeof(buffer) - stream.avail_out);
                if (result == Z_STREAM_END)
                    break;
            } while (stream.avail_in || stream.avail_out == 0);
            inflateEnd(&stream);
#endif
            if (decoded.find(marker) != std::string::npos)
                return true;
        }
        offset += header_size + length + 1;
    }
    return false;
}

std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

fs::path path_from_utf8(std::string_view bytes) {
    return fs::path(std::u8string(bytes.begin(), bytes.end()));
}

bool check_log(const fs::path& directory, const fs::path& cache, const std::string& prefix,
               std::string_view marker, bool compressed) {
    for (const auto& logdir : {directory, cache}) {
        for (const auto& entry : fs::directory_iterator(logdir)) {
            const auto& path = entry.path();
            if (path.extension() != ".xlog" || !utf8(path.filename()).starts_with(prefix + '_'))
                continue;
            std::ifstream log(path, std::ios::binary);
            const std::string data{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
            if (compressed ? contains_compressed_marker(data, marker) : data.find(marker) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool write_and_check(const fs::path& directory, const fs::path& cache, TAppenderMode mode, int cycle) {
    XLogConfig config;
    config.logdir_ = utf8(directory);
    config.cachedir_ = utf8(cache);
    config.nameprefix_ = mode == kAppenderAsync ? "async" : "sync";
    config.mode_ = cycle % 2 == 0 ? mode : kAppenderSync;
    config.compress_mode_ = kZstd;
    xlogger_SetLevel(kLevelInfo);
    appender_open(config);
    if (config.mode_ != mode)
        appender_setmode(mode);
    const std::string marker = "mars-xlog smoke cycle " + std::to_string(cycle) + " end";
    xinfo2(marker.c_str());
    if (cycle % 2 == 0)
        appender_flush();

    appender_close();
    return check_log(directory, cache, config.nameprefix_, marker, mode == kAppenderAsync);
}

bool check_mmap_recovery(const fs::path& directory, const fs::path& cache, bool oneshot) {
    constexpr unsigned int buffer_size = 150 * 1024;
    constexpr std::string_view marker = "xlog mmap recovery marker";
    const auto prefix = oneshot ? fs::path(u8"oneshot-\u65e5\u5fd7") : fs::path(u8"reopen-\u65e5\u5fd7");
    auto mmap_path = cache / prefix;
    mmap_path += ".mmap3";
    std::size_t used = 0;
    {
        mars::comm::MappedFile file;
        if (!file.Open(mmap_path, buffer_size))
            return false;
#ifdef XLOG_ENABLE_ZSTD
        LogZstdBuffer buffer(file.data(), buffer_size, true, "", 1);
#else
        LogZlibBuffer buffer(file.data(), buffer_size, true, "");
#endif
        if (!buffer.Write(marker.data(), marker.size()))
            return false;
        used = buffer.GetData().Length();
    }

    XLogConfig config;
    config.logdir_ = utf8(directory);
    config.cachedir_ = utf8(cache);
    config.nameprefix_ = utf8(prefix);
    config.mode_ = kAppenderSync;
    config.compress_mode_ = kZstd;
    if (oneshot) {
        TFileIOAction result = kActionUnnecessary;
        appender_oneshot_flush(config, &result);
        if (result != kActionSuccess || fs::exists(mmap_path))
            return false;
    } else {
        fs::resize_file(mmap_path, used);
        appender_open(config);
        appender_close();
        if (fs::file_size(mmap_path) != buffer_size)
            return false;
    }
    return check_log(directory, cache, config.nameprefix_, marker, true);
}

bool check_log_formatting(const fs::path& directory, const fs::path& cache) {
    XLOGGER_STATIC_ASSERT(sizeof(int) >= 2);
    const string_cast argument("first");
    char integer[66];
    if (XMessage()(TSF "%1 %0 %_ %%", argument, "second", "third").String() != "second first third %"
        || XMessage()(TSF "plain %%").String() != "plain %"
        || XMessage()(TSF "%_%_%_%_%_%_%_%_%_%_%_%_%_%_%_%_", 0, 1, 2, 3, 4, 5, 6, 7,
                      8, 9, 0, 1, 2, 3, 4, 5).String() != "0123456789012345"
        || std::string_view(string_cast_itoa((std::numeric_limits<int64_t>::min)(), integer)) != "-9223372036854775808"
        || std::string_view(string_cast_itoa(255, integer, 16)) != "FF"
        || std::string_view(string_cast_itoa(255, integer, 16, false)) != "ff"
        || std::string_view(string_cast_itoa(255, integer, 1)) != "itoa err") {
        return false;
    }

    XLogConfig config;
    config.logdir_ = utf8(directory);
    config.nameprefix_ = "format";
    config.mode_ = kAppenderSync;
    appender_open(config);
    xinfo2(TSF "typed log %_", 42);
    xlog_smoke_write_from_c();
    int suppressed = 0;
    xdebug2("disabled %d", ++suppressed);
    appender_close();
    return suppressed == 0
        && check_log(directory, cache, config.nameprefix_, "typed log 42", false)
        && check_log(directory, cache, config.nameprefix_, "C logging literal", false)
        && check_log(directory, cache, config.nameprefix_, "C formatted integer 42", false);
}

bool check_concurrent_reopen(const fs::path& directory, const fs::path& cache) {
    XLogConfig config;
    config.logdir_ = utf8(directory);
    config.mode_ = kAppenderAsync;
    config.compress_mode_ = kZstd;
    config.nameprefix_ = "concurrent";
    auto named = config;
    named.nameprefix_ = "concurrent-named";

    std::atomic<bool> stop{false};
    std::atomic<uintptr_t> handle{0};
    std::latch started(2);
    auto write_logs = [&] {
        started.count_down();
        while (!stop.load()) {
            xinfo2("default writer");
            if (const auto current = handle.load()) {
                XloggerWrite(current, nullptr, "named writer");
                Flush(current, true);
            }
            std::this_thread::sleep_for(1ms);
        }
    };
    std::thread first(write_logs);
    std::thread second(write_logs);
    started.wait();

    bool passed = true;
    for (int cycle = 0; cycle < 8; ++cycle) {
        config.mode_ = cycle % 2 == 0 ? kAppenderSync : kAppenderAsync;
        appender_open(config);
        const auto current = reinterpret_cast<uintptr_t>(NewXloggerInstance(named, kLevelInfo));
        if (!current) {
            appender_close();
            passed = false;
            break;
        }
        handle = current;
        passed = passed && reinterpret_cast<uintptr_t>(GetXloggerInstance(named.nameprefix_.c_str())) == current;
        xinfo2("default close marker %d", cycle);
        XloggerWrite(current, nullptr, "named close marker");
        std::this_thread::sleep_for(3ms);
        appender_close();
        ReleaseXloggerInstance(named.nameprefix_.c_str());
        passed = passed && !IsEnabledFor(current, kLevelInfo) && !GetXloggerInstance(named.nameprefix_.c_str());
    }
    stop = true;
    first.join();
    second.join();
    return passed
        && check_log(directory, cache, config.nameprefix_, "default close marker 7", true)
        && check_log(directory, cache, named.nameprefix_, "named close marker", true);
}

bool check_delayed_tasks() {
    bool ran = false;
    mars::comm::DelayedTask task(10ms, [&] { ran = true; });
    task.Join();
    if (!ran)
        return false;

    ran = false;
    {
        mars::comm::DelayedTask pending(2min, [&] { ran = true; });
        pending.RequestCancel();
    }
    return !ran;
}
}  // namespace

int main(int argc, char** argv) {
    const auto root = argc > 1 ? path_from_utf8(argv[1]) : fs::temp_directory_path();
    const auto run_id = std::chrono::steady_clock::now().time_since_epoch().count();
    auto directory = root / fs::path(u8"xlog-\u65e5\u5fd7-");
    directory += std::to_string(run_id);
    const auto cache = directory / "cache";
    fs::create_directories(cache);
    const auto stale_log = directory / "stale.xlog";
    const auto old_cache_log = cache / "async_20000101.xlog";
    for (const auto& path : {stale_log, old_cache_log}) {
        std::ofstream(path) << "old log";
        fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::days(30));
    }

    for (int cycle = 0; cycle < 8; ++cycle) {
        if (!write_and_check(directory, cache, kAppenderSync, cycle)
            || !write_and_check(directory, cache, kAppenderAsync, cycle)) {
            std::cerr << "Could not read log messages from cycle " << cycle << " in " << directory << '\n';
            return 1;
        }
    }
    const auto mmap_path = cache / "async.mmap3";
    if (!fs::remove(mmap_path) || !fs::create_directory(mmap_path)
        || !write_and_check(directory, cache, kAppenderAsync, 8)) {
        std::cerr << "Could not read logs written without a mapped buffer\n";
        return 1;
    }
    if (!check_delayed_tasks() || !fs::exists(stale_log) || !fs::exists(old_cache_log)) {
        std::cerr << "Unexpected result from delayed maintenance tasks\n";
        return 1;
    }
    if (!check_mmap_recovery(directory, cache, false) || !check_mmap_recovery(directory, cache, true)) {
        std::cerr << "Could not recover mapped logs in " << directory << '\n';
        return 1;
    }
    if (!check_log_formatting(directory, cache) || !check_concurrent_reopen(directory, cache)) {
        std::cerr << "Unexpected log formatting or concurrent close/reopen result\n";
        return 1;
    }
    fs::remove_all(directory);
    std::cout << "Verified sync and async " << compression_name
              << " log readback, mmap recovery, heap fallback, delayed task cancellation, "
                 "C/C++ formatting, and concurrent close/reopen\n";
    return 0;
}
