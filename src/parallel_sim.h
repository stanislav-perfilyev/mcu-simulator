#pragma once
// ─── parallel_sim.h ──────────────────────────────────────────────────────────
//
// Parallel simulation runner for MCU Simulator.
//
// Design:
//   - Each SimTask gets its own FlatMemoryBus + CortexM0 — fully independent.
//   - std::jthread per task: auto-joins on scope exit (C++20).
//   - TraceCollector: mutex-protected event log, safe for N simultaneous writers.
//   - SimResult vector: pre-indexed by sim_id → zero contention for writes.
//
// Thread-safety model:
//   write  → TraceCollector::push()  acquires unique_lock (exclusive)
//   read   → TraceCollector::drain() acquires unique_lock (exclusive, swaps)
//   size() → acquires unique_lock    (safe but call-rare)
//
// Usage:
//   SimulationRunner runner;
//   auto results = runner.run(tasks);
//   auto trace   = runner.trace()->drain();
//
#include "cpu.h"
#include "memory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mcu {

// ─── Trace event ─────────────────────────────────────────────────────────────

struct SimRecord {
    uint32_t    sim_id{0};
    std::string label;
    std::string event;   ///< "START", "CHECKPOINT", "DONE", "EXCEPTION"
    uint64_t    cycles{0};
    uint32_t    pc{0};
};

// ─── Thread-safe trace log ────────────────────────────────────────────────────
//
// Multiple simulation threads push() concurrently; one consumer calls drain().
// Lock granularity is per-push (fine-grained write, bulk read).

class TraceCollector {
public:
    void push(SimRecord r) {
        std::lock_guard lock(m_mtx);
        m_log.push_back(std::move(r));
    }

    /// Atomically remove and return all collected records.
    [[nodiscard]] std::vector<SimRecord> drain() {
        std::lock_guard lock(m_mtx);
        return std::exchange(m_log, {});
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock(m_mtx);
        return m_log.size();
    }

private:
    mutable std::mutex      m_mtx;
    std::vector<SimRecord>  m_log;
};

// ─── Task description ─────────────────────────────────────────────────────────

struct SimTask {
    uint32_t                  id{0};
    std::string               label;
    std::vector<uint16_t>     instructions;  ///< Thumb-16 opcodes loaded at 0x0000
    uint64_t                  max_steps{100'000};
    uint32_t                  initial_sp{0x8000};
    bool                      checkpoint_every_10k{false};
};

// ─── Per-task result ──────────────────────────────────────────────────────────

struct SimResult {
    uint32_t    sim_id{0};
    std::string label;
    uint64_t    cycles{0};
    bool        completed{false};  ///< true = halted cleanly; false = hit max_steps
    std::string exception_msg;     ///< non-empty on SimulatorError
    std::array<uint32_t, 16> final_regs{};
    std::chrono::nanoseconds wall_time{};
};

// ─── Runner ───────────────────────────────────────────────────────────────────

class SimulationRunner {
public:
    explicit SimulationRunner(
        std::shared_ptr<TraceCollector> trace = nullptr)
        : m_trace{trace ? std::move(trace) : std::make_shared<TraceCollector>()}
    {}

    /// Run all tasks in parallel (one std::jthread per task).
    /// Blocks until all threads finish, then returns results indexed by task order.
    [[nodiscard]] std::vector<SimResult> run(std::span<const SimTask> tasks) {
        const auto n = tasks.size();
        std::vector<SimResult> results(n);
        std::vector<std::jthread> threads;
        threads.reserve(n);

        for (std::size_t i = 0; i < n; ++i) {
            results[i].sim_id = tasks[i].id;
            results[i].label  = tasks[i].label;

            threads.emplace_back(
                [&results, &tasks, i, this]() {
                    run_one(tasks[i], results[i]);
                });
        }
        // jthreads join automatically when destroyed (RAII)
        return results;
    }

    [[nodiscard]] std::shared_ptr<TraceCollector> trace() const noexcept {
        return m_trace;
    }

    /// Total simulations run since construction.
    [[nodiscard]] uint64_t total_run() const noexcept {
        return m_total_run.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<TraceCollector> m_trace;
    std::atomic<uint64_t>           m_total_run{0};

    void run_one(const SimTask& task, SimResult& out) noexcept {
        const auto t0 = std::chrono::steady_clock::now();

        m_trace->push({task.id, task.label, "START", 0, 0});

        FlatMemoryBus mem;
        CortexM0      cpu{mem};
        uint64_t steps_done = 0;

        try {
            // Load instructions at address 0
            uint32_t addr = 0;
            for (uint16_t instr : task.instructions) {
                mem.write16(addr, instr);  // may throw BusFaultException
                addr += 2;
            }

            // Standard Cortex-M0 reset sequence
            cpu.reset();
            cpu.set_reg(RegIndex::PC, 0x0000);
            cpu.set_reg(RegIndex::LR, 0x0000); // LR=0 → BX LR halts
            cpu.set_reg(RegIndex::SP, task.initial_sp);
            if (!task.checkpoint_every_10k) {
                steps_done = cpu.run(task.max_steps);
                out.completed = (steps_done < task.max_steps);
            } else {
                // Manual step-loop for checkpoints
                uint64_t remaining = task.max_steps;
                bool halted = false;
                while (remaining > 0 && !halted) {
                    const uint64_t batch = std::min<uint64_t>(10'000, remaining);
                    const uint64_t done  = cpu.run(batch);
                    steps_done += done;
                    remaining  -= done;
                    if (done < batch) { halted = true; break; }

                    m_trace->push({task.id, task.label, "CHECKPOINT",
                                   cpu.cycle_count(),
                                   cpu.reg(RegIndex::PC)});
                }
                out.completed = halted;
            }
        } catch (const std::exception& e) {
            out.exception_msg = e.what();
            steps_done = cpu.cycle_count();
        }

        // Collect final state
        out.cycles = cpu.cycle_count();
        for (int r = 0; r < 16; ++r)
            out.final_regs[static_cast<std::size_t>(r)] =
                cpu.reg(static_cast<RegIndex>(r));

        out.wall_time = std::chrono::steady_clock::now() - t0;

        m_trace->push({task.id, task.label,
                       out.exception_msg.empty() ? "DONE" : "EXCEPTION",
                       out.cycles, out.final_regs[15]});

        m_total_run.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace mcu
