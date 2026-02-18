#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "ws/worker_pool.h"

using chromatin::ws::WorkerPool;

TEST(WorkerPool, ExecutesJobs) {
    std::atomic<int> counter{0};
    {
        WorkerPool pool(2);
        pool.post([&] { counter.fetch_add(1); });
        pool.post([&] { counter.fetch_add(1); });
        pool.post([&] { counter.fetch_add(1); });
    }
    // Pool destructor joins all threads, so all jobs must be done.
    EXPECT_EQ(counter.load(), 3);
}

TEST(WorkerPool, ConcurrentExecution) {
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    {
        WorkerPool pool(4);
        for (int i = 0; i < 4; ++i) {
            pool.post([&] {
                int cur = concurrent.fetch_add(1) + 1;
                // Update max_concurrent atomically
                int prev = max_concurrent.load();
                while (prev < cur && !max_concurrent.compare_exchange_weak(prev, cur)) {
                    // retry
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                concurrent.fetch_sub(1);
            });
        }
    }
    EXPECT_GE(max_concurrent.load(), 2)
        << "Expected at least 2 concurrent jobs but got " << max_concurrent.load();
}

TEST(WorkerPool, GracefulShutdown) {
    std::atomic<int> completed{0};
    {
        WorkerPool pool(2);
        for (int i = 0; i < 10; ++i) {
            pool.post([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                completed.fetch_add(1);
            });
        }
        // Pool destructor should drain the queue.
    }
    EXPECT_EQ(completed.load(), 10);
}
