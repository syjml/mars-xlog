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
 * @Author: garryyan
 * @LastEditors: garryyan
 * @Date: 2019-03-05 15:10:24
 * @LastEditTime: 2019-03-05 15:10:50
 */

#include "mars/xlog/xlogger_interface.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xlog/xlogger/xlogger_category.h"
#include "xlogger_appender.h"

namespace mars::xlog {
namespace {
using comm::XloggerCategory;

struct Registry {
    std::mutex lifecycle_mutex;
    std::mutex mutex;
    std::map<std::string, uintptr_t, std::less<>> names;
    std::unordered_map<uintptr_t, std::shared_ptr<XloggerCategory>> entries;
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

std::shared_ptr<XloggerCategory> FindByHandle(uintptr_t handle) {
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    const auto it = registry.entries.find(handle);
    return it != registry.entries.end() ? it->second : nullptr;
}

void FlushAppender(XloggerAppender& appender, bool sync) {
    if (sync)
        appender.FlushSync();
    else
        appender.Flush();
}
}  // namespace

XloggerCategory* NewXloggerInstance(const XLogConfig& config, TLogLevel level) {
    if (config.logdir_.empty() || config.nameprefix_.empty())
        return nullptr;

    auto& registry = GetRegistry();
    std::lock_guard lifecycle_lock(registry.lifecycle_mutex);
    {
        std::lock_guard lock(registry.mutex);
        const auto it = registry.names.find(config.nameprefix_);
        if (it != registry.names.end())
            return registry.entries.at(it->second).get();
    }

    auto category = std::make_shared<XloggerCategory>(XloggerAppender::NewInstance(config, 0));
    category->SetLevel(level);
    auto* instance = category.get();
    const auto handle = reinterpret_cast<uintptr_t>(instance);
    std::lock_guard lock(registry.mutex);
    const auto name = registry.names.emplace(config.nameprefix_, handle).first;
    try {
        registry.entries.emplace(handle, std::move(category));
    } catch (...) {
        registry.names.erase(name);
        throw;
    }
    return instance;
}

XloggerCategory* GetXloggerInstance(const char* nameprefix) {
    if (!nameprefix)
        return nullptr;

    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    const auto it = registry.names.find(nameprefix);
    return it != registry.names.end() ? registry.entries.at(it->second).get() : nullptr;
}

void ReleaseXloggerInstance(const char* nameprefix) {
    if (!nameprefix)
        return;

    auto& registry = GetRegistry();
    std::lock_guard lifecycle_lock(registry.lifecycle_mutex);
    std::shared_ptr<XloggerCategory> category;
    {
        std::lock_guard lock(registry.mutex);
        const auto it = registry.names.find(nameprefix);
        if (it == registry.names.end())
            return;
        category = std::move(registry.entries.extract(it->second).mapped());
        registry.names.erase(it);
    }
    category->GetAppender().Close();
}

void XloggerWrite(uintptr_t handle, const XLoggerInfo* info, const char* log) {
    if (!handle)
        xlogger_Write(info, log);
    else if (auto category = FindByHandle(handle))
        category->Write(info, log);
}

bool IsEnabledFor(uintptr_t handle, TLogLevel level) {
    if (!handle)
        return xlogger_IsEnabledFor(level);
    auto category = FindByHandle(handle);
    return category && category->IsEnabledFor(level);
}

TLogLevel GetLevel(uintptr_t handle) {
    if (!handle)
        return xlogger_Level();
    auto category = FindByHandle(handle);
    return category ? category->GetLevel() : kLevelNone;
}

void SetLevel(uintptr_t handle, TLogLevel level) {
    if (!handle)
        xlogger_SetLevel(level);
    else if (auto category = FindByHandle(handle))
        category->SetLevel(level);
}

void SetAppenderMode(uintptr_t handle, TAppenderMode mode) {
    if (!handle)
        appender_setmode(mode);
    else if (auto category = FindByHandle(handle))
        category->GetAppender().SetMode(mode);
}

void Flush(uintptr_t handle, bool sync) {
    if (!handle)
        sync ? appender_flush_sync() : appender_flush();
    else if (auto category = FindByHandle(handle))
        FlushAppender(category->GetAppender(), sync);
}

void FlushAll(bool sync) {
    Flush(0, sync);
    std::vector<std::shared_ptr<XloggerCategory>> categories;
    {
        auto& registry = GetRegistry();
        std::lock_guard lock(registry.mutex);
        categories.reserve(registry.entries.size());
        for (const auto& [name, handle] : registry.names)
            categories.push_back(registry.entries.at(handle));
    }
    for (const auto& category : categories)
        FlushAppender(category->GetAppender(), sync);
}

void SetConsoleLogOpen(uintptr_t handle, bool open) {
    if (!handle)
        appender_set_console_log(open);
    else if (auto category = FindByHandle(handle))
        category->GetAppender().SetConsoleLog(open);
}

void SetMaxFileSize(uintptr_t handle, long max_file_size) {
    if (!handle)
        appender_set_max_file_size(max_file_size);
    else if (auto category = FindByHandle(handle))
        category->GetAppender().SetMaxFileSize(max_file_size);
}

void SetMaxAliveTime(uintptr_t handle, long alive_seconds) {
    if (!handle)
        appender_set_max_alive_duration(alive_seconds);
    else if (auto category = FindByHandle(handle))
        category->GetAppender().SetMaxAliveDuration(alive_seconds);
}

}  // namespace mars::xlog
