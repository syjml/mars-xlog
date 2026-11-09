cmake_minimum_required(VERSION 3.21)

option(XLOG_ENABLE_ZSTD "Enable the zstd log compression backend" OFF)
option(XLOG_BUILD_TOOL "Build the xlog-tool command-line utility" OFF)
# vcpkg reads manifest features during project(). A parent project owns its manifest.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    list(REMOVE_ITEM VCPKG_MANIFEST_FEATURES zstd)
    if(XLOG_ENABLE_ZSTD OR XLOG_BUILD_TOOL)
        list(APPEND VCPKG_MANIFEST_FEATURES zstd)
    endif()
endif()

project(mars_xlog LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(PROJECT_IS_TOP_LEVEL)
    option(CMAKE_COMPILE_WARNING_AS_ERROR "Treat mars-xlog compiler warnings as errors" ON)
elseif(NOT DEFINED CMAKE_COMPILE_WARNING_AS_ERROR)
    set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux|Darwin|Android|iOS)$")
    message(FATAL_ERROR "Unsupported target platform: ${CMAKE_SYSTEM_NAME}")
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

# CMake 3.24 and newer select the warning-as-error flag for each compiler.
if(CMAKE_COMPILE_WARNING_AS_ERROR AND CMAKE_VERSION VERSION_LESS 3.24)
    foreach(lang IN ITEMS C CXX OBJCXX)
        if(CMAKE_${lang}_COMPILER_ID STREQUAL "MSVC" OR CMAKE_${lang}_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            add_compile_options("$<$<COMPILE_LANGUAGE:${lang}>:/WX>")
        elseif(CMAKE_${lang}_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
            add_compile_options("$<$<COMPILE_LANGUAGE:${lang}>:-Werror>")
        endif()
    endforeach()
endif()

include(GNUInstallDirs)
if(XLOG_ENABLE_ZSTD OR XLOG_BUILD_TOOL)
    find_package(zstd CONFIG REQUIRED)
endif()

set(xlog_sources
    src/comm/assert/__assert.c
    src/comm/autobuffer.cc
    src/comm/mmap_util.cc
    src/comm/ptrbuffer.cc
    src/comm/strutil.cc
    src/comm/time_utils.c
    src/xlog/appender.cc
    src/xlog/crypt/log_crypt.cc
    src/xlog/crypt/micro-ecc-master/uECC.c
    src/xlog/formater.cc
    src/xlog/log_base_buffer.cc
    src/xlog/log_zlib_buffer.cc
    src/xlog/xlogger/loginfo_extract.c
    src/xlog/xlogger/xlogger.cc
    src/xlog/xlogger/xlogger_category.cc
    src/xlog/xlogger/xloggerbase.cc
    src/xlog/xlogger_interface.cc
)

string(TIMESTAMP XLOG_BUILD_TIME "%Y-%m-%d %H:%M:%S UTC" UTC)
configure_file(cmake/verinfo.h.in generated/comm/verinfo.h @ONLY)

add_library(xlog STATIC ${xlog_sources})
add_library(xlog::xlog ALIAS xlog)
target_include_directories(xlog
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        src
        src/comm
)
if(XLOG_ENABLE_ZSTD)
    target_sources(xlog PRIVATE src/xlog/log_zstd_buffer.cc)
    target_compile_definitions(xlog PRIVATE XLOG_ENABLE_ZSTD=1)
    target_link_libraries(xlog PRIVATE zstd::libzstd)
endif()

if(WIN32)
    file(GLOB zlib_sources CONFIGURE_DEPENDS src/comm/windows/zlib/*.c)
    target_sources(xlog PRIVATE ${zlib_sources}
        src/comm/adler32.c
        src/comm/windows/projdef.cc
        src/comm/windows/xlogger_threadinfo.cpp
        src/xlog/console_log_win.cc
    )
    target_compile_definitions(xlog PRIVATE WIN32 UNICODE _UNICODE NOMINMAX _LIB Z_PREFIX)
    foreach(lang IN ITEMS C CXX)
        if(CMAKE_${lang}_COMPILER_ID STREQUAL "MSVC" OR CMAKE_${lang}_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            target_compile_options(xlog PRIVATE "$<$<COMPILE_LANGUAGE:${lang}>:/utf-8;/FI${CMAKE_CURRENT_SOURCE_DIR}/src/comm/projdef.h>")
        else()
            target_compile_options(xlog PRIVATE "$<$<COMPILE_LANGUAGE:${lang}>:-include;${CMAKE_CURRENT_SOURCE_DIR}/src/comm/projdef.h>")
        endif()
    endforeach()
    target_include_directories(xlog PRIVATE src/comm/windows/zlib)
    if(NOT MINGW)
        target_sources(xlog PRIVATE src/comm/windows/sys/time.c src/comm/windows/unistd.cpp)
        target_include_directories(xlog PRIVATE src/comm/windows include/mars/comm/windows)
    endif()
    target_link_libraries(xlog PRIVATE advapi32 crypt32)
else()
    find_package(ZLIB REQUIRED)
    set_target_properties(xlog PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(xlog PRIVATE src/comm/unix/xlogger_threadinfo.cc)
    target_link_libraries(xlog PRIVATE ZLIB::ZLIB)
    if(APPLE)
        target_sources(xlog PRIVATE src/xlog/console_log_apple.mm src/comm/apple/data_protection.mm)
        set_target_properties(xlog PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
        target_link_libraries(xlog PRIVATE "-framework Foundation")
    else()
        target_sources(xlog PRIVATE src/xlog/console_log_unix.cc)
        if(ANDROID)
            target_link_libraries(xlog PRIVATE log)
        endif()
    endif()
endif()

if(XLOG_BUILD_TOOL)
    add_executable(xlog_tool tools/xlog_tool.cpp)
    set_target_properties(xlog_tool PROPERTIES OUTPUT_NAME xlog-tool)
    target_include_directories(xlog_tool PRIVATE src)
    target_link_libraries(xlog_tool PRIVATE xlog::xlog zstd::libzstd)
    if(WIN32)
        target_compile_definitions(xlog_tool PRIVATE NOMINMAX Z_PREFIX)
        target_include_directories(xlog_tool PRIVATE src/comm/windows/zlib)
        target_link_libraries(xlog_tool PRIVATE advapi32 shell32)
    else()
        target_link_libraries(xlog_tool PRIVATE ZLIB::ZLIB)
    endif()
    install(TARGETS xlog_tool RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(TARGETS xlog EXPORT xlogTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h")
install(FILES LICENSE LICENSE.zlib "Copyright Notice of Boost" DESTINATION "${CMAKE_INSTALL_DATADIR}/xlog")
install(FILES src/xlog/crypt/micro-ecc-master/LICENSE.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/xlog" RENAME micro-ecc-license.txt)

set(xlog_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/xlog")
install(EXPORT xlogTargets NAMESPACE xlog:: DESTINATION "${xlog_package_dir}")
configure_file(cmake/xlogConfig.cmake.in xlogConfig.cmake @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/xlogConfig.cmake" DESTINATION "${xlog_package_dir}")

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(xlog_smoke tests/smoke.cpp tests/smoke_c.c)
        target_link_libraries(xlog_smoke PRIVATE xlog::xlog)
        target_include_directories(xlog_smoke PRIVATE src)
        if(XLOG_ENABLE_ZSTD)
            target_compile_definitions(xlog_smoke PRIVATE XLOG_ENABLE_ZSTD=1)
            target_link_libraries(xlog_smoke PRIVATE zstd::libzstd)
        elseif(WIN32)
            target_compile_definitions(xlog_smoke PRIVATE Z_PREFIX)
            target_include_directories(xlog_smoke PRIVATE src/comm/windows/zlib)
        else()
            target_link_libraries(xlog_smoke PRIVATE ZLIB::ZLIB)
        endif()
        if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
            add_test(NAME xlog.smoke COMMAND xlog_smoke)
            set_tests_properties(xlog.smoke PROPERTIES TIMEOUT 30)
        endif()
        if(XLOG_BUILD_TOOL AND NOT CMAKE_CROSSCOMPILING)
            add_executable(xlog_tool_fixture tests/tool_fixture.cpp)
            target_link_libraries(xlog_tool_fixture PRIVATE xlog::xlog)
            if(XLOG_ENABLE_ZSTD)
                target_compile_definitions(xlog_tool_fixture PRIVATE XLOG_ENABLE_ZSTD=1)
            endif()
            add_test(NAME xlog.tool COMMAND "${CMAKE_COMMAND}"
                "-DTOOL=$<TARGET_FILE:xlog_tool>"
                "-DFIXTURE=$<TARGET_FILE:xlog_tool_fixture>"
                "-DWORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
                "-DENCODER_HAS_ZSTD=${XLOG_ENABLE_ZSTD}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/tool_roundtrip.cmake")
            set_tests_properties(xlog.tool PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()
