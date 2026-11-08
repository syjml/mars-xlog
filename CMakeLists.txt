cmake_minimum_required(VERSION 3.21)
project(mars_xlog LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "mars-xlog requires Windows with MSVC or clang-cl")
endif()

include(GNUInstallDirs)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem iostreams nowide)
find_package(zstd CONFIG REQUIRED)

set(xlog_sources
    src/comm/adler32.c
    src/comm/assert/__assert.c
    src/comm/autobuffer.cc
    src/comm/mmap_util.cc
    src/comm/ptrbuffer.cc
    src/comm/strutil.cc
    src/comm/tickcount.cc
    src/comm/time_utils.c
    src/comm/windows/projdef.cc
    src/comm/windows/sys/time.c
    src/comm/windows/unistd.cpp
    src/comm/windows/xlogger_threadinfo.cpp
    src/xlog/appender.cc
    src/xlog/console_log_win.cc
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
file(GLOB zlib_sources CONFIGURE_DEPENDS src/comm/windows/zlib/*.c)

string(TIMESTAMP XLOG_BUILD_TIME "%Y-%m-%d %H:%M:%S UTC" UTC)
configure_file(cmake/verinfo.h.in generated/comm/verinfo.h @ONLY)

add_library(xlog STATIC ${xlog_sources} ${zlib_sources})
add_library(xlog::xlog ALIAS xlog)
target_compile_features(xlog PUBLIC cxx_std_17)
target_compile_definitions(xlog PRIVATE WIN32 UNICODE _UNICODE NOMINMAX _LIB BOOST_ALL_NO_LIB Z_PREFIX)
target_compile_options(xlog PRIVATE /utf-8 "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/comm/projdef.h")
target_include_directories(xlog
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        src
        src/comm
        src/comm/windows
        src/comm/windows/zlib
        include/mars/comm/windows
)
target_link_libraries(xlog PRIVATE
    Boost::filesystem Boost::iostreams Boost::nowide zstd::libzstd
    advapi32 crypt32
)

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
        target_link_libraries(xlog_smoke PRIVATE xlog::xlog)
        add_test(NAME xlog.smoke COMMAND xlog_smoke)
    endif()
endif()
