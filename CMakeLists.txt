cmake_minimum_required(VERSION 3.21)
project(mars_xlog LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux|Darwin|Android|iOS)$")
    message(FATAL_ERROR "Unsupported target platform: ${CMAKE_SYSTEM_NAME}")
endif()

include(GNUInstallDirs)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem iostreams nowide)
find_package(zstd CONFIG REQUIRED)

set(xlog_sources
    src/comm/assert/__assert.c
    src/comm/autobuffer.cc
    src/comm/mmap_util.cc
    src/comm/ptrbuffer.cc
    src/comm/strutil.cc
    src/comm/tickcount.cc
    src/comm/time_utils.c
    src/xlog/appender.cc
    src/xlog/crypt/log_crypt.cc
    src/xlog/crypt/micro-ecc-master/uECC.c
    src/xlog/formater.cc
    src/xlog/log_base_buffer.cc
    src/xlog/log_zlib_buffer.cc
    src/xlog/log_zstd_buffer.cc
    src/xlog/xlogger/loginfo_extract.c
    src/xlog/xlogger/xlogger.cc
    src/xlog/xlogger/xlogger_category.cc
    src/xlog/xlogger/xloggerbase.c
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
target_link_libraries(xlog PRIVATE
    Boost::filesystem Boost::iostreams Boost::nowide zstd::libzstd
)

if(WIN32)
    file(GLOB zlib_sources CONFIGURE_DEPENDS src/comm/windows/zlib/*.c)
    target_sources(xlog PRIVATE ${zlib_sources}
        src/comm/adler32.c
        src/comm/windows/projdef.cc
        src/comm/windows/xlogger_threadinfo.cpp
        src/xlog/console_log_win.cc
    )
    target_compile_definitions(xlog PRIVATE WIN32 UNICODE _UNICODE NOMINMAX _LIB BOOST_ALL_NO_LIB Z_PREFIX)
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
        enable_language(OBJCXX)
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

install(TARGETS xlog EXPORT xlogTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h")
install(FILES LICENSE LICENSE.zlib "Copyright Notice of Boost" DESTINATION "${CMAKE_INSTALL_DATADIR}/xlog")
install(FILES src/xlog/crypt/micro-ecc-master/LICENSE.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/xlog" RENAME micro-ecc-license.txt)

set(xlog_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/xlog")
install(EXPORT xlogTargets NAMESPACE xlog:: DESTINATION "${xlog_package_dir}")
install(FILES cmake/xlogConfig.cmake DESTINATION "${xlog_package_dir}")

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(xlog_smoke tests/smoke.cpp)
        target_link_libraries(xlog_smoke PRIVATE xlog::xlog zstd::libzstd)
        target_include_directories(xlog_smoke PRIVATE src)
        if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
            add_test(NAME xlog.smoke COMMAND xlog_smoke)
            set_tests_properties(xlog.smoke PROPERTIES TIMEOUT 30)
        endif()
    endif()
endif()
