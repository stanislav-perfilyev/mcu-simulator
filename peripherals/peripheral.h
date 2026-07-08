#pragma once
// -----------------------------------------------------------------------------
// peripherals/peripheral.h — Base peripheral interface + address dispatcher
// Session 3: MCU Simulator — Peripheral Bus (UART / SPI / I2C / CAN)
// -----------------------------------------------------------------------------
#include <cstdint>
#include <array>
#include <stdexcept>
#include <string>

// ─── Exceptions ──────────────────────────────────────────────────────────────

/// Base exception for all peripheral access errors.
struct PeripheralError : std::runtime_error {
    explicit PeripheralError(const std::string& msg)
        : std::runtime_error("Peripheral error: " + msg) {}
};

/// Thrown when an access falls outside a peripheral's mapped address range.
struct AddressError : PeripheralError {
    uint32_t address;
    explicit AddressError(uint32_t addr)
        : PeripheralError("unmapped address 0x" + to_hex(addr)), address(addr) {}
    [[nodiscard]] static std::string to_hex(uint32_t v) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%08X", v);
        return buf;
    }
};

// ─── Peripheral memory map ───────────────────────────────────────────────────

namespace PeriphMap {
    static constexpr uint32_t UART0_BASE = 0x4000'0000u;
    static constexpr uint32_t UART1_BASE = 0x4000'1000u;
    static constexpr uint32_t SPI0_BASE  = 0x4000'2000u;
    static constexpr uint32_t I2C0_BASE  = 0x4000'3000u;
    static constexpr uint32_t CAN0_BASE  = 0x4000'4000u;
    static constexpr uint32_t CAN1_BASE  = 0x4000'5000u;
    static constexpr uint32_t REGION_SIZE= 0x0000'1000u; // 4KB per peripheral
}

// ─── Abstract peripheral interface ───────────────────────────────────────────

/// Abstract interface for a memory-mapped peripheral.
class IPeripheral {
public:
    virtual ~IPeripheral() = default;

    [[nodiscard]] virtual uint32_t read_reg (uint32_t offset) const = 0;
    virtual void                   write_reg(uint32_t offset, uint32_t val) = 0;
    [[nodiscard]] virtual const char* name  () const noexcept = 0;
};

// ─── Peripheral bus dispatcher ────────────────────────────────────────────────
// Maps 32-bit addresses to registered IPeripheral instances.

/// Address-decode bus: dispatches memory reads/writes to registered peripherals.
class PeripheralBus {
public:
    static constexpr size_t MAX_PERIPHERALS = 8;

    /// Maps one peripheral to its base address and byte-size window.
    struct Entry {
        uint32_t     base    = 0;
        uint32_t     size    = PeriphMap::REGION_SIZE;
        IPeripheral* periph  = nullptr;
    };

    // Register peripheral at given base address
    void attach(uint32_t base, IPeripheral* p,
                uint32_t size = PeriphMap::REGION_SIZE) {
        if (count_ >= MAX_PERIPHERALS)
            throw PeripheralError("too many peripherals");
        entries_[count_++] = {base, size, p};
    }

    // Read 32-bit register; throws AddressError if unmapped
    [[nodiscard]] uint32_t read(uint32_t addr) const {
        const auto* e = find(addr);
        if (!e) throw AddressError(addr);
        return e->periph->read_reg(addr - e->base);
    }

    // Write 32-bit register; throws AddressError if unmapped
    void write(uint32_t addr, uint32_t val) {
        auto* e = find(addr);
        if (!e) throw AddressError(addr);
        e->periph->write_reg(addr - e->base, val);
    }

    [[nodiscard]] bool is_mapped(uint32_t addr) const noexcept {
        return find(addr) != nullptr;
    }

    [[nodiscard]] size_t peripheral_count() const noexcept { return count_; }

private:
    std::array<Entry, MAX_PERIPHERALS> entries_{};
    size_t count_ = 0;

    [[nodiscard]] const Entry* find(uint32_t addr) const noexcept {
        for (size_t i = 0; i < count_; ++i) {
            const auto& e = entries_[i];
            if (addr >= e.base && addr < e.base + e.size)
                return &e;
        }
        return nullptr;
    }
    [[nodiscard]] Entry* find(uint32_t addr) noexcept {
        return const_cast<Entry*>(
            static_cast<const PeripheralBus*>(this)->find(addr));
    }
};
