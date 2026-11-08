#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace mars::comm {

class Thread {
 public:
    explicit Thread(std::function<void()> task) : state_(std::make_shared<State>(std::move(task))) {}

    ~Thread() {
        // A temporary Thread can schedule deferred cleanup and release its handle.
        if (worker_.joinable())
            worker_.detach();
    }

    void start() { start_after(0); }

    void start_after(unsigned int milliseconds) {
        if (isruning())
            return;
        join();
        state_->cancelled = false;
        state_->running = true;
        try {
            worker_ = std::thread([state = state_, milliseconds] {
                std::unique_lock<std::mutex> lock(state->mutex);
                const bool cancelled = state->condition.wait_for(
                    lock, std::chrono::milliseconds(milliseconds), [&] { return state->cancelled; });
                lock.unlock();
                if (!cancelled)
                    state->task();
                state->running = false;
            });
        } catch (...) {
            state_->running = false;
            throw;
        }
    }

    void cancel_after() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->cancelled = true;
        }
        state_->condition.notify_all();
        join();
    }

    void join() {
        if (worker_.joinable())
            worker_.join();
    }

    bool isruning() const { return state_->running; }

 private:
    struct State {
        explicit State(std::function<void()> operation) : task(std::move(operation)) {}

        std::function<void()> task;
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic<bool> running{false};
        bool cancelled = false;
    };

    std::shared_ptr<State> state_;
    std::thread worker_;
};

}  // namespace mars::comm
