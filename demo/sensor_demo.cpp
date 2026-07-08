// -----------------------------------------------------------------------------
// demo/sensor_demo.cpp — RTOS sensor pipeline demo
//
// Three tasks communicate over an RTOS Queue:
//   Task 1 (priority 2): I2C reader — polls LM75 temperature sensor every 500ms
//   Task 2 (priority 1): CAN forwarder — receives temp readings, transmits CAN frames
//   Task 3 (priority 2): UART logger  — receives same readings, logs ASCII to UART
//
// PeripheralBus wires all peripherals at their MemMap addresses.
// The RTOS queue passes TempReading structs between tasks.
// -----------------------------------------------------------------------------
#include "rtos/scheduler.h"
#include "rtos/queue.h"
#include "peripherals/peripheral.h"
#include "peripherals/uart.h"
#include "peripherals/spi.h"
#include "peripherals/i2c.h"
#include "peripherals/can.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace rtos;

// ─── Constants ────────────────────────────────────────────────────────────────
static constexpr size_t   BUF_SIZE         = 32;      ///< snprintf buffer for formatted temperature
static constexpr uint32_t POLL_DELAY_TICKS = 500;     ///< I2C poll period in RTOS ticks (ms)
static constexpr float    SENSOR_ERROR     = -999.0f; ///< Sentinel value for sensor read failure
static constexpr int      TEMP_SCALE_BASE  = 250;     ///< Base raw temp (250 = 25.0 °C × 10)

// ─── Shared data types ────────────────────────────────────────────────────────

struct TempReading {
    uint32_t tick;
    int16_t  raw;   // LM75 raw 9-bit (MSB=integer °C, bit0=0.5°C)
    float    celsius;
};

// ─── Globals ──────────────────────────────────────────────────────────────────

static Scheduler        sched;
static Queue<TempReading, 8> to_can{sched};
static Queue<TempReading, 8> to_uart{sched};

// Peripheral instances
static CanBus           can_bus;
static Uart             uart0("UART0");
static Spi              spi0("SPI0");
static I2c              i2c0("I2C0");
static Can              can0("CAN0", &can_bus);
static Can              can1("CAN1", &can_bus);  // "remote" receiver

// Peripheral bus
static PeripheralBus    periph_bus;

// ─── Helper: format float without printf %f (embedded-friendly) ──────────────

static std::string fmt_temp(float t) {
    int integer = static_cast<int>(t);
    int frac    = static_cast<int>((t - static_cast<float>(integer)) * 10.0f);
    if (frac < 0) frac = -frac;
    char buf[BUF_SIZE];
    std::snprintf(buf, sizeof(buf), "%d.%d", integer, frac);
    return buf;
}

// ─── Task 1: I2C temperature reader ──────────────────────────────────────────

static void task_i2c_reader() {
    // Simulate 10 readings at 500-ms intervals (driven by tick)
    for (int reading = 0; reading < 10; ++reading) {
        sched.vTaskDelay(POLL_DELAY_TICKS);

        // Read temperature via PeripheralBus (I2C0 at 0x4000_3000)
        periph_bus.write(PeriphMap::I2C0_BASE + I2c::REG_REG_ADDR, Lm75::REG_TEMP);
        uint32_t cr = Lm75::DEFAULT_ADDR | I2c::CR_READ | I2c::CR_START;
        periph_bus.write(PeriphMap::I2C0_BASE + I2c::REG_CR, cr);

        uint32_t sr = periph_bus.read(PeriphMap::I2C0_BASE + I2c::REG_SR);
        if (!(sr & I2c::SR_ACK_OK)) {
            // Sensor not responding — post sentinel
            TempReading err{ sched.xTaskGetTickCount(), -1, SENSOR_ERROR };
            to_can.send(err);
            to_uart.send(err);
            continue;
        }

        uint8_t msb = static_cast<uint8_t>(
            periph_bus.read(PeriphMap::I2C0_BASE + I2c::REG_DR));
        uint8_t lsb = static_cast<uint8_t>(
            periph_bus.read(PeriphMap::I2C0_BASE + I2c::REG_DR));

        float celsius = static_cast<float>(static_cast<int8_t>(msb));
        if (lsb & 0x80) celsius += 0.5f;

        int16_t raw = (static_cast<int16_t>(msb) << 1) | ((lsb & 0x80) ? 1 : 0);

        TempReading r{ sched.xTaskGetTickCount(), raw, celsius };

        // Simulate temperature drift: +0.5°C per reading (for demo variety)
        i2c0.sensor().set_temperature_c10(
            static_cast<int>(TEMP_SCALE_BASE + reading * 5)); // +0.5 °C per reading

        to_can.send(r);
        to_uart.send(r);
    }
}

