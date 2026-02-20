#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace chromatin::ws {

/// Fixed-size thread pool for offloading blocking operations from the
/// uWebSockets event loop (e.g. kademlia.store() calls).
class WorkerPool {
public:
    explicit WorkerPool(size_t num_threads = 4, size_t max_queue_size = 1024)
        : max_queue_size_(max_queue_size) {
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            t.join();
        }
    }

    // Non-copyable, non-movable
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    /// Enqueue a job for execution on a worker thread.
    /// Returns false if the queue is full (backpressure).
    bool post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (jobs_.size() >= max_queue_size_) {
                return false;  // queue full, reject
            }
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
        return true;
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            job();
        }
    }

    size_t max_queue_size_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
};

} // namespace chromatin::ws
