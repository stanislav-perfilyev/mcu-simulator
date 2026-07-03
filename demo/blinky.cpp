// -----------------------------------------------------------------------------
// demo/blinky.cpp -- FreeRTOS-style demo: producer / consumer / watchdog
//
// Three tasks communicate over a Queue. A Semaphore signals completion.
// The Watchdog monitors liveness via an EventGroup.
//
// Time is simulated: the scheduler advances the tick counter automatically
// when all tasks are blocked, so 1 000 ticks of delay run in microseconds.
// -----------------------------------------------------------------------------
#include "../rtos/scheduler.h"
#include "../rtos/semaphore.h"
#include "../rtos/queue.h"
#include "../rtos/event_group.h"
#include <iostream>
#include <iomanip>

namespace {

rtos::Scheduler      sched;
rtos::Queue<int, 8>  item_queue { sched };
rtos::Semaphore      done_sem   { sched, 0 };
rtos::EventGroup     liveness   { sched };

constexpr uint32_t BIT_PRODUCED = 0x01;
constexpr uint32_t BIT_CONSUMED = 0x02;
constexpr int      TOTAL_ITEMS  = 10;

int g_produced = 0;
int g_consumed = 0;

// -- Producer task (priority 1) -----------------------------------------------
void producer_task() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        item_queue.send(i);
        ++g_produced;
        liveness.set_bits(BIT_PRODUCED);
        std::cout << "[" << std::setw(4) << sched.xTaskGetTickCount()
                  << "ms] PROD item=" << i << "\n";
        sched.vTaskDelay(10);
    }
    done_sem.give();
}

// -- Consumer task (priority 1) -----------------------------------------------
void consumer_task() {
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        int item = item_queue.receive();
        ++g_consumed;
        liveness.set_bits(BIT_CONSUMED);
        std::cout << "[" << std::setw(4) << sched.xTaskGetTickCount()
                  << "ms] CONS item=" << item << "\n";
        sched.vTaskDelay(5);
    }
}

// -- Watchdog task (priority 0 = highest) -------------------------------------
void watchdog_task() {
    constexpr uint32_t TIMEOUT  = 500;
    constexpr uint32_t INTERVAL = 100;

    while (g_produced < TOTAL_ITEMS || g_consumed < TOTAL_ITEMS) {
        uint32_t before = sched.xTaskGetTickCount();
        sched.vTaskDelay(INTERVAL);
        uint32_t after  = sched.xTaskGetTickCount();

        bool ok = liveness.check_bits(BIT_PRODUCED | BIT_CONSUMED, false);
        if (!ok && (after - before) >= TIMEOUT) {
            std::cerr << "[WATCHDOG] No activity for " << TIMEOUT << " ms -- system stuck!\n";
            return;
        }
        liveness.clear_bits(BIT_PRODUCED | BIT_CONSUMED);
    }
    std::cout << "[WATCHDOG] All tasks healthy -- done.\n";
}

} // namespace

int main() {
    std::cout << "=== MCU Simulator RTOS Demo ===\n\n";

    (void)sched.xTaskCreate("watchdog", 0, watchdog_task);
    (void)sched.xTaskCreate("producer", 1, producer_task);
    (void)sched.xTaskCreate("consumer", 1, consumer_task);

    sched.vTaskStartScheduler();

    std::cout << "\n=== Results ===\n"
              << "  Items produced : " << g_produced << " / " << TOTAL_ITEMS << "\n"
              << "  Items consumed : " << g_consumed << " / " << TOTAL_ITEMS << "\n"
              << "  Sim time       : " << sched.xTaskGetTickCount() << " ms\n";

    return (g_produced == TOTAL_ITEMS && g_consumed == TOTAL_ITEMS) ? 0 : 1;
}
