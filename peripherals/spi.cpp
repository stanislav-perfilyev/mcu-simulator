#include "spi.h"
#include <cstring>
#include <algorithm>

// ══ W25Q32Flash ══════════════════════════════════════════════════════════════

uint8_t W25Q32Flash::exchange(uint8_t tx) noexcept {
    switch (state_) {
        case State::IDLE:
            return 0xFF;

        case State::CMD:
            cmd_ = tx;
            addr_ = 0; addr_bytes_ = 0;
            buf_.clear();
            if (cmd_ == CMD_WRITE_ENABLE) {
                wel_ = true;
                state_ = State::IDLE; // single-byte command
                return 0xFF;
            }
            if (cmd_ == CMD_READ_ID) {
                state_ = State::DATA;
                return 0xFF;
            }
            if (cmd_ == CMD_READ_STATUS) {
                state_ = State::DATA;
                return 0xFF;
            }
            // Commands with 3-byte address
            state_ = State::ADDR;
            return 0xFF;

        case State::ADDR:
            addr_ = (addr_ << 8) | tx;
            if (++addr_bytes_ == 3) {
                addr_ &= (CAPACITY - 1);
                state_ = State::DATA;
            }
            return 0xFF;

        case State::DATA:
            return handle_data(tx);
    }
    return 0xFF;
}

uint8_t W25Q32Flash::handle_data(uint8_t tx) noexcept {
    switch (cmd_) {
        case CMD_READ_ID: {
            // Response sequence: dummy, MFR=0xEF, Dev hi=0x40, Dev lo=0x16
            static const uint8_t id_seq[] = {0xEF, 0x40, 0x16};
            size_t idx = buf_.size();
            buf_.push_back(tx);
            return (idx < 3) ? id_seq[idx] : 0xFF;
        }
        case CMD_READ_STATUS:
            return wel_ ? 0x02u : 0x00u;

        case CMD_READ_DATA: {
            uint8_t val = data_[addr_];
            addr_ = (addr_ + 1) & (CAPACITY - 1);
            return val;
        }
        case CMD_PAGE_PROGRAM:
            buf_.push_back(tx);
            return 0xFF;

        case CMD_SECTOR_ERASE:
            return 0xFF; // erase happens on cs_deassert (commit)

        default:
            return 0xFF;
    }
}

void W25Q32Flash::commit() noexcept {
    if (cmd_ == CMD_PAGE_PROGRAM && wel_ && !buf_.empty()) {
        uint32_t page_base = addr_ & ~(PAGE_SIZE - 1u);
        for (size_t i = 0; i < buf_.size() && i < PAGE_SIZE; ++i) {
            uint32_t a = page_base + static_cast<uint32_t>(i);
            if (a < CAPACITY)
                data_[a] &= buf_[i]; // NOR flash: can only clear bits
        }
        wel_ = false;
    } else if (cmd_ == CMD_SECTOR_ERASE && wel_) {
        erase_sector(addr_);
        wel_ = false;
    }
}

uint8_t W25Q32Flash::read_byte(uint32_t addr) const noexcept {
    return (addr < CAPACITY) ? data_[addr] : 0xFF;
}

bool W25Q32Flash::write_byte(uint32_t addr, uint8_t val) noexcept {
    if (addr >= CAPACITY) return false;
    data_[addr] &= val; // NOR flash semantics
    return true;
}

void W25Q32Flash::erase_sector(uint32_t addr) noexcept {
    uint32_t base = (addr / SECTOR_SIZE) * SECTOR_SIZE;
    if (base >= CAPACITY) return;
    std::fill(data_.begin() + base,
              data_.begin() + base + SECTOR_SIZE, 0xFF);
}

// ══ Spi ══════════════════════════════════════════════════════════════════════

