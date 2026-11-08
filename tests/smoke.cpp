#include <mars/xlog/appender.h>
#include <mars/xlog/xlogger.h>
#include <mars/xlog/xlogger_interface.h>

#include <chrono>
#include <filesystem>

int main() {
    using namespace mars::xlog;
    namespace fs = std::filesystem;

    const auto run_id = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = fs::temp_directory_path() / fs::u8path(u8"xlog-\u65e5\u5fd7-" + std::to_string(run_id));
    XLogConfig config;
    config.logdir_ = directory.u8string();
    config.nameprefix_ = "smoke";
    config.compress_mode_ = kZstd;
    xlogger_SetLevel(kLevelInfo);
    appender_open(config);
    xinfo2("mars-xlog smoke");
    FlushAll(true);

    std::vector<std::string> paths;
    const bool found = appender_make_logfile_name(0, config.nameprefix_.c_str(), paths);
    appender_close();
    if (!found || paths.empty()) {
        return 1;
    }
    const auto log = directory / fs::u8path(paths.front()).filename();
    if (!fs::exists(log) || fs::file_size(log) == 0) {
        return 1;
    }
    fs::remove_all(directory);
    return 0;
}
