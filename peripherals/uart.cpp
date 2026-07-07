#include "uart.h"
#include <cstring>

// ── FIFO helpers ─────────────────────────────────────────────────────────────

bool Uart::tx_push(uint8_t b) noexcept {
    if (tx_count_ == FIFO_SIZE) return false;
    tx_fifo_[tx_tail_] = b;
    tx_tail_ = (tx_tail_ + 1) % FIFO_SIZE;
    ++tx_count_;
    return true;
}

bool Uart::tx_pop(uint8_t& b) noexcept {
    if (tx_count_ == 0) return false;
    b = tx_fifo_[tx_head_];
    tx_head_ = (tx_head_ + 1) % FIFO_SIZE;
    --tx_count_;
    return true;
}

bool Uart::rx_push(uint8_t b) noexcept {
    if (rx_count_ == FIFO_SIZE) return false;
    rx_fifo_[rx_tail_] = b;
    rx_tail_ = (rx_tail_ + 1) % FIFO_SIZE;
    ++rx_count_;
    return true;
}

bool Uart::rx_pop(uint8_t& b) const noexcept {
    if (rx_count_ == 0) return false;
    b = rx_fifo_[rx_head_];
    rx_head_ = (rx_head_ + 1) % FIFO_SIZE;
    --rx_count_;
    return true;
}

// ── Status register ──────────────────────────────────────────────────────────

uint32_t Uart::build_sr() const noexcept {
    uint32_t sr = 0;
    if (tx_count_ < FIFO_SIZE) sr |= SR_TX_EMPTY;
    if (rx_count_ > 0)          sr |= SR_RX_READY;
    if (rx_count_ == FIFO_SIZE) sr |= SR_RX_FULL;
    if (tx_count_ == FIFO_SIZE) sr |= SR_TX_FULL;
    if ((cr_ & CR_RX_IRQ_EN) && rx_irq_pending()) sr |= SR_RX_IRQ;
    return sr;
}

bool Uart::rx_irq_pending() const noexcept {
    return rx_count_ >= irq_thr_;
}

// ── IPeripheral ──────────────────────────────────────────────────────────────

uint32_t Uart::read_reg(uint32_t offset) const {
    switch (offset) {
        case REG_DR: {
            // Const-cast: reading DR is logically const (pop from FIFO)
            // but modifies internal state — use mutable or cast.
            uint8_t b = 0;
            [[maybe_unused]] bool ok = rx_pop(b);  // b=0 if FIFO empty (underflow)
            return b;
        }
        case REG_SR:       return build_sr();
        case REG_CR:       return cr_;
        case REG_IRQ_THR:  return irq_thr_;
        case REG_FIFO_LVL: return (static_cast<uint32_t>(tx_count_) << 8)
                                | static_cast<uint32_t>(rx_count_);
        default:           return 0;
    }
}

void Uart::write_reg(uint32_t offset, uint32_t val) {
    switch (offset) {
        case REG_DR: {
            uint8_t b = static_cast<uint8_t>(val & 0xFF);
            if (cr_ & CR_LOOPBACK) {
                [[maybe_unused]] bool ok2 = rx_push(b);  // loopback TX→RX
            } else {
                tx_push(b);
            }
            break;
        }
        case REG_CR:
            cr_ = val & 0x07u;
            break;
        case REG_IRQ_THR:
            irq_thr_ = (val == 0 || val > FIFO_SIZE) ? 1 : val;
            break;
        default:
            break; // ignore writes to read-only regs
    }
}

// ── Direct API ───────────────────────────────────────────────────────────────

bool Uart::inject_rx(uint8_t byte) noexcept {
    return rx_push(byte);
}

bool Uart::consume_tx(uint8_t& out) noexcept {
    return tx_pop(out);
}

std::string Uart::drain_tx() noexcept {
    std::string result;
    uint8_t b;
    while (tx_pop(b)) result.push_back(static_cast<char>(b));
    return result;
}

void Uart::reset() noexcept {
    tx_head_ = tx_tail_ = tx_count_ = 0;
    rx_head_ = rx_tail_ = rx_count_ = 0;
    cr_      = CR_ENABLE;
    irq_thr_ = 1;
}
