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

namespace {
using namespace mars::xlog;
namespace fs = std::filesystem;
constexpr char marker[] = "mars-xlog smoke";

bool contains_compressed_marker(const std::string& data) {
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

bool write_and_check(const fs::path& directory, TAppenderMode mode) {
    XLogConfig config;
    config.logdir_ = directory.u8string();
    config.nameprefix_ = mode == kAppenderAsync ? "async" : "sync";
    config.mode_ = mode;
    config.compress_mode_ = kZstd;
    xlogger_SetLevel(kLevelInfo);
    appender_open(config);
    xinfo2(marker);

    std::vector<std::string> paths;
    const bool found = appender_make_logfile_name(0, config.nameprefix_.c_str(), paths);
    appender_close();
    if (!found || paths.empty())
        return false;

    std::ifstream log(fs::u8path(paths.front()), std::ios::binary);
    const std::string data{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    return mode == kAppenderAsync ? contains_compressed_marker(data) : data.find(marker) != std::string::npos;
}
}  // namespace

int main(int argc, char** argv) {
    const auto root = argc > 1 ? fs::u8path(argv[1]) : fs::temp_directory_path();
    const auto run_id = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = root / fs::u8path(u8"xlog-\u65e5\u5fd7-" + std::to_string(run_id));
    if (!write_and_check(directory, kAppenderSync) || !write_and_check(directory, kAppenderAsync)) {
        std::cerr << "Could not read the expected log messages from " << directory << '\n';
        return 1;
    }
    fs::remove_all(directory);
    std::cout << "Verified sync and async zstd log messages\n";
    return 0;
}
