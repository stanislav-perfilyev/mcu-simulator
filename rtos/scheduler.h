#pragma once
// -----------------------------------------------------------------------------
// rtos/scheduler.h -- FreeRTOS-compatible RTOS kernel (ucontext_t backend)
// Cooperative + priority-preemptive scheduler for the MCU Simulator.
// -----------------------------------------------------------------------------
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <ucontext.h>

namespace rtos {

inline constexpr size_t   MAX_TASKS   = 16;
inline constexpr size_t   STACK_SIZE  = 8192;
inline constexpr uint8_t  PRI_HIGHEST = 0;
inline constexpr uint8_t  PRI_LOWEST  = 7;

using TaskHandle_t = uint8_t;
inline constexpr TaskHandle_t INVALID_TASK = 0xFF;

enum class TaskState : uint8_t {
    READY=0, RUNNING=1, BLOCKED=2, SUSPENDED=3, DELETED=4
};

struct TCB {
    TaskHandle_t  id           = INVALID_TASK;
    char          name[16]     = {};
    uint8_t       priority     = PRI_LOWEST;
    TaskState     state        = TaskState::DELETED;
    uint32_t      resume_tick  = 0;
    ucontext_t    ctx          = {};
    alignas(16) char stack[STACK_SIZE] = {};
};

// --------------------------------------------------------------------------
// Scheduler
// --------------------------------------------------------------------------
class Scheduler {
public:
    using TaskFn = std::function<void()>;

    Scheduler() noexcept = default;
    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // -- FreeRTOS API ---------------------------------------------------------
    [[nodiscard]] TaskHandle_t xTaskCreate(
        const char* name, uint8_t priority, TaskFn fn) noexcept;

    void vTaskStartScheduler() noexcept;
    void vTaskDelay(uint32_t ticks) noexcept;
    void vTaskSuspend(TaskHandle_t task) noexcept;
    void vTaskResume(TaskHandle_t task)  noexcept;
    void vTaskDelete(TaskHandle_t task)  noexcept;

    [[nodiscard]] uint32_t     xTaskGetTickCount()          const noexcept { return tick_count_; }
    [[nodiscard]] TaskHandle_t xTaskGetCurrentTaskHandle()  const noexcept { return current_; }

    // -- Tick (external time injection) ---------------------------------------
    void tick(uint32_t ms = 1) noexcept;

    // -- Used by synchronisation primitives -----------------------------------
    void block_current() noexcept;
    void unblock(TaskHandle_t task) noexcept;

    // -- Introspection (for tests) --------------------------------------------
    [[nodiscard]] TaskState get_state   (TaskHandle_t t) const noexcept;
    [[nodiscard]] uint8_t   get_priority(TaskHandle_t t) const noexcept;
    [[nodiscard]] uint8_t   task_count  ()               const noexcept { return task_count_; }

private:
    std::array<TCB,    MAX_TASKS> tasks_    {};
    std::array<TaskFn, MAX_TASKS> task_fns_ {};
    uint8_t       task_count_ = 0;
    uint32_t      tick_count_ = 0;
    TaskHandle_t  current_    = INVALID_TASK;

    ucontext_t    idle_ctx_   = {};
    alignas(16) char idle_stack_[STACK_SIZE] = {};

    // Pick highest-priority READY task (round-robin among equal priorities).
    [[nodiscard]] TaskHandle_t select_next() const noexcept;

    // Save context of prev, restore context of next.
    void switch_to(TaskHandle_t prev, TaskHandle_t next) noexcept;

    // Called when current task needs to give up CPU.
    void schedule_from(TaskHandle_t prev) noexcept;

    // Called after a task function returns (no context to save).
    // Runs DES loop + setcontext to next task or idle.
    void schedule_after_delete() noexcept;

    // Task entry trampoline (passed to makecontext).
    static void task_entry(uint32_t lo, uint32_t hi, uint32_t id) noexcept;
};

} // namespace rtos
