# Mars xlog

从 Tencent Mars 提取的独立 C++ 静态库，源自 [Mars 6aa5b56](https://github.com/Tencent/mars/tree/6aa5b567afada93936252bac2f271c4b43eccd1f) 和本地 Windows 兼容修复 `1406c4a`。保留 appender、日志宏、独立 logger 实例、同步与异步写入、zlib 与 zstd 压缩以及 xlog 加密。

项目使用 vcpkg manifest 管理可选的 zstd。Linux、macOS、Android 和 iOS 的 zlib 也由 vcpkg 管理。Windows 使用随源码提供的 zlib，各平台的日志加密使用随源码提供的 micro-ecc。文件操作使用 `std::filesystem`，`.mmap3` 恢复缓冲区使用系统的文件映射 API。

公开头文件位于 `include/mars`，日志实现位于 `src/xlog`，内部工具和平台适配位于 `src/comm`。

## 压缩选项

`XLOG_ENABLE_ZSTD` 默认关闭。默认的独立构建会跳过 vcpkg 的 zstd 安装，新日志使用 zlib。已有代码设置 `compress_mode_ = kZstd` 时也会回退到 zlib。需要写 zstd 日志时，在构建预设后加 `-DXLOG_ENABLE_ZSTD=ON`，再将 `compress_mode_` 设为 `kZstd`。安装后的 `xlog::xlog` 自动按构建选项解析依赖。

## 密钥和离线解码

Windows、Linux 和 macOS 可用 `-DXLOG_BUILD_TOOL=ON` 构建 `xlog-tool`。工具默认关闭，启用时 vcpkg 会为解码器安装 zstd。工具始终支持 zlib 和 zstd 解码，日志库是否写 zstd 仍由 `XLOG_ENABLE_ZSTD` 控制。CMake 安装会将工具放入 `bin`。

```sh
cmake --preset linux-x64 -DXLOG_BUILD_TOOL=ON
cmake --build build/linux-x64 --target xlog_tool
build/linux-x64/xlog-tool keygen xlog-private.hex
build/linux-x64/xlog-tool decode xlog-private.hex app.xlog app.log
```

Windows 使用多配置预设时，构建命令加 `--config Release`，可执行文件位于 `build/<preset>/Release/xlog-tool.exe`。

`keygen` 把 secp256k1 私钥写入新文件，格式为 64 个十六进制字符。工具在 POSIX 上使用 `0600`，在 Windows 上为新文件设置仅向所有者授予访问权的 ACL。终端输出 128 个十六进制字符的公钥，可直接填入 `XLogConfig::pub_key_`。客户端选择 `kAppenderAsync` 才会加密正文，私钥留在离线解码端。

`decode` 从私钥文件读取密钥，逐条解码当前格式的 `.xlog`，并输出文本日志。它也能处理同一文件中的同步和未加密记录。两个命令都要求输出路径尚不存在。当前 xlog 加密格式没有认证标签，解码结果无法用于验证日志的真实性。

## Windows 构建

构建 xlog 需要 CMake 3.21 或更新版本，以及支持 C++20 的编译器。设置 `VCPKG_ROOT` 指向已完成 bootstrap 的 vcpkg 目录，首次配置会安装依赖。使用 MSVC 时，安装 VS2022 的「使用 C++ 的桌面开发」工作负载。

默认将编译警告视为错误。需要关闭时，在配置阶段传入 `-DCMAKE_COMPILE_WARNING_AS_ERROR=OFF`。

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

Win32 对应 `msvc-x86` 和 `clang-x86`，Clang 需要在 x86 Native Tools Command Prompt 中配置。在 x64 开发者命令行中可使用 `clang-gnu-x64` 预设，通过 `clang` 和 `clang++` 的 GNU 风格命令行生成 MSVC ABI 的库。上述预设使用静态 CRT，Release 对应 `/MT`，Debug 对应 `/MTd`，按架构共享 vcpkg 安装目录。

使用 MinGW-w64 时，把包含 `x86_64-w64-mingw32-gcc` 和 `x86_64-w64-mingw32-g++` 的目录及 Ninja 加入 `PATH`，并设置 `VCPKG_ROOT`。例如可使用 MSYS2 UCRT64 的 GCC。`mingw-x64` 预设使用 vcpkg 的 `x64-mingw-static` triplet，应用应使用相同的工具链和运行库。

```powershell
cmake --preset mingw-x64
cmake --build build/mingw-x64 --config Release
ctest --test-dir build/mingw-x64 -C Release --output-on-failure
```

调试版使用 `--config Debug`，运行 CTest 时使用 `-C Debug`。自定义工具链可直接通过 CMake 配置，vcpkg triplet 应与目标架构、ABI 和运行库匹配。

## Linux 和 macOS 构建

安装 CMake、Ninja 和支持 C++20 的编译器，设置 `VCPKG_ROOT` 指向已完成 bootstrap 的 vcpkg 目录。macOS 使用 Xcode 工具链。预设默认构建 Release，可以用 `-DCMAKE_BUILD_TYPE=Debug` 切换。

```sh
cmake --preset linux-x64
cmake --build build/linux-x64
ctest --test-dir build/linux-x64 --output-on-failure
```

在 Apple Silicon Mac 上使用 `macos-arm64` 预设，最低部署版本为 macOS 11。

```sh
cmake --preset macos-arm64
cmake --build build/macos-arm64
ctest --test-dir build/macos-arm64 --output-on-failure
```

## Android 构建

设置 `ANDROID_NDK_HOME` 指向 Android NDK。预设使用 `arm64-v8a`、API 28 和 `c++_shared`，应用应使用匹配的 ABI 和 C++ 运行库。

```sh
cmake --preset android-arm64
cmake --build build/android-arm64
```

将 `build/android-arm64/xlog_smoke` 和 NDK 对应 ABI 的 `libc++_shared.so` 推送到设备上的测试目录。例如，在 `/data/local/tmp/mars-xlog` 中执行：

```sh
adb shell 'cd /data/local/tmp/mars-xlog && chmod +x xlog_smoke && LD_LIBRARY_PATH=. ./xlog_smoke .'
```

## iOS 构建

在 Apple Silicon Mac 上用 Xcode 工具链构建，最低部署版本为 iOS 14。真机和模拟器使用不同的 vcpkg triplet 和输出目录。Apple 预设通过 `cmake/triplets` 给 xlog 和 vcpkg 依赖设定相同的最低部署版本。

```sh
cmake --preset ios-arm64
cmake --build build/ios-arm64
cmake --preset ios-simulator-arm64
cmake --build build/ios-simulator-arm64
xcrun simctl spawn booted "$PWD/build/ios-simulator-arm64/xlog_smoke.app/xlog_smoke" /tmp
```

最后一条命令需要已启动的 iOS 模拟器。真机应用通过自己的签名和部署流程运行。

## CI

[GitHub Actions](.github/workflows/ci.yml) 在 `main` 分支推送、Pull Request 和手动触发时运行。CI 使用 `vcpkg.json` 的基线检出 vcpkg，并按 runner 镜像和构建预设缓存 vcpkg 二进制包。

| 平台 | 构建 | 日志测试 |
| --- | --- | --- |
| Windows | VS2022 和 clang-cl（x64、x86），GNU 风格 Clang 和 MinGW GCC（x64），Release 和 Debug | CTest |
| Linux | x64 Release | CTest |
| macOS | ARM64 Release | CTest |
| Android | ARM64 和 x86_64 Release | x86_64 Android 模拟器 |
| iOS | ARM64 真机和模拟器 Release | ARM64 iOS 模拟器 |

五个平台的 CI 都覆盖默认的 zlib 构建和显式启用 zstd 的构建。日志测试覆盖 Unicode 路径下的同步日志和异步压缩日志回读、`.mmap3` 恢复、反复打开关闭，以及关闭时取消待执行的维护任务。Android ARM64 和 iOS 真机任务验证交叉构建。

## 接入

在使用同一 vcpkg 工具链的 CMake 项目里添加本项目源码，并在应用自己的 vcpkg manifest 中声明所需依赖。非 Windows 目标需要 `zlib`。启用 `XLOG_ENABLE_ZSTD` 或 `XLOG_BUILD_TOOL` 时还需要 `zstd`。

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

安装后的 `xlog::xlog` 目标会链接 vcpkg 依赖。应用的 vcpkg triplet、架构和 CRT 设置须与库匹配。交叉编译时，可将 `xlog_DIR` 显式设为安装前缀下的 `lib/cmake/xlog`。

```cpp
#include <mars/xlog/appender.h>
#include <mars/xlog/xlogger.h>

int main() {
    using namespace mars::xlog;
    XLogConfig config;
    config.logdir_ = "xlog-files";
    config.nameprefix_ = "demo";
    config.compress_mode_ = kZlib;

    xlogger_SetLevel(kLevelInfo);
    appender_open(config);
    xinfo2("xlog started");
    appender_close();
}
```

为 xlog 使用专门的日志目录，库会自动清理过期日志。
