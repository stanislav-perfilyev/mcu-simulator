#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <stdexcept>
#include <string>
#include <span>

// ─── Exceptions ──────────────────────────────────────────────────────────────

struct BusFaultException : std::runtime_error {
    uint32_t address;
    explicit BusFaultException(uint32_t addr)
        : std::runtime_error("Bus fault at 0x" + toHex(addr)), address(addr) {}
    [[nodiscard]] static std::string toHex(uint32_t v);
};

// ─── Memory map constants ─────────────────────────────────────────────────────

namespace MemMap {
    static constexpr uint32_t FLASH_BASE  = 0x0000'0000u;
    static constexpr uint32_t FLASH_SIZE  = 0x0000'8000u; // 32KB
    static constexpr uint32_t SRAM_BASE   = 0x2000'0000u;
    static constexpr uint32_t SRAM_SIZE   = 0x0000'4000u; // 16KB
    static constexpr uint32_t PERIPH_BASE = 0x4000'0000u;
    static constexpr uint32_t PERIPH_SIZE = 0x0000'1000u; // 4KB (stub)
    static constexpr uint32_t SCS_BASE    = 0xE000'0000u;
    static constexpr uint32_t SCS_SIZE    = 0x0001'0000u;

    // Flat 64KB legacy view (used by simulator for simplicity)
    static constexpr uint32_t TOTAL_SIZE  = 0x0001'0000u;
}

// ─── Abstract bus interface ───────────────────────────────────────────────────

class IMemoryBus {
public:
    virtual ~IMemoryBus() = default;

    [[nodiscard]] virtual uint8_t  read8 (uint32_t addr) const = 0;
    [[nodiscard]] virtual uint16_t read16(uint32_t addr) const = 0;
    [[nodiscard]] virtual uint32_t read32(uint32_t addr) const = 0;

    virtual void write8 (uint32_t addr, uint8_t  val) = 0;
    virtual void write16(uint32_t addr, uint16_t val) = 0;
    virtual void write32(uint32_t addr, uint32_t val) = 0;

    // Load raw bytes into memory at given address
    virtual void load(uint32_t addr, const uint8_t* data, size_t len) = 0;

    // Expose raw region for inspection/testing
    [[nodiscard]] virtual std::span<const uint8_t> flash_region() const noexcept = 0;
    [[nodiscard]] virtual std::span<const uint8_t> sram_region()  const noexcept = 0;
};

// ─── Flat 64KB memory bus (simulator model) ──────────────────────────────────

class FlatMemoryBus final : public IMemoryBus {
public:
    static constexpr size_t SIZE = MemMap::TOTAL_SIZE;

    FlatMemoryBus() noexcept;
    ~FlatMemoryBus() override = default;

    FlatMemoryBus(const FlatMemoryBus&)            = delete;
    FlatMemoryBus& operator=(const FlatMemoryBus&) = delete;
    FlatMemoryBus(FlatMemoryBus&&)                 = default;
    FlatMemoryBus& operator=(FlatMemoryBus&&)      = default;

    [[nodiscard]] uint8_t  read8 (uint32_t addr) const override;
    [[nodiscard]] uint16_t read16(uint32_t addr) const override;
    [[nodiscard]] uint32_t read32(uint32_t addr) const override;

    void write8 (uint32_t addr, uint8_t  val) override;
    void write16(uint32_t addr, uint16_t val) override;
    void write32(uint32_t addr, uint32_t val) override;

    void load(uint32_t addr, const uint8_t* data, size_t len) override;

    [[nodiscard]] std::span<const uint8_t> flash_region() const noexcept override;
    [[nodiscard]] std::span<const uint8_t> sram_region()  const noexcept override;

    void clear() noexcept;

private:
    std::array<uint8_t, SIZE> data_{};

    void check_bounds(uint32_t addr, size_t access_size) const;
    [[nodiscard]] uint32_t translate(uint32_t addr) const;
};
