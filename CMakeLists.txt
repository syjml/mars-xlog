cmake_minimum_required(VERSION 3.21)
project(mars_xlog LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "mars-xlog requires Windows with MSVC or clang-cl")
endif()

include(GNUInstallDirs)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem iostreams nowide)
find_package(zstd CONFIG REQUIRED)

set(xlog_sources
    mars/comm/adler32.c
    mars/comm/assert/__assert.c
    mars/comm/autobuffer.cc
    mars/comm/mmap_util.cc
    mars/comm/ptrbuffer.cc
    mars/comm/strutil.cc
    mars/comm/tickcount.cc
    mars/comm/time_utils.c
    mars/comm/windows/projdef.cc
    mars/comm/windows/sys/time.c
    mars/comm/windows/unistd.cpp
    mars/comm/windows/xlogger_threadinfo.cpp
    mars/comm/xlogger/loginfo_extract.c
    mars/comm/xlogger/xlogger.cc
    mars/comm/xlogger/xlogger_category.cc
    mars/comm/xlogger/xloggerbase.c
    mars/xlog/crypt/log_crypt.cc
    mars/xlog/crypt/micro-ecc-master/uECC.c
    mars/xlog/src/appender.cc
    mars/xlog/src/console_log_win.cc
    mars/xlog/src/formater.cc
    mars/xlog/src/log_base_buffer.cc
    mars/xlog/src/log_zlib_buffer.cc
    mars/xlog/src/log_zstd_buffer.cc
    mars/xlog/src/xlogger_interface.cc
)
file(GLOB zlib_sources CONFIGURE_DEPENDS mars/comm/windows/zlib/*.c)

string(TIMESTAMP XLOG_BUILD_TIME "%Y-%m-%d %H:%M:%S UTC" UTC)
configure_file(cmake/verinfo.h.in generated/mars/comm/verinfo.h @ONLY)

add_library(xlog STATIC ${xlog_sources} ${zlib_sources})
add_library(xlog::xlog ALIAS xlog)
target_compile_features(xlog PUBLIC cxx_std_17)
target_compile_definitions(xlog PRIVATE WIN32 UNICODE _UNICODE NOMINMAX _LIB BOOST_ALL_NO_LIB Z_PREFIX)
target_compile_options(xlog PRIVATE /utf-8 "/FI${CMAKE_CURRENT_SOURCE_DIR}/mars/comm/projdef.h")
target_include_directories(xlog
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
        mars
        mars/comm
        mars/comm/windows
        mars/comm/windows/zlib
)
target_link_libraries(xlog PRIVATE
    Boost::filesystem Boost::iostreams Boost::nowide zstd::libzstd
    advapi32 crypt32
)

install(TARGETS xlog EXPORT xlogTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")
install(FILES
    mars/xlog/appender.h
    mars/xlog/preprocessor.h
    mars/xlog/xlogger.h
    mars/xlog/xlogger_interface.h
    mars/xlog/xloggerbase.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mars/xlog"
)
install(FILES mars/comm/string_cast.h mars/comm/strutil.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mars/comm")
install(FILES mars/comm/windows/sys/cdefs.h mars/comm/windows/sys/time.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mars/comm/windows/sys")
install(FILES LICENSE LICENSE.zlib "Copyright Notice of Boost" DESTINATION "${CMAKE_INSTALL_DATADIR}/xlog")
install(FILES mars/xlog/crypt/micro-ecc-master/LICENSE.txt
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
