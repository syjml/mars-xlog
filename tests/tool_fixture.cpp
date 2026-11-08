#include <mars/xlog/appender.h>
#include <mars/xlog/xlogger.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

namespace {
using namespace mars::xlog;
namespace fs = std::filesystem;

bool WriteFixture(const char* public_key, TCompressMode compression, const char* prefix, int magic) {
    XLogConfig config;
    const auto directory = fs::current_path();
    const auto bytes = directory.u8string();
    config.logdir_.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    config.nameprefix_ = prefix;
    config.pub_key_ = public_key;
    config.mode_ = kAppenderAsync;
    config.compress_mode_ = compression;
    appender_open(config);
    xinfo2("encrypted begin");

    std::mt19937 generator(23);
    constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int line = 0; line < 40; ++line) {
        std::string message(2048, ' ');
        for (auto& c : message)
            c = alphabet[generator() % alphabet.size()];
        xinfo2("%s", message.c_str());
    }
    xinfo2("encrypted end");
    appender_flush_sync();
    appender_setmode(kAppenderSync);
    xinfo2("synchronous record");
    appender_close();

    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto& path = entry.path();
        if (path.extension() == ".xlog" && path.filename().string().starts_with(std::string(prefix) + '_')) {
            std::ifstream log(path, std::ios::binary);
            return log.get() == magic;
        }
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    xlogger_SetLevel(kLevelInfo);
    if (!WriteFixture(argv[1], kZlib, "zlib", 0x07)) {
        std::cerr << "could not create encrypted zlib fixture\n";
        return 1;
    }
#ifdef XLOG_ENABLE_ZSTD
    if (!WriteFixture(argv[1], kZstd, "zstd", 0x0c)) {
        std::cerr << "could not create encrypted zstd fixture\n";
        return 1;
    }
#endif
}
