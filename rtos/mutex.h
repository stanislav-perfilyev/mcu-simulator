#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// rtos/mutex.h — Binary mutex (non-recursive)
// Mirrors FreeRTOS SemaphoreHandle_t used as mutex.
// Priority-order wake-up: highest-priority waiter gets the mutex first.
// ─────────────────────────────────────────────────────────────────────────────
#include "scheduler.h"
#include <array>

namespace rtos {

class Mutex {
public:
    explicit Mutex(Scheduler& s) noexcept : sched_(s) {}

    Mutex(const Mutex&)            = delete;
    Mutex& operator=(const Mutex&) = delete;

    // xSemaphoreTakeRecursive — blocks if held by another task
    void lock() noexcept {
        const TaskHandle_t me = sched_.xTaskGetCurrentTaskHandle();
        if (owner_ == INVALID_TASK) { owner_ = me; return; }
        // Enqueue and block
        if (wait_count_ < MAX_TASKS) waiters_[wait_count_++] = me;
        sched_.block_current();
        // Returns here owning the mutex (set by unlock())
    }

    // xSemaphoreGive
    void unlock() noexcept {
        if (wait_count_ > 0) {
            // Wake the highest-priority waiter
            uint8_t best_idx  = 0;
            uint8_t best_prio = sched_.get_priority(waiters_[0]);
            for (uint8_t i = 1; i < wait_count_; ++i) {
                uint8_t p = sched_.get_priority(waiters_[i]);
                if (p < best_prio) { best_prio = p; best_idx = i; }
            }
            owner_ = waiters_[best_idx];
            // Compact waiters array
            waiters_[best_idx] = waiters_[--wait_count_];
            sched_.unblock(owner_);
        } else {
            owner_ = INVALID_TASK;
        }
    }

    // xSemaphoreTake with pdFALSE timeout (non-blocking)
    [[nodiscard]] bool try_lock() noexcept {
        if (owner_ != INVALID_TASK) return false;
        owner_ = sched_.xTaskGetCurrentTaskHandle();
        return true;
    }

    [[nodiscard]] bool is_locked()  const noexcept { return owner_ != INVALID_TASK; }
    [[nodiscard]] TaskHandle_t owner() const noexcept { return owner_; }

    // RAII guard (like std::lock_guard)
    struct [[nodiscard]] Guard {
        explicit Guard(Mutex& m) noexcept : mtx_(m) { mtx_.lock(); }
        ~Guard() noexcept { mtx_.unlock(); }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        Mutex& mtx_;
    };
    [[nodiscard]] Guard lock_guard() noexcept { return Guard{*this}; }

private:
    Scheduler&    sched_;
    TaskHandle_t  owner_      = INVALID_TASK;
    std::array<TaskHandle_t, MAX_TASKS> waiters_ {};
    uint8_t       wait_count_ = 0;
};

} // namespace rtos
