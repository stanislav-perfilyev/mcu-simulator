// -----------------------------------------------------------------------------
// peripherals/i2c.cpp — I2C master controller implementation
// -----------------------------------------------------------------------------
#include "i2c.h"
#include <cassert>
#include <cstddef>

// ─── Register dispatch ────────────────────────────────────────────────────────

uint32_t I2c::read_reg(uint32_t offset) const {
    switch (offset) {
        case REG_SR: return sr_;
        case REG_DR: {
            if (rx_queue_.empty()) return 0xFFu;
            // rx_queue_ and sr_ are mutable — logically const read with side-effects
            uint8_t byte = rx_queue_.front();
            rx_queue_.pop();
            if (rx_queue_.empty())
                sr_ &= ~SR_RX_READY;
            return byte;
        }
        case REG_REG_ADDR: return reg_addr_;
        default: return 0;
    }
}

void I2c::write_reg(uint32_t offset, uint32_t val) {
    switch (offset) {
        case REG_CR: {
            const uint8_t  slave = static_cast<uint8_t>(val & 0x7Fu);
            const bool     read  = (val & CR_READ) != 0;
            const bool     start = (val & CR_START) != 0;

            if (!start) break; // nothing to do without START

            sr_ = SR_BUSY;

            if (read) {
                if (dispatch_read(slave, reg_addr_)) {
                    sr_ = SR_ACK_OK | SR_RX_READY;
                } else {
                    sr_ = SR_NACK;
                }
            } else {
                // For write: val[31:16] = 16-bit data, val[15:8] = reg_addr
                // (simple protocol: CR write with data embedded when STOP set)
                const bool     stop    = (val & CR_STOP) != 0;
                const uint8_t  reg     = static_cast<uint8_t>((val >> 8) & 0xFFu);
                const uint16_t data16  = static_cast<uint16_t>((val >> 16) & 0xFFFFu);
                reg_addr_ = reg;
                if (stop) {
                    uint8_t buf[2] = {
                        static_cast<uint8_t>(data16 >> 8),
                        static_cast<uint8_t>(data16 & 0xFF)
                    };
                    if (dispatch_write(slave, reg, buf, 2)) {
                        sr_ = SR_ACK_OK;
                    } else {
                        sr_ = SR_NACK;
                    }
                } else {
                    sr_ = SR_ACK_OK; // partial write, reg_addr latched
                }
            }
            break;
        }
        case REG_REG_ADDR:
            reg_addr_ = static_cast<uint8_t>(val & 0xFF);
            break;
        case REG_DR:
            // raw TX byte — not used in this register-map model
            break;
        default:
            break;
    }
}

// ─── Sensor dispatch ─────────────────────────────────────────────────────────

bool I2c::dispatch_read(uint8_t slave, uint8_t reg) noexcept {
    if (slave != Lm75::DEFAULT_ADDR) return false;

    // Clear old data
    while (!rx_queue_.empty()) rx_queue_.pop();

    uint16_t word = lm75_.read_reg16(reg);
    rx_queue_.push(static_cast<uint8_t>(word >> 8));
    rx_queue_.push(static_cast<uint8_t>(word & 0xFF));
    return true;
}

bool I2c::dispatch_write(uint8_t slave, uint8_t reg, uint8_t* data, size_t len) noexcept {
    if (slave != Lm75::DEFAULT_ADDR) return false;
    if (len < 2) return false;
    uint16_t val = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    lm75_.write_reg16(reg, val);
    return true;
}

// ─── High-level helpers ───────────────────────────────────────────────────────

size_t I2c::read_sensor_reg(uint8_t slave_addr, uint8_t reg, size_t /*n_bytes*/) noexcept {
    reg_addr_ = reg;
    if (!dispatch_read(slave_addr, reg)) {
        sr_ = SR_NACK;
        return 0;
    }
    sr_ = SR_ACK_OK | SR_RX_READY;
    return rx_queue_.size();
}

void I2c::write_sensor_reg(uint8_t slave_addr, uint8_t reg, uint16_t value) noexcept {
    uint8_t buf[2] = {
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFF)
    };
    if (dispatch_write(slave_addr, reg, buf, 2)) {
        sr_ = SR_ACK_OK;
    } else {
        sr_ = SR_NACK;
    }
}
