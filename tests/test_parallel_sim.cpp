// MT stress-tests for SimulationRunner (parallel Cortex-M0 instances)
//
// Each test launches N independent CortexM0 CPUs in std::jthread.
// TraceCollector gathers events from all threads safely via std::mutex.
//
// Thumb-16 instruction encoding used in tests:
//   MOV Rn, #imm8 → 0x2000 | (n<<8) | imm8
//   ADD Rn, Rm    → 0x4400 | (n<<3) | m   (hi-reg form, but easier: ALU ADD)
//   NOP           → 0xBF00
//   BX LR (halt)  → 0x4770
//
#include <gtest/gtest.h>

#include "parallel_sim.h"

using namespace mcu;

// Encodes: MOV r0, #val
static constexpr uint16_t MOV_R0(uint8_t val) { return static_cast<uint16_t>(0x2000u | val); }
// Encodes: NOP
static constexpr uint16_t NOP = 0xBF00u;
// Encodes: BX LR  (halts the simulator)
static constexpr uint16_t BX_LR = 0x4770u;

// ─── Helper: build a trivial program that runs N NOPs then halts ──────────────

static SimTask make_nop_task(uint32_t id, const std::string& label,
                              int nops, uint64_t max_steps = 100'000) {
    SimTask t;
    t.id        = id;
    t.label     = std::move(label);
    t.max_steps = max_steps;
    for (int i = 0; i < nops; ++i) t.instructions.push_back(NOP);
    t.instructions.push_back(BX_LR);
    return t;
}

// ─── Test 1: Single task — sanity ─────────────────────────────────────────────

TEST(SimulationRunner, SingleTask_HaltsCleanly) {
    SimulationRunner runner;
    std::vector<SimTask> tasks = { make_nop_task(0, "hello", /*nops=*/5) };
    auto results = runner.run(tasks);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].completed);
    EXPECT_EQ(results[0].sim_id, 0u);
    EXPECT_TRUE(results[0].exception_msg.empty());
    EXPECT_GE(results[0].cycles, 5u);  // at least 5 NOP cycles

    // Trace: START + DONE
    auto events = runner.trace()->drain();
    EXPECT_GE(events.size(), 2u);
}

// ─── Test 2: N parallel independent simulations, all halt correctly ───────────

TEST(SimulationRunner, ParallelTasks_AllComplete) {
    constexpr int N = 8;
    SimulationRunner runner;
    std::vector<SimTask> tasks;
    for (int i = 0; i < N; ++i)
        tasks.push_back(make_nop_task(static_cast<uint32_t>(i),
                                      "sim_" + std::to_string(i),
                                      /*nops=*/i * 100));

    auto results = runner.run(tasks);

    ASSERT_EQ(results.size(), static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(results[static_cast<std::size_t>(i)].completed)
            << "sim " << i << " did not halt cleanly";
        EXPECT_TRUE(results[static_cast<std::size_t>(i)].exception_msg.empty())
            << "sim " << i << " threw: " << results[static_cast<std::size_t>(i)].exception_msg;
    }

    EXPECT_EQ(runner.total_run(), static_cast<uint64_t>(N));
}

// ─── Test 3: TraceCollector — N threads push concurrently, count correct ──────

TEST(TraceCollector, ConcurrentPushes_CountCorrect) {
    constexpr int kThreads = 16;
    constexpr int kRecordsEach = 1'000;

    TraceCollector col;
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&col, t]() {
            for (int i = 0; i < kRecordsEach; ++i)
                col.push({static_cast<uint32_t>(t), "thread", "EVENT",
                          static_cast<uint64_t>(i), 0});
        });
    }
    // jthreads join on destruction
    threads.clear();

    auto drained = col.drain();
    EXPECT_EQ(drained.size(),
              static_cast<std::size_t>(kThreads * kRecordsEach));
    // drain clears the log
    EXPECT_EQ(col.size(), 0u);
}

// ─── Test 4: Simulations with different programs run independently ─────────────

