// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PROJECT_DEFINE
#define PROJECT_DEFINE

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS 1
#endif
#ifndef _SCL_SECURE_NO_WARNINGS
#define _SCL_SECURE_NO_WARNINGS 1
#endif

#include <basetsd.h>
#include <inttypes.h>
#include <io.h>  // for close
#include <stdio.h>
#include <string.h>
#ifdef __MINGW32__
#include <direct.h>
#include <sys/stat.h>
#ifdef __cplusplus
// libstdc++ undefines the stdio macros when it first includes <cstdio>.
#include <cstdio>
#endif
#endif
// 编译时在最上方引用本文件，可强制保证WinSock2.h在Windows.h之前被引用。
#include <WinSock2.h>
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE* mars_fopen_utf8(const char* _path, const char* _model);
int mars_mkdir_utf8(const char* _path, unsigned short _model);
int mars_access_utf8(char const* _path, int _model);
// int mars_remove_utf8(char const* _path);

#ifdef __cplusplus
}
#endif

#undef fopen
#define fopen mars_fopen_utf8

#ifdef WINAPI_FAMILY_PARTITION
#include <winapifamily.h>
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_PC_APP) && WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_PHONE_APP) \
    && WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) && WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#define UWP 1
#endif
#endif

#if !UWP

#endif
#ifdef UWP
#define mkdir _mkdir4UWP
#else
#define mkdir mars_mkdir_utf8
#endif
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define stricmp _stricmp
#ifndef __MINGW32__
#define snprintf _snprintf
#endif
#undef access
#define access mars_access_utf8
#if !defined(WIN32)
#define sscanf sscanf_s
#endif

#define strdup _strdup

#ifndef S_ISDIR
#define S_ISDIR(x) (_S_IFDIR & x)
#endif

#if defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP
/// This code is for Windows phone 8
#define PLATFORM_WP8
#if !UWP
#define InitializeCriticalSection(x) InitializeCriticalSectionEx(x, 0, 0)
#endif
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#if !defined(WIN32)
#define sscanf sscanf_s
#endif

typedef SSIZE_T ssize_t;

#if defined(WIN32) && !defined(SIZE_T_MAX)
#define SIZE_T_MAX UINT_MAX
#endif

struct tcp_info {
    void* dummy;
};

#endif
