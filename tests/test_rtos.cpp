// ─────────────────────────────────────────────────────────────────────────────
// tests/test_rtos.cpp — GTests for RTOS kernel + synchronisation primitives
// ─────────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>
#include "../rtos/scheduler.h"
#include "../rtos/mutex.h"
#include "../rtos/semaphore.h"
#include "../rtos/queue.h"
#include "../rtos/event_group.h"
#include <vector>
#include <string>

// ═══════════════════════════════════════════════════════════════════════════════
// Scheduler tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SchedulerTest, CreateSingleTask) {
    rtos::Scheduler s;
    auto h = s.xTaskCreate("t", 1, []{ });
    EXPECT_NE(h, rtos::INVALID_TASK);
    EXPECT_EQ(s.task_count(), 1u);
    EXPECT_EQ(s.get_state(h),    rtos::TaskState::READY);
    EXPECT_EQ(s.get_priority(h), 1u);
}

TEST(SchedulerTest, MaxTasksReturnsInvalid) {
    rtos::Scheduler s;
    for (size_t i = 0; i < rtos::MAX_TASKS; ++i)
        EXPECT_NE(s.xTaskCreate("t", 1, []{ }), rtos::INVALID_TASK);
    EXPECT_EQ(s.xTaskCreate("overflow", 1, []{ }), rtos::INVALID_TASK);
}

TEST(SchedulerTest, RunSingleTask) {
    rtos::Scheduler s;
    int counter = 0;
    s.xTaskCreate("inc", 1, [&]{ ++counter; });
    s.vTaskStartScheduler();
    EXPECT_EQ(counter, 1);
}

TEST(SchedulerTest, PriorityOrderThreeTasks) {
    rtos::Scheduler s;
    std::vector<int> order;
    // Intentionally created out of priority order
    s.xTaskCreate("low",  2, [&]{ order.push_back(2); });
    s.xTaskCreate("high", 0, [&]{ order.push_back(0); });
    s.xTaskCreate("mid",  1, [&]{ order.push_back(1); });
    s.vTaskStartScheduler();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 0);   // highest priority first
    EXPECT_EQ(order[1], 1);
    EXPECT_EQ(order[2], 2);
}

TEST(SchedulerTest, DelayAdvancesTickCount) {
    rtos::Scheduler s;
    uint32_t tick_after_delay = 0;
    s.xTaskCreate("delayed", 1, [&]{
        s.vTaskDelay(100);
        tick_after_delay = s.xTaskGetTickCount();
    });
    s.vTaskStartScheduler();
    EXPECT_EQ(tick_after_delay, 100u);
}

TEST(SchedulerTest, MultipleDelaysOrdered) {
    rtos::Scheduler s;
    std::vector<std::string> log;

    s.xTaskCreate("A", 1, [&]{
        log.push_back("A:start");
        s.vTaskDelay(20);
        log.push_back("A:end");
    });
    s.xTaskCreate("B", 1, [&]{
        log.push_back("B:start");
        s.vTaskDelay(10);
        log.push_back("B:end");
    });
    s.vTaskStartScheduler();

    // A and B both start at tick 0, then B wakes at 10, A at 20
    EXPECT_EQ(log.size(), 4u);
    // First two entries are starts (order may vary by round-robin)
    EXPECT_TRUE(log[0] == "A:start" || log[0] == "B:start");
    EXPECT_TRUE(log[1] == "A:start" || log[1] == "B:start");
    EXPECT_NE(log[0], log[1]);
    // B ends before A (shorter delay)
    auto b_end = std::find(log.begin(), log.end(), "B:end");
    auto a_end = std::find(log.begin(), log.end(), "A:end");
    EXPECT_LT(b_end, a_end);
}

