#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace mars::comm {

class DelayedTask {
 public:
    DelayedTask(std::chrono::milliseconds delay, std::function<void()> task)
    : worker_([this, delay, task = std::move(task)] {
        std::unique_lock lock(mutex_);
        if (condition_.wait_for(lock, delay, [this] { return cancelled_; }))
            return;
        lock.unlock();
        task();
    }) {}

    ~DelayedTask() {
        RequestCancel();
        Join();
    }

    void RequestCancel() {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        condition_.notify_one();
    }

    void Join() {
        if (worker_.joinable())
            worker_.join();
    }

 private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool cancelled_ = false;
    std::thread worker_;
};

template <class Owner>
void DeferRelease(Owner owner) {
    std::thread([owner = std::move(owner)] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }).detach();
}

}  // namespace mars::comm