// ─── Task 2: CAN forwarder ────────────────────────────────────────────────────

static void task_can_forwarder() {
    // CAN frame format (custom, 8 bytes):
    //   bytes 0-1: tick (big-endian)
    //   bytes 2-3: raw temp (big-endian)
    //   bytes 4-7: reserved 0x00
    static constexpr uint16_t TEMP_CAN_ID = 0x1B0u; // "temperature" frame

    for (;;) {
        TempReading r = to_can.receive();

        // Pack into CAN frame words
        uint32_t d0 = (static_cast<uint32_t>(r.tick & 0xFFFFu) << 16) |
                      (static_cast<uint32_t>(static_cast<uint16_t>(r.raw)));
        uint32_t d1 = 0;

        periph_bus.write(PeriphMap::CAN0_BASE + Can::REG_TX_ID,  TEMP_CAN_ID);
        periph_bus.write(PeriphMap::CAN0_BASE + Can::REG_TX_DLC, 4u);
        periph_bus.write(PeriphMap::CAN0_BASE + Can::REG_TX_D0,  d0);
        periph_bus.write(PeriphMap::CAN0_BASE + Can::REG_TX_D1,  d1);
        periph_bus.write(PeriphMap::CAN0_BASE + Can::REG_TX_SEND, 1u);

        uint32_t sr = periph_bus.read(PeriphMap::CAN0_BASE + Can::REG_SR);
        (void)sr; // TX_OK always set in simulation
    }
}

// ─── Task 3: UART logger ──────────────────────────────────────────────────────

static void task_uart_logger() {
    int count = 0;
    for (;;) {
        TempReading r = to_uart.receive();
        ++count;

        std::string line = "[UART] tick=" + std::to_string(r.tick) +
                           " temp=" + fmt_temp(r.celsius) + "C\n";

        // Write each byte via PeripheralBus (UART0 at 0x4000_0000)
        for (char c : line) {
            periph_bus.write(PeriphMap::UART0_BASE + Uart::REG_DR,
                             static_cast<uint32_t>(c));
        }

        if (count == 10) break; // Stop after 10 readings
    }
}

// ─── Entry point ─────────────────────────────────────────────────────────────

int main() {
    // --- Wire up peripheral bus ---
    periph_bus.attach(PeriphMap::UART0_BASE, &uart0);
    periph_bus.attach(PeriphMap::SPI0_BASE,  &spi0);
    periph_bus.attach(PeriphMap::I2C0_BASE,  &i2c0);
    periph_bus.attach(PeriphMap::CAN0_BASE,  &can0);
    periph_bus.attach(PeriphMap::CAN1_BASE,  &can1);

    // Enable UART
    periph_bus.write(PeriphMap::UART0_BASE + Uart::REG_CR,
                     Uart::CR_ENABLE);

    // Print banner
    const char* banner = "=== MCU Sensor Pipeline Demo ===\n";
    for (const char* p = banner; *p; ++p)
        uart0.write_reg(Uart::REG_DR, static_cast<uint32_t>(*p));

    // --- Create RTOS tasks ---
    (void)sched.xTaskCreate("i2c_reader",    2, task_i2c_reader);
    (void)sched.xTaskCreate("can_forwarder", 1, task_can_forwarder);
    (void)sched.xTaskCreate("uart_logger",   2, task_uart_logger);

    // --- Run until all tasks complete ---
    // We inject ticks externally to drive the scheduler deterministically.
    // In a real MCU this would be a SysTick ISR.
    sched.vTaskStartScheduler();

    // --- Dump UART TX buffer to stdout ---
    std::string output = uart0.drain_tx();
    std::printf("%s", output.c_str());

    // --- Show CAN receive side (what can1 received) ---
    std::printf("\n--- CAN1 received %zu frame(s) ---\n", can1.rx_count());
    while (can1.rx_ready()) {
        CanFrame f = can1.rx_peek();
        can1.write_reg(Can::REG_RX_POP, 1u);
        // Data was packed as: d0 = (tick & 0xFFFF) << 16 | raw_u16  (native-endian uint32_t)
        uint32_t d0 = 0;
        std::memcpy(&d0, f.data, 4);
        uint16_t tick_lo = static_cast<uint16_t>((d0 >> 16) & 0xFFFFu);
        int16_t  raw     = static_cast<int16_t>(d0 & 0xFFFFu);
        float    t       = static_cast<float>(raw >> 1) + ((raw & 1) ? 0.5f : 0.0f);
        std::printf("  CAN ID=0x%03X DLC=%u tick=%u temp=%.1f C\n",
                    f.id, f.dlc, tick_lo, static_cast<double>(t));
    }

    return 0;
}
