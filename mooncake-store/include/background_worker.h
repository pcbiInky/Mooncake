#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mooncake {

// Runs a callback on a dedicated thread when scheduled. Multiple Schedule()
// calls made before the worker consumes the pending request are coalesced into
// one callback invocation. An optional periodic callback mode is available to
// callers that also need a fixed-interval safety-net run. Start() and Stop()
// must be called serially by the owner; Schedule() may be called concurrently
// from other threads.
class BackgroundWorker {
   public:
    using Callback = std::function<void()>;

    explicit BackgroundWorker(Callback callback)
        : callback_(std::move(callback)) {}

    BackgroundWorker(Callback callback,
                     std::chrono::milliseconds periodic_interval)
        : callback_(std::move(callback)),
          periodic_interval_(periodic_interval) {
        if (periodic_interval.count() <= 0) {
            throw std::invalid_argument(
                "BackgroundWorker periodic interval must be positive");
        }
    }

    ~BackgroundWorker() { Stop(); }

    BackgroundWorker(const BackgroundWorker&) = delete;
    BackgroundWorker& operator=(const BackgroundWorker&) = delete;

    void Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        requested_ = false;
        if (periodic_interval_) {
            thread_ = std::thread(&BackgroundWorker::PeriodicThreadFunc, this);
        } else {
            thread_ = std::thread(&BackgroundWorker::ThreadFunc, this);
        }
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !thread_.joinable()) {
                return;
            }
            running_ = false;
            requested_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Schedule() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            requested_ = true;
        }
        cv_.notify_one();
    }

    // Changes the interval used by periodic mode. A concurrent periodic wait
    // is restarted from the time of this call. Schedule() remains the only
    // way to request an immediate callback.
    void SetPeriodicInterval(std::chrono::milliseconds periodic_interval) {
        if (periodic_interval.count() <= 0) {
            throw std::invalid_argument(
                "BackgroundWorker periodic interval must be positive");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!periodic_interval_) {
                throw std::logic_error(
                    "BackgroundWorker is not in periodic mode");
            }
            if (*periodic_interval_ == periodic_interval) {
                return;
            }
            periodic_interval_ = periodic_interval;
            ++periodic_interval_generation_;
        }
        cv_.notify_all();
    }

   private:
    void ThreadFunc() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !running_ || requested_; });
                if (!running_) {
                    return;
                }
                requested_ = false;
            }
            callback_();
        }
    }

    void PeriodicThreadFunc() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const uint64_t interval_generation =
                    periodic_interval_generation_;
                const auto interval = *periodic_interval_;
                cv_.wait_for(lock, interval, [this, interval_generation] {
                    return !running_ || requested_ ||
                           periodic_interval_generation_ !=
                               interval_generation;
                });
                if (!running_) {
                    return;
                }
                if (!requested_ && periodic_interval_generation_ !=
                                       interval_generation) {
                    // Restart the wait with the newly selected interval.
                    continue;
                }
                requested_ = false;
            }
            callback_();
        }
    }

    Callback callback_;
    std::optional<std::chrono::milliseconds> periodic_interval_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool running_{false};
    bool requested_{false};
    uint64_t periodic_interval_generation_{0};
};

}  // namespace mooncake
