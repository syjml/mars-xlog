#include <mars/xlog/appender.h>
#include <mars/xlog/xlogger.h>
#include <mars/xlog/xlogger_interface.h>
#include <zstd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "comm/delayed_task.h"

namespace {
using namespace mars::xlog;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

bool contains_compressed_marker(const std::string& data, std::string_view marker) {
    constexpr std::size_t header_size = 73;
    constexpr unsigned char async_zstd_magic = 0x0d;
    for (std::size_t offset = 0; data.size() - offset > header_size;) {
        std::size_t length = 0;
        for (unsigned i = 0; i < 4; ++i) {
            length |= static_cast<std::size_t>(static_cast<unsigned char>(data[offset + 5 + i])) << (8 * i);
        }
        if (length >= data.size() - offset - header_size)
            return false;
        if (static_cast<unsigned char>(data[offset]) == async_zstd_magic) {
            std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), ZSTD_freeDCtx);
            if (!context)
                return false;
            ZSTD_inBuffer input{data.data() + offset + header_size, length, 0};
            std::string decoded;
            while (input.pos < input.size) {
                char buffer[4096];
                ZSTD_outBuffer output{buffer, sizeof(buffer), 0};
                if (ZSTD_isError(ZSTD_decompressStream(context.get(), &output, &input)))
                    return false;
                decoded.append(buffer, output.pos);
            }
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

    for (const auto& logdir : {directory, cache}) {
        for (const auto& entry : fs::directory_iterator(logdir)) {
            const auto& path = entry.path();
            if (path.extension() != ".xlog" || !path.filename().string().starts_with(config.nameprefix_ + '_'))
                continue;
            std::ifstream log(path, std::ios::binary);
            const std::string data{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
            if (mode == kAppenderAsync ? contains_compressed_marker(data, marker) : data.find(marker) != std::string::npos)
                return true;
        }
    }
    return false;
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
    if (!check_delayed_tasks() || !fs::exists(stale_log) || !fs::exists(old_cache_log)) {
        std::cerr << "Unexpected result from delayed maintenance tasks\n";
        return 1;
    }
    fs::remove_all(directory);
    std::cout << "Verified sync and async zstd log readback, 16 open/close cycles, and delayed task cancellation\n";
    return 0;
}
