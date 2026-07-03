// -----------------------------------------------------------------------------
// rtos/scheduler.cpp -- RTOS kernel implementation
// -----------------------------------------------------------------------------
#include "scheduler.h"
#include <algorithm>
#include <stdexcept>

namespace rtos {

// -- xTaskCreate --------------------------------------------------------------
TaskHandle_t Scheduler::xTaskCreate(
        const char* name, uint8_t priority, TaskFn fn) noexcept {
    if (task_count_ >= MAX_TASKS) return INVALID_TASK;

    const TaskHandle_t id = task_count_++;
    TCB& tcb              = tasks_[id];
    tcb.id                = id;
    tcb.priority          = priority;
    tcb.state             = TaskState::READY;
    tcb.resume_tick       = 0;
    std::strncpy(tcb.name, name ? name : "", sizeof(tcb.name) - 1);
    tcb.name[sizeof(tcb.name) - 1] = '\0';

    task_fns_[id] = std::move(fn);

    getcontext(&tcb.ctx);
    tcb.ctx.uc_stack.ss_sp   = tcb.stack;
    tcb.ctx.uc_stack.ss_size = STACK_SIZE;
    tcb.ctx.uc_link          = nullptr;

    // Pass Scheduler* as two uint32_t halves (makecontext takes int args)
    auto ptr = reinterpret_cast<uintptr_t>(this);
    makecontext(&tcb.ctx,
                reinterpret_cast<void(*)()>(task_entry),
                3,
                static_cast<uint32_t>(ptr & 0xFFFFFFFFu),
                static_cast<uint32_t>((ptr >> 32) & 0xFFFFFFFFu),
                static_cast<uint32_t>(id));
    return id;
}

// -- task_entry trampoline ----------------------------------------------------
void Scheduler::task_entry(uint32_t lo, uint32_t hi, uint32_t id) noexcept {
    auto* s = reinterpret_cast<Scheduler*>(
        (static_cast<uintptr_t>(hi) << 32) | lo);

    try { s->task_fns_[id](); } catch (...) {}

    // Task returned: mark deleted, then pick next with DES
    s->tasks_[id].state = TaskState::DELETED;
    s->schedule_after_delete();
    // unreachable
}

// -- vTaskStartScheduler ------------------------------------------------------
void Scheduler::vTaskStartScheduler() noexcept {
    getcontext(&idle_ctx_);
    idle_ctx_.uc_stack.ss_sp   = idle_stack_;
    idle_ctx_.uc_stack.ss_size = STACK_SIZE;

    TaskHandle_t first = select_next();
    if (first == INVALID_TASK) return;

    current_              = first;
    tasks_[first].state   = TaskState::RUNNING;
    swapcontext(&idle_ctx_, &tasks_[first].ctx);
    // Returns here when all tasks complete or deadlock
}

// -- vTaskDelay ---------------------------------------------------------------
void Scheduler::vTaskDelay(uint32_t ticks) noexcept {
    if (current_ == INVALID_TASK) return;
    if (ticks == 0) {
        tasks_[current_].state = TaskState::READY;
        schedule_from(current_);
        return;
    }
    tasks_[current_].resume_tick = tick_count_ + ticks;
    tasks_[current_].state       = TaskState::BLOCKED;
    schedule_from(current_);
}

// -- vTaskSuspend / vTaskResume / vTaskDelete ---------------------------------
void Scheduler::vTaskSuspend(TaskHandle_t task) noexcept {
    if (task >= task_count_) return;
    tasks_[task].state = TaskState::SUSPENDED;
    if (task == current_) schedule_from(current_);
}

void Scheduler::vTaskResume(TaskHandle_t task) noexcept {
    if (task >= task_count_) return;
    if (tasks_[task].state == TaskState::SUSPENDED)
        tasks_[task].state = TaskState::READY;
}

void Scheduler::vTaskDelete(TaskHandle_t task) noexcept {
    if (task >= task_count_) return;
    tasks_[task].state = TaskState::DELETED;
    if (task == current_) schedule_from(current_);
}

// -- block_current / unblock --------------------------------------------------
void Scheduler::block_current() noexcept {
    if (current_ == INVALID_TASK) return;
    tasks_[current_].state = TaskState::BLOCKED;
    schedule_from(current_);
}

void Scheduler::unblock(TaskHandle_t task) noexcept {
    if (task >= task_count_) return;
    if (tasks_[task].state != TaskState::BLOCKED) return;
    tasks_[task].state = TaskState::READY;

    // Preempt if newly unblocked task has higher priority
    if (current_ != INVALID_TASK &&
        tasks_[task].priority < tasks_[current_].priority) {
        tasks_[current_].state = TaskState::READY;
        schedule_from(current_);
    }
}

// -- tick ---------------------------------------------------------------------
void Scheduler::tick(uint32_t ms) noexcept {
    tick_count_ += ms;
    for (uint8_t i = 0; i < task_count_; ++i) {
        TCB& t = tasks_[i];
        if (t.state == TaskState::BLOCKED && t.resume_tick > 0 &&
            t.resume_tick <= tick_count_) {
            t.state       = TaskState::READY;
            t.resume_tick = 0;
        }
    }
    TaskHandle_t next = select_next();
    if (next != INVALID_TASK && current_ != INVALID_TASK &&
        tasks_[next].priority < tasks_[current_].priority) {
        tasks_[current_].state = TaskState::READY;
        schedule_from(current_);
    }
}

// -- Introspection ------------------------------------------------------------
TaskState Scheduler::get_state(TaskHandle_t t) const noexcept {
    if (t >= task_count_) return TaskState::DELETED;
    return tasks_[t].state;
}

uint8_t Scheduler::get_priority(TaskHandle_t t) const noexcept {
    if (t >= task_count_) return 0xFF;
    return tasks_[t].priority;
}

// -- select_next --------------------------------------------------------------
TaskHandle_t Scheduler::select_next() const noexcept {
    uint8_t best_prio = 0xFF;
    for (uint8_t i = 0; i < task_count_; ++i) {
        if (tasks_[i].state == TaskState::READY && tasks_[i].priority < best_prio)
            best_prio = tasks_[i].priority;
    }
    if (best_prio == 0xFF) return INVALID_TASK;

    uint8_t start = (current_ == INVALID_TASK) ? uint8_t{0} : static_cast<uint8_t>((current_ + 1) % task_count_);
    for (uint8_t i = 0; i < task_count_; ++i) {
        uint8_t idx = static_cast<uint8_t>((start + i) % task_count_);
        if (tasks_[idx].state == TaskState::READY &&
            tasks_[idx].priority == best_prio)
            return idx;
    }
    return INVALID_TASK;
}

// -- schedule_from ------------------------------------------------------------
// Called when prev (currently running) needs to yield.
// DES-style: if all tasks are blocked, advance simulated time.
void Scheduler::schedule_from(TaskHandle_t prev) noexcept {
    while (true) {
        TaskHandle_t next = select_next();

        if (next != INVALID_TASK) {
            if (next == prev) {
                tasks_[prev].state = TaskState::RUNNING;
                return;
            }
            current_              = next;
            tasks_[next].state    = TaskState::RUNNING;
            swapcontext(&tasks_[prev].ctx, &tasks_[next].ctx);
            // Returns here when prev is rescheduled
            tasks_[prev].state    = TaskState::RUNNING;
            return;
        }

        // No READY tasks: DES advance
        uint32_t earliest = UINT32_MAX;
        for (uint8_t i = 0; i < task_count_; ++i) {
            const TCB& t = tasks_[i];
            if (t.state == TaskState::BLOCKED && t.resume_tick > 0)
                earliest = std::min(earliest, t.resume_tick);
        }

        if (earliest == UINT32_MAX) {
            // Deadlock: return to idle
            current_ = INVALID_TASK;
            swapcontext(&tasks_[prev].ctx, &idle_ctx_);
            return;
        }

        tick_count_ = earliest;
        for (uint8_t i = 0; i < task_count_; ++i) {
            TCB& t = tasks_[i];
            if (t.state == TaskState::BLOCKED &&
                t.resume_tick > 0 && t.resume_tick <= tick_count_) {
                t.state       = TaskState::READY;
                t.resume_tick = 0;
            }
        }
    }
}

// -- schedule_after_delete ----------------------------------------------------
// Called when a task function returns. No context to save (one-way jump).
// Applies DES same as schedule_from, but uses setcontext instead of swapcontext.
void Scheduler::schedule_after_delete() noexcept {
    while (true) {
        TaskHandle_t next = select_next();

        if (next != INVALID_TASK) {
            current_              = next;
            tasks_[next].state    = TaskState::RUNNING;
            setcontext(&tasks_[next].ctx);
            return; // unreachable
        }

        // No READY tasks: DES advance
        uint32_t earliest = UINT32_MAX;
        for (uint8_t i = 0; i < task_count_; ++i) {
            const TCB& t = tasks_[i];
            if (t.state == TaskState::BLOCKED && t.resume_tick > 0)
                earliest = std::min(earliest, t.resume_tick);
        }

        if (earliest == UINT32_MAX) {
            current_ = INVALID_TASK;
            setcontext(&idle_ctx_);
            return; // unreachable
        }

        tick_count_ = earliest;
        for (uint8_t i = 0; i < task_count_; ++i) {
            TCB& t = tasks_[i];
            if (t.state == TaskState::BLOCKED &&
                t.resume_tick > 0 && t.resume_tick <= tick_count_) {
                t.state       = TaskState::READY;
                t.resume_tick = 0;
            }
        }
    }
}

} // namespace rtos