TEST(SchedulerTest, YieldZeroTicks) {
    rtos::Scheduler s;
    std::vector<int> order;
    s.xTaskCreate("A", 1, [&]{
        order.push_back(1);
        s.vTaskDelay(0);   // yield but don't advance time
        order.push_back(3);
    });
    s.xTaskCreate("B", 1, [&]{
        order.push_back(2);
    });
    s.vTaskStartScheduler();
    // A runs first (same priority, created first → round-robin starts at A)
    // A yields, B runs, then A resumes
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(SchedulerTest, SuspendAndResume) {
    rtos::Scheduler s;
    int counter = 0;
    rtos::TaskHandle_t h;
    // Task increments, suspends itself, then would increment again (but won't)
    h = s.xTaskCreate("self-suspend", 1, [&]{
        ++counter;
        s.vTaskSuspend(h);
        ++counter;  // must NOT execute
    });
    s.vTaskStartScheduler();
    EXPECT_EQ(counter, 1);
    EXPECT_EQ(s.get_state(h), rtos::TaskState::SUSPENDED);
}

TEST(SchedulerTest, DeleteTask) {
    rtos::Scheduler s;
    int counter = 0;
    rtos::TaskHandle_t h;
    h = s.xTaskCreate("del", 1, [&]{
        ++counter;
        s.vTaskDelete(h);
        ++counter;   // must NOT execute
    });
    s.vTaskStartScheduler();
    EXPECT_EQ(counter, 1);
    EXPECT_EQ(s.get_state(h), rtos::TaskState::DELETED);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Semaphore tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SemaphoreTest, BinarySignalWait) {
    rtos::Scheduler s;
    rtos::Semaphore sem{ s, 0 };
    int result = 0;

    // Consumer (lower priority) runs second; waits on semaphore
    s.xTaskCreate("consumer", 2, [&]{
        sem.take();
        result = 42;
    });
    // Producer (higher priority) runs first; gives semaphore
    s.xTaskCreate("producer", 1, [&]{
        sem.give();
    });
    s.vTaskStartScheduler();
    EXPECT_EQ(result, 42);
}

TEST(SemaphoreTest, CountingSemaphore) {
    rtos::Scheduler s;
    rtos::Semaphore sem{ s, 3, 5 };
    int taken = 0;
    s.xTaskCreate("taker", 1, [&]{
        while (sem.try_take()) ++taken;
    });
    s.vTaskStartScheduler();
    EXPECT_EQ(taken, 3);
    EXPECT_EQ(sem.count(), 0);
}

TEST(SemaphoreTest, BlocksWhenEmpty) {
    rtos::Scheduler s;
    rtos::Semaphore sem{ s, 0 };
    std::vector<int> order;

    s.xTaskCreate("waiter", 0, [&]{   // high priority — runs first
        order.push_back(1);
        sem.take();                    // blocks immediately
        order.push_back(3);
    });
    s.xTaskCreate("giver", 1, [&]{
        order.push_back(2);
        sem.give();
    });
    s.vTaskStartScheduler();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(SemaphoreTest, GiveMaxCountCaps) {
    rtos::Scheduler s;
    rtos::Semaphore sem{ s, 2, 2 };
    s.xTaskCreate("giver", 1, [&]{ sem.give(); sem.give(); });
    s.vTaskStartScheduler();
    EXPECT_EQ(sem.count(), 2);   // capped at max
}

// ═══════════════════════════════════════════════════════════════════════════════
// Mutex tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MutexTest, BasicLockUnlock) {
    rtos::Scheduler s;
    rtos::Mutex mtx{ s };
    bool entered = false;
    s.xTaskCreate("t", 1, [&]{
        mtx.lock();
        entered = true;
        mtx.unlock();
    });
    s.vTaskStartScheduler();
    EXPECT_TRUE(entered);
    EXPECT_FALSE(mtx.is_locked());
}

TEST(MutexTest, TryLockSucceedsWhenFree) {
    rtos::Scheduler s;
    rtos::Mutex mtx{ s };
    bool ok = false;
    s.xTaskCreate("t", 1, [&]{ ok = mtx.try_lock(); mtx.unlock(); });
    s.vTaskStartScheduler();
    EXPECT_TRUE(ok);
}

TEST(MutexTest, ExclusiveCriticalSection) {
    rtos::Scheduler s;
    rtos::Mutex   mtx{ s };
    std::vector<int> log;

    // Both tasks try to enter a critical section; sections must not interleave
    auto critical = [&](int id){
        mtx.lock();
        log.push_back(id * 10);
        s.vTaskDelay(5);
        log.push_back(id * 10 + 1);
        mtx.unlock();
    };
    s.xTaskCreate("T1", 1, [&]{ critical(1); });
    s.xTaskCreate("T2", 1, [&]{ critical(2); });
    s.vTaskStartScheduler();

    ASSERT_EQ(log.size(), 4u);
    // Pairs (10,11) and (20,21) must appear consecutively
    if (log[0] == 10) {
        EXPECT_EQ(log[1], 11); EXPECT_EQ(log[2], 20); EXPECT_EQ(log[3], 21);
    } else {
        EXPECT_EQ(log[0], 20); EXPECT_EQ(log[1], 21); EXPECT_EQ(log[2], 10); EXPECT_EQ(log[3], 11);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Queue tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(QueueTest, SendReceiveSingle) {
    rtos::Scheduler s;
    rtos::Queue<int, 8> q{ s };
    int received = -1;
    s.xTaskCreate("sender",   1, [&]{ q.send(99); });
    s.xTaskCreate("receiver", 2, [&]{ received = q.receive(); });
    s.vTaskStartScheduler();
    EXPECT_EQ(received, 99);
}

TEST(QueueTest, FifoOrder) {
    rtos::Scheduler s;
    rtos::Queue<int, 8> q{ s };
    std::vector<int> results;
    s.xTaskCreate("sender",   1, [&]{ q.send(1); q.send(2); q.send(3); });
    s.xTaskCreate("receiver", 2, [&]{
        results.push_back(q.receive());
        results.push_back(q.receive());
        results.push_back(q.receive());
    });
    s.vTaskStartScheduler();
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);
    EXPECT_EQ(results[2], 3);
}

TEST(QueueTest, ReceiverBlocksOnEmpty) {
    rtos::Scheduler s;
    rtos::Queue<int, 4> q{ s };
    int value = -1;
    std::vector<int> order;

    s.xTaskCreate("receiver", 0, [&]{
        order.push_back(1);
        value = q.receive();    // blocks (empty)
        order.push_back(3);
    });
    s.xTaskCreate("sender", 1, [&]{
        order.push_back(2);
        q.send(77);
    });
    s.vTaskStartScheduler();
    EXPECT_EQ(value, 77);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1); EXPECT_EQ(order[1], 2); EXPECT_EQ(order[2], 3);
}

TEST(QueueTest, SenderBlocksWhenFull) {
    rtos::Scheduler s;
    rtos::Queue<int, 2> q{ s };   // capacity = 2
    std::vector<int> log;

    s.xTaskCreate("sender", 1, [&]{
        q.send(10);   // slot 0
        q.send(20);   // slot 1
        log.push_back(0); // full — next send blocks
        q.send(30);   // blocks until receiver drains
        log.push_back(1);
    });
    s.xTaskCreate("receiver", 2, [&]{
        s.vTaskDelay(5);         // let sender fill the queue first
        log.push_back(2);
        q.receive();             // drain one slot → unblocks sender
        q.receive();
        q.receive();
    });
    s.vTaskStartScheduler();
    // log[0]=0 (sender full), log[1]=2 (receiver wakes), log[2]=1 (sender finishes)
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], 0);
    EXPECT_EQ(log[1], 2);
    EXPECT_EQ(log[2], 1);
}

