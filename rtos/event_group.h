#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// rtos/event_group.h — 32-bit event flags (xEventGroup in FreeRTOS)
// Tasks wait for ANY or ALL of a set of bits to be set.
// ─────────────────────────────────────────────────────────────────────────────
#include "scheduler.h"
#include <array>
#include <cstdint>

namespace rtos {

/// RTOS event group: tasks can set, clear, and wait on bit-flag combinations.
class EventGroup {
public:
    explicit EventGroup(Scheduler& s) noexcept : sched_(s) {}

    EventGroup(const EventGroup&)            = delete;
    EventGroup& operator=(const EventGroup&) = delete;

    // xEventGroupSetBits — sets bits, wakes satisfied waiters
    void set_bits(uint32_t bits) noexcept {
        bits_ |= bits;
        check_waiters();
    }

    // xEventGroupClearBits
    void clear_bits(uint32_t bits) noexcept { bits_ &= ~bits; }

    // xEventGroupGetBits (non-blocking peek)
    [[nodiscard]] uint32_t get_bits() const noexcept { return bits_; }

    // xEventGroupWaitBits — blocks until condition met
    // wait_all=true : wait for ALL `bits` to be set
    // wait_all=false: wait for ANY of `bits` to be set
    // clear_on_exit : clear matched bits before returning
    [[nodiscard]] uint32_t wait_bits(uint32_t bits,
                       bool     wait_all     = true,
                       bool     clear_on_exit = true) noexcept {
        while (!satisfied(bits, wait_all)) {
            if (wait_count_ < MAX_TASKS) {
                waiters_[wait_count_++] = {
                    sched_.xTaskGetCurrentTaskHandle(), bits, wait_all };
            }
            sched_.block_current();
        }
        const uint32_t snapshot = bits_;
        if (clear_on_exit) bits_ &= ~bits;
        return snapshot;
    }

    // Non-blocking check
    [[nodiscard]] bool check_bits(uint32_t bits, bool all = true) const noexcept {
        return satisfied(bits, all);
    }

private:
    /// A task waiting on a specific bit-mask condition.
    struct Waiter {
        TaskHandle_t task;
        uint32_t     wait_bits;
        bool         all; // true = wait for ALL bits
    };

    [[nodiscard]] bool satisfied(uint32_t bits, bool all) const noexcept {
        return all ? (bits_ & bits) == bits   // ALL
                   : (bits_ & bits) != 0;     // ANY
    }

    void check_waiters() noexcept {
        for (uint8_t i = 0; i < wait_count_; ) {
            if (satisfied(waiters_[i].wait_bits, waiters_[i].all)) {
                sched_.unblock(waiters_[i].task);
                waiters_[i] = waiters_[--wait_count_]; // compact
            } else {
                ++i;
            }
        }
    }

    Scheduler& sched_;
    uint32_t   bits_       = 0;
    std::array<Waiter, MAX_TASKS> waiters_ {};
    uint8_t    wait_count_ = 0;
};

} // namespace rtos
