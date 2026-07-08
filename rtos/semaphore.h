#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// rtos/semaphore.h — Counting semaphore
// xSemaphoreCreateCounting(max_count, initial_count) equivalent.
// Binary semaphore: initial=0, max=1  (signal/wait pattern).
// ─────────────────────────────────────────────────────────────────────────────
#include "scheduler.h"
#include <array>
#include <cstdint>
#include <climits>

namespace rtos {

/// RTOS counting semaphore: give/take with blocked-task wait queue.
class Semaphore {
public:
    explicit Semaphore(Scheduler& s,
                       int32_t initial_count = 0,
                       int32_t max_count     = INT32_MAX) noexcept
        : sched_(s), count_(initial_count), max_count_(max_count) {}

    Semaphore(const Semaphore&)            = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    // xSemaphoreTake — blocks until count > 0
    void take() noexcept {
        if (count_ > 0) { --count_; return; }
        if (wait_count_ < MAX_TASKS)
            waiters_[wait_count_++] = sched_.xTaskGetCurrentTaskHandle();
        sched_.block_current();
        // Returns here after give() decremented count for us
    }

    // xSemaphoreGive — increments count or wakes a waiter
    void give() noexcept {
        if (wait_count_ > 0) {
            // Wake highest-priority waiter (count stays 0 — given directly)
            uint8_t best_idx  = 0;
            uint8_t best_prio = sched_.get_priority(waiters_[0]);
            for (uint8_t i = 1; i < wait_count_; ++i) {
                uint8_t p = sched_.get_priority(waiters_[i]);
                if (p < best_prio) { best_prio = p; best_idx = i; }
            }
            TaskHandle_t w    = waiters_[best_idx];
            waiters_[best_idx] = waiters_[--wait_count_];
            sched_.unblock(w);
        } else if (count_ < max_count_) {
            ++count_;
        }
    }

    // Non-blocking take
    [[nodiscard]] bool try_take() noexcept {
        if (count_ <= 0) return false;
        --count_;
        return true;
    }

    [[nodiscard]] int32_t count()     const noexcept { return count_; }
    [[nodiscard]] int32_t max_count() const noexcept { return max_count_; }

private:
    Scheduler&  sched_;
    int32_t     count_;
    int32_t     max_count_;
    std::array<TaskHandle_t, MAX_TASKS> waiters_ {};
    uint8_t     wait_count_ = 0;
};

} // namespace rtos
