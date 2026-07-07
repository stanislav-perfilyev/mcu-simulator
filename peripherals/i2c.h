#pragma once
// -----------------------------------------------------------------------------
// peripherals/i2c.h — I2C master + simulated LM75 temperature sensor
//
// Register map (offset from base):
//   0x00  CR  — Control: write a transaction command word
//                 bits[7:0]  = slave_addr (7-bit)
//                 bit8       = RW (0=write, 1=read)
//                 bit9       = START (generate START condition)
//                 bit10      = STOP  (generate STOP after transfer)
//   0x04  SR  — Status (read-only):
//                 bit0 = BUSY
//                 bit1 = ACK_OK  (last transfer acknowledged)
//                 bit2 = NACK   (address not found)
//                 bit3 = RX_READY
//   0x08  DR  — Data: write=TX byte, read=pop RX byte
//   0x0C  REG_ADDR — slave register address (for combined write-then-read)
//
// LM75 simulated at address 0x48:
//   Reg 0x00  Temp  (16-bit, big-endian): default 0x1900 = 25.0°C
//   Reg 0x01  Conf  (8-bit): default 0x00
//   Reg 0x02  Tos   (16-bit): default 0x5000 = 80°C
//   Reg 0x03  Thyst (16-bit): default 0x4B00 = 75°C
// -----------------------------------------------------------------------------
#include "peripheral.h"
#include <array>
#include <cstdint>
#include <queue>

// ─── LM75 temperature sensor simulation ──────────────────────────────────────

class Lm75 {
public:
    static constexpr uint8_t DEFAULT_ADDR = 0x48;

    static constexpr uint8_t REG_TEMP  = 0x00;
    static constexpr uint8_t REG_CONF  = 0x01;
    static constexpr uint8_t REG_TOS   = 0x02;
    static constexpr uint8_t REG_THYST = 0x03;

    Lm75() noexcept {
        // 25.0°C = 0x19_00 in 9-bit two-complement, MSB first
        regs_[REG_TEMP*2]   = 0x19; regs_[REG_TEMP*2+1]   = 0x00;
        regs_[REG_CONF*2]   = 0x00; regs_[REG_CONF*2+1]   = 0x00;
        regs_[REG_TOS*2]    = 0x50; regs_[REG_TOS*2+1]    = 0x00;
        regs_[REG_THYST*2]  = 0x4B; regs_[REG_THYST*2+1]  = 0x00;
    }

    // Read two bytes starting at reg (MSB first)
    [[nodiscard]] uint16_t read_reg16(uint8_t reg) const noexcept {
        if (reg > 3) return 0;
        size_t base = static_cast<size_t>(reg) * 2u;
        return (static_cast<uint16_t>(regs_[base]) << 8) | regs_[base + 1u];
    }

    void write_reg16(uint8_t reg, uint16_t val) noexcept {
        if (reg > 3) return;
        size_t base = static_cast<size_t>(reg) * 2u;
        regs_[base]     = static_cast<uint8_t>(val >> 8);
        regs_[base + 1u] = static_cast<uint8_t>(val & 0xFF);
    }

    // Set temperature in 1/256 °C units (LM75 uses 1/2 °C LSB, 9-bit)
    void set_temperature_raw(int16_t raw_9bit) noexcept {
        // raw_9bit: bit8=sign, bits[7:1]=integer, bit0=0.5°C
        // Store in upper 9 bits of 16-bit register
        regs_[0] = static_cast<uint8_t>((raw_9bit >> 1) & 0xFF);
        regs_[1] = static_cast<uint8_t>((raw_9bit & 1) ? 0x80 : 0x00);
    }

    // Helper: set temperature in tenths of Celsius (e.g. 255 = 25.5°C)
    void set_temperature_c10(int temp_c10) noexcept {
        // LM75: MSB = integer °C, LSB bit7 = 0.5°C flag
        int integer_part = temp_c10 / 10;
        bool half        = (temp_c10 % 10) >= 5;
        regs_[0] = static_cast<uint8_t>(static_cast<int8_t>(integer_part));
        regs_[1] = half ? 0x80u : 0x00u;
    }

    [[nodiscard]] float temperature_c() const noexcept {
        int16_t msb = static_cast<int8_t>(regs_[0]);
        float f = static_cast<float>(msb);
        if (regs_[1] & 0x80) f += 0.5f;
        return f;
    }

private:
    std::array<uint8_t, 8> regs_{}; // 4 regs × 2 bytes each
};

// ─── I2C peripheral ───────────────────────────────────────────────────────────

class I2c final : public IPeripheral {
public:
    static constexpr uint32_t REG_CR       = 0x00;
    static constexpr uint32_t REG_SR       = 0x04;
    static constexpr uint32_t REG_DR       = 0x08;
    static constexpr uint32_t REG_REG_ADDR = 0x0C;

    // CR bits
    static constexpr uint32_t CR_READ  = (1u << 8);
    static constexpr uint32_t CR_START = (1u << 9);
    static constexpr uint32_t CR_STOP  = (1u << 10);

    // SR bits
    static constexpr uint32_t SR_BUSY      = (1u << 0);
    static constexpr uint32_t SR_ACK_OK    = (1u << 1);
    static constexpr uint32_t SR_NACK      = (1u << 2);
    static constexpr uint32_t SR_RX_READY  = (1u << 3);

    explicit I2c(const char* n = "I2C0") noexcept : name_(n) {}
    I2c(const I2c&) = delete;
    I2c& operator=(const I2c&) = delete;

    [[nodiscard]] const char*  name()      const noexcept override { return name_; }
    [[nodiscard]] uint32_t     read_reg (uint32_t offset) const override;
    void                       write_reg(uint32_t offset, uint32_t val) override;

    // Direct access to the simulated sensor (for tests)
    [[nodiscard]] Lm75& sensor() noexcept { return lm75_; }

    // Perform a combined write+read (write reg_addr, then read N bytes)
    // Returns number of bytes placed in rx_queue_
    [[nodiscard]] size_t read_sensor_reg(uint8_t slave_addr,
                                         uint8_t reg, size_t n_bytes) noexcept;

    void write_sensor_reg(uint8_t slave_addr,
                          uint8_t reg, uint16_t value) noexcept;

private:
    const char*  name_;
    mutable uint32_t sr_  = 0;
    uint8_t      reg_addr_= 0;

    mutable std::queue<uint8_t> rx_queue_;
    Lm75 lm75_;

    [[nodiscard]] bool dispatch_read(uint8_t slave, uint8_t reg) noexcept;
    [[nodiscard]] bool dispatch_write(uint8_t slave, uint8_t reg, uint8_t* data, size_t len) noexcept;
};