void Spi::set_nss(bool low) noexcept {
    if (low && !cs_active_) {
        flash_.cs_assert();
        cs_active_ = true;
    } else if (!low && cs_active_) {
        flash_.cs_deassert();
        cs_active_ = false;
    }
    if (low) cr_ |= CR_NSS_LOW; else cr_ &= ~CR_NSS_LOW;
}

uint8_t Spi::transfer_byte(uint8_t tx) noexcept {
    return cs_active_ ? flash_.exchange(tx) : 0xFF;
}

void Spi::do_burst() noexcept {
    // burst_len_ = number of 32-bit words; auto-wraps NSS for a complete transaction
    if (!cs_active_) { flash_.cs_assert(); cs_active_ = true; }
    uint32_t n_bytes = burst_len_ * 4;
    for (uint32_t i = 0; i < n_bytes && i < 64; ++i)
        burst_rx_[i] = transfer_byte(burst_tx_[i]);
    // Auto-deassert to commit the transaction
    flash_.cs_deassert();
    cs_active_ = false;
    cr_ &= ~CR_NSS_LOW;
}

uint32_t Spi::read_reg(uint32_t offset) const {
    if (offset == REG_CR)  return cr_;
    if (offset == REG_SR)  return SR_TX_EMPTY | (rx_queue_.empty() ? 0u : SR_RX_READY);
    if (offset == REG_DR) {
        if (rx_queue_.empty()) return 0xFF;
        uint8_t b = rx_queue_.front(); rx_queue_.pop();
        return b;
    }
    if (offset == REG_BURST_LEN) return burst_len_;
    // BURST_TX read-back
    if (offset >= REG_BURST_TX && offset < REG_BURST_TX + 64) {
        uint32_t idx = (offset - REG_BURST_TX) / 4;
        return (static_cast<uint32_t>(burst_tx_[idx*4+0]) << 24) |
               (static_cast<uint32_t>(burst_tx_[idx*4+1]) << 16) |
               (static_cast<uint32_t>(burst_tx_[idx*4+2]) <<  8) |
                static_cast<uint32_t>(burst_tx_[idx*4+3]);
    }
    // BURST_RX
    if (offset >= REG_BURST_RX && offset < REG_BURST_RX + 64) {
        uint32_t idx = (offset - REG_BURST_RX) / 4;
        return (static_cast<uint32_t>(burst_rx_[idx*4+0]) << 24) |
               (static_cast<uint32_t>(burst_rx_[idx*4+1]) << 16) |
               (static_cast<uint32_t>(burst_rx_[idx*4+2]) <<  8) |
                static_cast<uint32_t>(burst_rx_[idx*4+3]);
    }
    return 0;
}

void Spi::write_reg(uint32_t offset, uint32_t val) {
    if (offset == REG_CR) {
        bool want_nss = (val & CR_NSS_LOW) != 0;
        cr_ = val & 0x0F;
        set_nss(want_nss);
        return;
    }
    if (offset == REG_DR) {
        uint8_t rx = transfer_byte(static_cast<uint8_t>(val & 0xFF));
        rx_queue_.push(rx);
        return;
    }
    if (offset == REG_BURST_LEN) {
        burst_len_ = std::min(val, 16u); // word count
        return;
    }
    // BURST_TX write
    if (offset >= REG_BURST_TX && offset < REG_BURST_TX + 64) {
        uint32_t idx = (offset - REG_BURST_TX) / 4;
        // Store MSB-first (big-endian) — SPI sends high byte first
        burst_tx_[idx*4+0] = static_cast<uint8_t>((val >> 24) & 0xFF);
        burst_tx_[idx*4+1] = static_cast<uint8_t>((val >> 16) & 0xFF);
        burst_tx_[idx*4+2] = static_cast<uint8_t>((val >>  8) & 0xFF);
        burst_tx_[idx*4+3] = static_cast<uint8_t>( val        & 0xFF);
        // Writing last TX word (burst_len_ = word count) triggers burst
        if (idx + 1 >= burst_len_)
            do_burst();
        return;
    }
}
