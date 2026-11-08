# Mars xlog

从 Tencent Mars 提取的独立 C++ 静态库，源自 [Mars 6aa5b56](https://github.com/Tencent/mars/tree/6aa5b567afada93936252bac2f271c4b43eccd1f) 和本地 Windows 兼容修复 `1406c4a`。保留 appender、日志宏、独立 logger 实例、同步与异步写入、zlib 与 zstd 压缩以及 xlog 加密。

项目使用 vcpkg manifest 管理 Boost.Filesystem、Boost.Iostreams、Boost.Nowide 和 zstd。Windows 线程等待使用 C++ 标准库，zlib 和 micro-ecc 随源码提供。

公开头文件位于 `include/mars`，日志实现位于 `src/xlog`，内部工具和平台适配位于 `src/comm`。

## Windows 构建

安装 VS2022 的「使用 C++ 的桌面开发」工作负载和 CMake 3.21 或更新版本，设置 `VCPKG_ROOT` 指向已完成 bootstrap 的 vcpkg 目录。首次配置会安装所需依赖。

```powershell
cmake --preset msvc-x64
cmake --build build/msvc-x64 --config Release
ctest --test-dir build/msvc-x64 -C Release --output-on-failure
```

使用 Clang 时，打开 x64 Native Tools Command Prompt，在该窗口内设置 `VCPKG_ROOT`，并把 `clang-cl` 和 Ninja 加入 `PATH`。VS 开发者命令行可能会把 `VCPKG_ROOT` 设为 VS 自带的 vcpkg 路径。

```bat
set "VCPKG_ROOT=C:\dev\vcpkg"
cmake --preset clang-x64
cmake --build build/clang-x64 --config Release
ctest --test-dir build/clang-x64 -C Release --output-on-failure
```

Win32 对应 `msvc-x86` 和 `clang-x86`，Clang 需要在 x86 Native Tools Command Prompt 中配置。调试版使用 `--config Debug`。这些预设使用 MSVC ABI 和静态 CRT，Release 对应 `/MT`，Debug 对应 `/MTd`。clang-cl 与 MSVC 按架构共享 vcpkg 安装目录。

## 接入

在使用同一 vcpkg 工具链的 CMake 项目里添加本项目源码，并在应用的 vcpkg manifest 中声明 [相同依赖](vcpkg.json)。

```cmake
add_subdirectory(path/to/mars-xlog)
target_link_libraries(my_app PRIVATE xlog::xlog)
```

也可以先安装库、头文件和 CMake 配置，再将安装目录加入应用的 `CMAKE_PREFIX_PATH`。

```powershell
cmake --install build/msvc-x64 --config Release --prefix build/sdk/msvc-x64
```

```cmake
find_package(xlog CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE xlog::xlog)
```

安装后的 `xlog::xlog` 目标会链接 Boost 和 zstd。应用的 vcpkg triplet、架构和 CRT 设置须与库一致。

```cpp
#include <mars/xlog/appender.h>
#include <mars/xlog/xlogger.h>

int main() {
    using namespace mars::xlog;
    XLogConfig config;
    config.logdir_ = "xlog-files";
    config.nameprefix_ = "demo";
    config.compress_mode_ = kZstd;

    xlogger_SetLevel(kLevelInfo);
    appender_open(config);
    xinfo2("xlog started");
    appender_close();
}
```

为 xlog 使用专门的日志目录，库会自动清理过期日志。