TEST(QueueTest, TrySendTryReceive) {
    rtos::Scheduler s;
    rtos::Queue<int, 2> q{ s };
    s.xTaskCreate("t", 1, [&]{
        EXPECT_TRUE(q.try_send(1));
        EXPECT_TRUE(q.try_send(2));
        EXPECT_FALSE(q.try_send(3));   // full
        int v = 0;
        EXPECT_TRUE(q.try_receive(v));  EXPECT_EQ(v, 1);
        EXPECT_TRUE(q.try_receive(v));  EXPECT_EQ(v, 2);
        EXPECT_FALSE(q.try_receive(v)); // empty
    });
    s.vTaskStartScheduler();
}

// ═══════════════════════════════════════════════════════════════════════════════
// EventGroup tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EventGroupTest, SetAndWaitAny) {
    rtos::Scheduler s;
    rtos::EventGroup eg{ s };
    bool woken = false;

    constexpr uint32_t BIT_A = 0x01;
    constexpr uint32_t BIT_B = 0x02;

    s.xTaskCreate("waiter", 1, [&]{
        eg.wait_bits(BIT_A | BIT_B, /*all=*/false);
        woken = true;
    });
    s.xTaskCreate("setter", 2, [&]{
        s.vTaskDelay(10);
        eg.set_bits(BIT_A);   // only BIT_A — enough for ANY
    });
    s.vTaskStartScheduler();
    EXPECT_TRUE(woken);
}

TEST(EventGroupTest, WaitAllRequiresBothBits) {
    rtos::Scheduler s;
    rtos::EventGroup eg{ s };
    std::vector<int> log;

    constexpr uint32_t BIT_A = 0x01;
    constexpr uint32_t BIT_B = 0x02;

    s.xTaskCreate("waiter", 1, [&]{
        eg.wait_bits(BIT_A | BIT_B, /*all=*/true);
        log.push_back(99);
    });
    s.xTaskCreate("setter", 2, [&]{
        s.vTaskDelay(10);
        eg.set_bits(BIT_A);   // not enough yet
        s.vTaskDelay(10);
        eg.set_bits(BIT_B);   // now both set → waiter wakes
    });
    s.vTaskStartScheduler();
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], 99);
}

TEST(EventGroupTest, ClearOnExit) {
    rtos::Scheduler s;
    rtos::EventGroup eg{ s };

    constexpr uint32_t BIT_X = 0x04;
    eg.set_bits(BIT_X); // pre-set

    s.xTaskCreate("t", 1, [&]{
        eg.wait_bits(BIT_X, true, /*clear_on_exit=*/true);
        EXPECT_EQ(eg.get_bits() & BIT_X, 0u);   // cleared
    });
    s.vTaskStartScheduler();
}

TEST(EventGroupTest, CheckBitsNonBlocking) {
    rtos::Scheduler s;
    rtos::EventGroup eg{ s };
    eg.set_bits(0x03);
    s.xTaskCreate("t", 1, [&]{
        EXPECT_TRUE(eg.check_bits(0x01, false)); // ANY
        EXPECT_TRUE(eg.check_bits(0x03, true));  // ALL
        EXPECT_FALSE(eg.check_bits(0x07, true)); // ALL — bit 2 not set
    });
    s.vTaskStartScheduler();
}
