// -----------------------------------------------------------------------------
// peripherals/can.cpp — CAN 2.0A controller implementation
// -----------------------------------------------------------------------------
#include "can.h"
#include <algorithm>
#include <cstring>

// ─── CanBus ───────────────────────────────────────────────────────────────────

void CanBus::attach(Can* node) noexcept {
    if (count_ < MAX_NODES) nodes_[count_++] = node;
}

void CanBus::detach(Can* node) noexcept {
    for (size_t i = 0; i < count_; ++i) {
        if (nodes_[i] == node) {
            nodes_[i] = nodes_[--count_];
            nodes_[count_] = nullptr;
            return;
        }
    }
}

bool CanBus::broadcast(const CanFrame& frame, const Can* sender) noexcept {
    bool delivered = false;
    for (size_t i = 0; i < count_; ++i) {
        if (nodes_[i] != sender) {
            delivered |= nodes_[i]->inject(frame);
        }
    }
    return delivered;
}

// ─── Can ──────────────────────────────────────────────────────────────────────

Can::Can(const char* n, CanBus* bus) noexcept : name_(n), bus_(bus) {
    if (bus_) bus_->attach(this);
}

Can::~Can() noexcept {
    if (bus_) bus_->detach(this);
}

// ─── Register reads ───────────────────────────────────────────────────────────

uint32_t Can::read_reg(uint32_t offset) const {
    switch (offset) {
        case REG_TX_ID:  return tx_frame_.id;
        case REG_TX_DLC: return tx_frame_.dlc;
        case REG_TX_D0: {
            uint32_t w = 0;
            std::memcpy(&w, tx_frame_.data, 4);
            return w;
        }
        case REG_TX_D1: {
            uint32_t w = 0;
            std::memcpy(&w, tx_frame_.data + 4, 4);
            return w;
        }
        case REG_RX_ID:
            return (rx_count_ > 0) ? rx_buf_[rx_head_].id : 0u;
        case REG_RX_DLC:
            return (rx_count_ > 0) ? rx_buf_[rx_head_].dlc : 0u;
        case REG_RX_D0: {
            if (rx_count_ == 0) return 0;
            uint32_t w = 0;
            std::memcpy(&w, rx_buf_[rx_head_].data, 4);
            return w;
        }
        case REG_RX_D1: {
            if (rx_count_ == 0) return 0;
            uint32_t w = 0;
            std::memcpy(&w, rx_buf_[rx_head_].data + 4, 4);
            return w;
        }
        case REG_SR: return sr_;
        case REG_FILTER_ID:   return filter_id_;
        case REG_FILTER_MASK: return filter_mask_;
        default: return 0;
    }
}

// ─── Register writes ──────────────────────────────────────────────────────────

void Can::write_reg(uint32_t offset, uint32_t val) {
    switch (offset) {
        case REG_TX_ID:
            tx_frame_.id = static_cast<uint16_t>(val & 0x7FFu);
            break;
        case REG_TX_DLC:
            tx_frame_.dlc = static_cast<uint8_t>(val & 0xFu);
            if (tx_frame_.dlc > 8) tx_frame_.dlc = 8;
            break;
        case REG_TX_D0:
            std::memcpy(tx_frame_.data, &val, 4);
            break;
        case REG_TX_D1:
            std::memcpy(tx_frame_.data + 4, &val, 4);
            break;
        case REG_TX_SEND: {
            // Transmit: send frame to bus (and optionally loopback to self)
            sr_ &= ~SR_TX_OK;
            if (bus_) {
                bus_->broadcast(tx_frame_, this);
            }
            // Always ACK (no NACK in loopback-only simulation)
            sr_ |= SR_TX_OK;
            break;
        }
        case REG_RX_POP:
            if (rx_count_ > 0) {
                rx_head_ = (rx_head_ + 1) % RX_CAPACITY;
                --rx_count_;
                if (rx_count_ == 0) {
                    sr_ &= ~SR_RX_READY;
                }
            }
            break;
        case REG_FILTER_ID:
            filter_id_ = static_cast<uint16_t>(val & 0x7FFu);
            break;
        case REG_FILTER_MASK:
            filter_mask_ = static_cast<uint16_t>(val & 0x7FFu);
            break;
        default:
            break;
    }
}

// ─── Injection (from CanBus) ──────────────────────────────────────────────────

bool Can::inject(const CanFrame& frame) noexcept {
    if (!passes_filter(frame.id)) return false;
    if (!rx_push(frame)) {
        sr_ |= SR_RX_OVERFLOW;
        return false;
    }
    sr_ |= SR_RX_READY;
    return true;
}

// ─── Ring buffer helpers ──────────────────────────────────────────────────────

bool Can::rx_push(const CanFrame& f) noexcept {
    if (rx_count_ == RX_CAPACITY) return false;
    rx_buf_[rx_tail_] = f;
    rx_tail_ = (rx_tail_ + 1) % RX_CAPACITY;
    ++rx_count_;
    return true;
}

CanFrame Can::rx_peek() const noexcept {
    if (rx_count_ == 0) return {};
    return rx_buf_[rx_head_];
}
