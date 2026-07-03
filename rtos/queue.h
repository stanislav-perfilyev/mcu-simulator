#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// rtos/queue.h — Typed blocking FIFO queue
// Equivalent to xQueueCreate(N, sizeof(T)) in FreeRTOS.
// Senders block when full; receivers block when empty.
// ─────────────────────────────────────────────────────────────────────────────
#include "scheduler.h"
#include <array>
#include <utility>

namespace rtos {

template<typename T, size_t N>
class Queue {
    static_assert(N > 0, "Queue capacity must be > 0");
public:
    explicit Queue(Scheduler& s) noexcept : sched_(s) {}

    Queue(const Queue&)            = delete;
    Queue& operator=(const Queue&) = delete;

    // xQueueSend — blocks if full
    void send(T item) noexcept {
        while (count_ == N) {
            if (send_count_ < MAX_TASKS)
                send_waiters_[send_count_++] = sched_.xTaskGetCurrentTaskHandle();
            sched_.block_current();
        }
        buf_[tail_] = std::move(item);
        tail_       = (tail_ + 1) % N;
        ++count_;
        wake_receiver();
    }

    // xQueueReceive — blocks if empty
    [[nodiscard]] T receive() noexcept {
        while (count_ == 0) {
            if (recv_count_ < MAX_TASKS)
                recv_waiters_[recv_count_++] = sched_.xTaskGetCurrentTaskHandle();
            sched_.block_current();
        }
        T item  = std::move(buf_[head_]);
        head_   = (head_ + 1) % N;
        --count_;
        wake_sender();
        return item;
    }

    // xQueueSendFromISR (non-blocking variant)
    [[nodiscard]] bool try_send(T item) noexcept {
        if (count_ == N) return false;
        buf_[tail_] = std::move(item);
        tail_       = (tail_ + 1) % N;
        ++count_;
        wake_receiver();
        return true;
    }

    // xQueuePeek (non-blocking)
    [[nodiscard]] bool try_receive(T& out) noexcept {
        if (count_ == 0) return false;
        out   = std::move(buf_[head_]);
        head_ = (head_ + 1) % N;
        --count_;
        wake_sender();
        return true;
    }

    [[nodiscard]] bool   empty()    const noexcept { return count_ == 0; }
    [[nodiscard]] bool   full()     const noexcept { return count_ == N; }
    [[nodiscard]] size_t size()     const noexcept { return count_; }
    [[nodiscard]] size_t capacity() const noexcept { return N; }

private:
    void wake_receiver() noexcept {
        if (recv_count_ == 0) return;
        uint8_t best_idx  = 0;
        uint8_t best_prio = sched_.get_priority(recv_waiters_[0]);
        for (uint8_t i = 1; i < recv_count_; ++i) {
            uint8_t p = sched_.get_priority(recv_waiters_[i]);
            if (p < best_prio) { best_prio = p; best_idx = i; }
        }
        TaskHandle_t w        = recv_waiters_[best_idx];
        recv_waiters_[best_idx] = recv_waiters_[--recv_count_];
        sched_.unblock(w);
    }

    void wake_sender() noexcept {
        if (send_count_ == 0) return;
        uint8_t best_idx  = 0;
        uint8_t best_prio = sched_.get_priority(send_waiters_[0]);
        for (uint8_t i = 1; i < send_count_; ++i) {
            uint8_t p = sched_.get_priority(send_waiters_[i]);
            if (p < best_prio) { best_prio = p; best_idx = i; }
        }
        TaskHandle_t w        = send_waiters_[best_idx];
        send_waiters_[best_idx] = send_waiters_[--send_count_];
        sched_.unblock(w);
    }

    Scheduler&        sched_;
    std::array<T, N>  buf_       {};
    size_t            head_      = 0;
    size_t            tail_      = 0;
    size_t            count_     = 0;

    std::array<TaskHandle_t, MAX_TASKS> recv_waiters_ {};
    std::array<TaskHandle_t, MAX_TASKS> send_waiters_ {};
    uint8_t recv_count_ = 0;
    uint8_t send_count_ = 0;
};

} // namespace rtos
