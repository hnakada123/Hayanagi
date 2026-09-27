#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace shogi {

// 呼び出し元を含めて N スレッドで実行する再利用可能なワーカー群。
// run は同期的。同じインスタンスへの並行呼び出し・再帰呼び出しは不可。
class ParallelTeam {
public:
    explicit ParallelTeam(int count) {
        try {
            for (int i = 1; i < std::clamp(count, 1, 128); ++i) {
                threads_.emplace_back([this, i] { worker_loop(static_cast<std::size_t>(i)); });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~ParallelTeam() { shutdown(); }
    ParallelTeam(const ParallelTeam&) = delete;
    ParallelTeam& operator=(const ParallelTeam&) = delete;

    std::size_t size() const { return threads_.size() + 1; }

    void run(std::size_t count, std::function<void(std::size_t, std::size_t)> task) {
        if (count == 0) return;
        if (threads_.empty() || count == 1) {
            for (std::size_t i = 0; i < count; ++i) task(i, 0);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = std::move(task);
            count_ = count;
            next_.store(0, std::memory_order_relaxed);
            failed_.store(false, std::memory_order_relaxed);
            error_ = nullptr;
            active_ = threads_.size();
            ++generation_;
        }
        start_.notify_all();
        execute(0);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return active_ == 0; });
        task_ = {};
        if (error_) std::rethrow_exception(error_);
    }

private:
    void execute(std::size_t worker) {
        try {
            while (!failed_.load(std::memory_order_relaxed)) {
                const std::size_t index = next_.fetch_add(1, std::memory_order_relaxed);
                if (index >= count_) break;
                task_(index, worker);
            }
        } catch (...) {
            failed_.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!error_) error_ = std::current_exception();
        }
    }

    void worker_loop(std::size_t worker) {
        std::size_t generation = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            start_.wait(lock, [&] { return exiting_ || generation_ != generation; });
            if (exiting_) return;
            generation = generation_;
            lock.unlock();
            execute(worker);
            lock.lock();
            if (--active_ == 0) done_.notify_one();
        }
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exiting_ = true;
        }
        start_.notify_all();
        for (auto& thread : threads_) thread.join();
    }

    std::mutex mutex_;
    std::condition_variable start_, done_;
    std::vector<std::thread> threads_;
    std::function<void(std::size_t, std::size_t)> task_;
    std::atomic_size_t next_{0};
    std::atomic_bool failed_{false};
    std::size_t count_ = 0, active_ = 0, generation_ = 0;
    bool exiting_ = false;
    std::exception_ptr error_;
};

}  // namespace shogi