TEST(SimulationRunner, DifferentPrograms_IndependentResults) {
    SimulationRunner runner;

    // sim 0: MOV r0, #10  → r0 should be 10
    SimTask t0;
    t0.id = 0; t0.label = "set_10";
    t0.instructions = { MOV_R0(10), BX_LR };

    // sim 1: MOV r0, #255 → r0 should be 255
    SimTask t1;
    t1.id = 1; t1.label = "set_255";
    t1.instructions = { MOV_R0(255), BX_LR };

    // sim 2: 1000 NOPs
    SimTask t2 = make_nop_task(2, "nop_1000", 1000);

    std::vector<SimTask> tasks = { t0, t1, t2 };
    auto results = runner.run(tasks);

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].final_regs[0], 10u)  << "sim 0: r0 wrong";
    EXPECT_EQ(results[1].final_regs[0], 255u) << "sim 1: r0 wrong";
    EXPECT_GE(results[2].cycles, 1000u)        << "sim 2: cycle count wrong";
    for (auto& r : results) EXPECT_TRUE(r.completed);
}

// ─── Test 5: max_steps limit respected ────────────────────────────────────────

TEST(SimulationRunner, MaxStepsLimit_NotCompleted) {
    SimulationRunner runner;
    // Program with 1000 NOPs but limit to 10 steps
    SimTask task = make_nop_task(0, "limited", /*nops=*/1000, /*max_steps=*/10);
    auto results = runner.run(std::span<const SimTask>(&task, 1));

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].completed);   // stopped by limit, not BX LR
    EXPECT_LE(results[0].cycles, 15u);    // shouldn't overshoot by much
}

// ─── Test 6: Checkpoint events are recorded from parallel threads ──────────────

TEST(SimulationRunner, Checkpoints_RecordedThreadSafely) {
    constexpr int N = 4;
    SimulationRunner runner;
    std::vector<SimTask> tasks;
    for (int i = 0; i < N; ++i) {
        SimTask t = make_nop_task(static_cast<uint32_t>(i),
                                  "ck_" + std::to_string(i),
                                  /*nops=*/50'000,
                                  /*max_steps=*/50'001);
        t.checkpoint_every_10k = true;
        tasks.push_back(std::move(t));
    }
    auto results = runner.run(tasks);
    for (auto& r : results) EXPECT_TRUE(r.completed);

    auto events = runner.trace()->drain();
    // Each sim: 1 START + ~5 CHECKPOINTs + 1 DONE = ~7 events × 4 sims = ~28
    EXPECT_GE(events.size(), static_cast<std::size_t>(N * 3));

    int checkpoints = 0;
    for (auto& e : events)
        if (e.event == "CHECKPOINT") ++checkpoints;
    EXPECT_GE(checkpoints, N);  // at least one checkpoint per sim
}

// ─── Test 7: Wall time is measured per-simulation ─────────────────────────────

TEST(SimulationRunner, WallTime_PerSimulation) {
    SimulationRunner runner;
    // One heavy sim, one trivial sim — heavy should take longer
    SimTask heavy = make_nop_task(0, "heavy", /*nops=*/200'000, 200'001);
    SimTask light = make_nop_task(1, "light", /*nops=*/1);

    std::vector<SimTask> tasks = { heavy, light };
    auto results = runner.run(tasks);

    EXPECT_GT(results[0].wall_time.count(), 0);
    EXPECT_GT(results[1].wall_time.count(), 0);
    // Both measured (not zero)
    EXPECT_GT(results[0].wall_time, std::chrono::nanoseconds{0});
}

// ─── Test 8: Exception captured without killing other threads ──────────────────

TEST(SimulationRunner, Exception_CapturedPerSim) {
    SimulationRunner runner;

    // sim 0: valid program
    SimTask ok = make_nop_task(0, "ok", 5);

    // sim 1: undefined instruction — will throw UndefinedInstructionException
    SimTask bad;
    bad.id = 1; bad.label = "bad";
    bad.instructions = { 0xDEADu }; // garbage opcode
    bad.max_steps = 5;

    std::vector<SimTask> tasks = { ok, bad };
    auto results = runner.run(tasks);

    EXPECT_TRUE(results[0].completed);
    EXPECT_TRUE(results[0].exception_msg.empty());

    // bad sim should have captured an exception
    EXPECT_FALSE(results[1].exception_msg.empty())
        << "Expected exception from bad instruction";
}
