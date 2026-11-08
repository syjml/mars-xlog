// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

/*
 * mmap_util.c
 *
 *  Created on: 2016-2-22
 *      Author: yanguoyue
 */

#include "mmap_util.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mars::comm {

MappedFile::~MappedFile() {
    Close();
}

bool MappedFile::Open(const std::filesystem::path& path, unsigned int size) {
    Close();
    if (path.empty() || size == 0)
        return false;

    // Preserve existing log bytes and allocate any missing space before mapping it.
    const char zeros[4096]{};
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER end{};
    bool ready = SetFilePointerEx(file, end, &end, FILE_END);
    for (auto length = end.QuadPart; ready && length < size;) {
        const DWORD chunk = std::min<DWORD>(sizeof(zeros), size - static_cast<unsigned int>(length));
        DWORD written = 0;
        ready = WriteFile(file, zeros, chunk, &written, nullptr) && written != 0;
        length += written;
    }
    if (ready) {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, size, nullptr);
        if (mapping) {
            data_ = static_cast<char*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, size));
            CloseHandle(mapping);
        }
    }
    CloseHandle(file);
#else
    const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (file < 0)
        return false;

    struct stat info{};
    bool ready = fstat(file, &info) == 0 && S_ISREG(info.st_mode);
    for (off_t length = info.st_size; ready && length < size;) {
        const auto chunk = std::min<std::size_t>(sizeof(zeros), size - static_cast<unsigned int>(length));
        const auto written = pwrite(file, zeros, chunk, length);
        if (written < 0 && errno == EINTR)
            continue;
        ready = written > 0;
        if (ready)
            length += written;
    }
    if (ready) {
        void* view = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
        if (view != MAP_FAILED) {
            data_ = static_cast<char*>(view);
            size_ = size;
        }
    }
    close(file);
#endif
    return data_ != nullptr;
}

void MappedFile::Close() {
    if (!data_)
        return;
#ifdef _WIN32
    UnmapViewOfFile(data_);
#else
    munmap(data_, size_);
    size_ = 0;
#endif
    data_ = nullptr;
}

}  // namespace mars::comm
