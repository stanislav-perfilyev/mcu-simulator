#pragma once
// -----------------------------------------------------------------------------
// peripherals/uart.h — UART peripheral simulation
//
// Register map (32-bit word access, offset from base):
//   0x00  DR  — Data Register: write=push to TX FIFO, read=pop from RX FIFO
//   0x04  SR  — Status Register (read-only):
//                 bit0 TX_EMPTY   — TX FIFO has room
//                 bit1 RX_READY   — RX FIFO has data
//                 bit2 RX_FULL    — RX FIFO full (16 bytes)
//                 bit3 TX_FULL    — TX FIFO full
//                 bit4 RX_IRQ     — RX byte count >= irq_threshold
//   0x08  CR  — Control Register:
//                 bit0 ENABLE     — UART enabled
//                 bit1 RX_IRQ_EN  — enable RX interrupt flag
//                 bit2 LOOPBACK   — TX bytes immediately appear in RX
//   0x0C  IRQ_THR — RX interrupt threshold (1–16, default 1)
//   0x10  FIFO_LVL — bits[7:0]=rx_count, bits[15:8]=tx_count (read-only)
// -----------------------------------------------------------------------------
#include "peripheral.h"
#include <array>
#include <cstdint>
#include <cstring>

/// Memory-mapped UART peripheral: TX FIFO, RX loopback, configurable baud rate.
class Uart final : public IPeripheral {
public:
    static constexpr size_t   FIFO_SIZE = 256;

    // Register offsets
    static constexpr uint32_t REG_DR       = 0x00;
    static constexpr uint32_t REG_SR       = 0x04;
    static constexpr uint32_t REG_CR       = 0x08;
    static constexpr uint32_t REG_IRQ_THR  = 0x0C;
    static constexpr uint32_t REG_FIFO_LVL = 0x10;

    // SR bits
    static constexpr uint32_t SR_TX_EMPTY = (1u << 0);
    static constexpr uint32_t SR_RX_READY = (1u << 1);
    static constexpr uint32_t SR_RX_FULL  = (1u << 2);
    static constexpr uint32_t SR_TX_FULL  = (1u << 3);
    static constexpr uint32_t SR_RX_IRQ   = (1u << 4);

    // CR bits
    static constexpr uint32_t CR_ENABLE    = (1u << 0);
    static constexpr uint32_t CR_RX_IRQ_EN = (1u << 1);
    static constexpr uint32_t CR_LOOPBACK  = (1u << 2);

    explicit Uart(const char* n = "UART") noexcept : name_(n) {
        cr_  = CR_ENABLE;
        irq_thr_ = 1;
    }

    Uart(const Uart&)            = delete;
    Uart& operator=(const Uart&) = delete;

    [[nodiscard]] const char* name() const noexcept override { return name_; }

    // ── IPeripheral ──────────────────────────────────────────────────────────
    [[nodiscard]] uint32_t read_reg(uint32_t offset) const override;
    void write_reg(uint32_t offset, uint32_t val) override;

    // ── Direct injection (for tests / inter-peripheral wiring) ───────────────
    // Inject byte into RX FIFO (simulates data received from the line)
    [[nodiscard]] bool inject_rx(uint8_t byte) noexcept;

    // Pop byte from TX FIFO (simulates data sent to the line)
    [[nodiscard]] bool consume_tx(uint8_t& out) noexcept;

    // Drain entire TX FIFO into a string (useful for tests)
    [[nodiscard]] std::string drain_tx() noexcept;

    [[nodiscard]] size_t rx_count() const noexcept { return rx_count_; }
    [[nodiscard]] size_t tx_count() const noexcept { return tx_count_; }
    [[nodiscard]] bool   rx_irq_pending() const noexcept;

    void reset() noexcept;

private:
    const char* name_;

    // FIFOs implemented as circular arrays
    std::array<uint8_t, FIFO_SIZE> tx_fifo_{};
    mutable std::array<uint8_t, FIFO_SIZE> rx_fifo_{};
    size_t tx_head_ = 0, tx_tail_ = 0, tx_count_ = 0;
    mutable size_t rx_head_ = 0, rx_tail_ = 0, rx_count_ = 0;

    uint32_t cr_      = CR_ENABLE;
    uint32_t irq_thr_ = 1;

    [[nodiscard]] uint32_t build_sr() const noexcept;

    [[nodiscard]] bool tx_push(uint8_t b) noexcept;
    [[nodiscard]] bool tx_pop (uint8_t& b) noexcept;
    [[nodiscard]] bool rx_push(uint8_t b) noexcept;
    [[nodiscard]] bool rx_pop (uint8_t& b) const noexcept;
};
