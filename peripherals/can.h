#pragma once
// -----------------------------------------------------------------------------
// peripherals/can.h — CAN 2.0A controller (11-bit ID, 8-byte payload)
//
// Register map (offset from base):
//   0x00  TX_ID   — bits[10:0] = CAN ID to transmit
//   0x04  TX_DLC  — bits[3:0]  = data length code (0-8)
//   0x08  TX_D0   — bytes 0-3 of TX data (big-endian word)
//   0x0C  TX_D1   — bytes 4-7 of TX data
//   0x10  TX_SEND — write any value to trigger transmission
//   0x14  RX_ID   — ID of head frame in RX buffer (read-only)
//   0x18  RX_DLC  — DLC of head frame (read-only)
//   0x1C  RX_D0   — bytes 0-3 of head RX frame
//   0x20  RX_D1   — bytes 4-7 of head RX frame
//   0x24  RX_POP  — write any value to consume head frame
//   0x28  SR      — status register (read-only):
//                     bit0 = TX_OK (last send succeeded)
//                     bit1 = RX_READY (RX buffer non-empty)
//                     bit2 = RX_OVERFLOW (buffer was full, frame lost)
//   0x2C  FILTER_ID   — acceptance filter: ID  (11-bit)
//   0x30  FILTER_MASK — acceptance filter: mask (11-bit); bit=1 means "must match"
// -----------------------------------------------------------------------------
#include "peripheral.h"
#include <array>
#include <cstdint>
#include <cstring>

// ─── CAN frame ────────────────────────────────────────────────────────────────

struct CanFrame {
    uint16_t id  = 0;           // 11-bit CAN ID
    uint8_t  dlc = 0;           // data length code, 0-8
    uint8_t  data[8] = {};
};

// ─── Shared bus (loopback / dual-node testing) ────────────────────────────────

class CanBus {
public:
    static constexpr size_t MAX_NODES = 8;

    void attach(class Can* node) noexcept;
    void detach(class Can* node) noexcept;

    // Broadcast frame to all nodes except sender; returns false if no receivers
    [[nodiscard]] bool broadcast(const CanFrame& frame, const Can* sender) noexcept;

private:
    std::array<Can*, MAX_NODES> nodes_ {};
    size_t count_ = 0;
};

// ─── CAN controller ───────────────────────────────────────────────────────────

class Can final : public IPeripheral {
public:
    static constexpr size_t RX_CAPACITY = 32;

    static constexpr uint32_t REG_TX_ID       = 0x00;
    static constexpr uint32_t REG_TX_DLC      = 0x04;
    static constexpr uint32_t REG_TX_D0       = 0x08;
    static constexpr uint32_t REG_TX_D1       = 0x0C;
    static constexpr uint32_t REG_TX_SEND     = 0x10;
    static constexpr uint32_t REG_RX_ID       = 0x14;
    static constexpr uint32_t REG_RX_DLC      = 0x18;
    static constexpr uint32_t REG_RX_D0       = 0x1C;
    static constexpr uint32_t REG_RX_D1       = 0x20;
    static constexpr uint32_t REG_RX_POP      = 0x24;
    static constexpr uint32_t REG_SR          = 0x28;
    static constexpr uint32_t REG_FILTER_ID   = 0x2C;
    static constexpr uint32_t REG_FILTER_MASK = 0x30;

    static constexpr uint32_t SR_TX_OK       = (1u << 0);
    static constexpr uint32_t SR_RX_READY    = (1u << 1);
    static constexpr uint32_t SR_RX_OVERFLOW = (1u << 2);

    explicit Can(const char* n = "CAN0", CanBus* bus = nullptr) noexcept;
    ~Can() noexcept;

    Can(const Can&)            = delete;
    Can& operator=(const Can&) = delete;

    [[nodiscard]] const char* name()      const noexcept override { return name_; }
    [[nodiscard]] uint32_t    read_reg (uint32_t offset) const override;
    void                      write_reg(uint32_t offset, uint32_t val) override;

    // Called by CanBus to inject a received frame (after filter check)
    [[nodiscard]] bool inject(const CanFrame& frame) noexcept;

    // Direct helpers for tests
    [[nodiscard]] bool        rx_ready()  const noexcept { return rx_count_ > 0; }
    [[nodiscard]] size_t      rx_count()  const noexcept { return rx_count_; }
    [[nodiscard]] CanFrame    rx_peek()   const noexcept;

private:
    const char* name_;
    CanBus*     bus_;

    // TX staging registers
    CanFrame    tx_frame_ {};

    // RX ring buffer
    std::array<CanFrame, RX_CAPACITY> rx_buf_ {};
    size_t rx_head_  = 0;
    size_t rx_tail_  = 0;
    size_t rx_count_ = 0;

    // Status
    uint32_t sr_ = 0;

    // Acceptance filter
    uint16_t filter_id_   = 0x000u;
    uint16_t filter_mask_ = 0x000u; // mask=0 → accept all

    [[nodiscard]] bool passes_filter(uint16_t id) const noexcept {
        return (id & filter_mask_) == (filter_id_ & filter_mask_);
    }

    [[nodiscard]] bool rx_push(const CanFrame& f) noexcept;
    CanFrame rx_pop() noexcept;
};
