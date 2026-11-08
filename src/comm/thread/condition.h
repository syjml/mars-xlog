// Tencent is pleased to support the open source community by making Mars available.
// Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

// Licensed under the MIT License (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://opensource.org/licenses/MIT

// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#ifdef _WIN32
#include "../windows/thread/condition.h"
#else
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace mars::comm {
class Condition {
 public:
    void wait(unsigned long milliseconds) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return notified_; });
        notified_ = false;
    }

    void notifyAll(bool = true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            notified_ = true;
        }
        condition_.notify_all();
    }

 private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool notified_ = false;
};
}
#endif
