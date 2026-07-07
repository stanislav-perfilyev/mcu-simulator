#pragma once
// -----------------------------------------------------------------------------
// peripherals/spi.h — SPI master + simulated W25Q32 SPI Flash (64 KB)
//
// Register map (offset from base):
//   0x00  CR   — bit0=EN, bit1=CPOL, bit2=CPHA, bit3=NSS_LOW (chip select)
//   0x04  SR   — bit0=TX_EMPTY, bit1=RX_READY, bit2=BUSY (read-only)
//   0x08  DR   — write=queue TX byte + trigger transfer, read=pop RX byte
//   0x0C  BURST_LEN — number of bytes for burst_transfer (1–64)
//   0x10  BURST_TX[0..15] — TX data for burst (16 × 4-byte words = 64 bytes)
//   0x50  BURST_RX[0..15] — RX data after burst (read-only)
//
// W25Q32 commands simulated:
//   0x9F  READ_ID      → manufacturer=0xEF device=0x4016
//   0x03  READ_DATA    → addr[23:0] then N data bytes
//   0x02  PAGE_PROGRAM → addr[23:0] then up to 256 data bytes (write 0s only)
//   0x20  SECTOR_ERASE → addr[23:0] erases 4KB sector (fills 0xFF)
//   0x05  READ_STATUS  → bit1=WEL, bit0=BUSY
//   0x06  WRITE_ENABLE → sets WEL
// -----------------------------------------------------------------------------
#include "peripheral.h"
#include <array>
#include <cstdint>
#include <vector>
#include <queue>
#include <string>

class W25Q32Flash {
public:
    static constexpr uint32_t CAPACITY     = 64 * 1024; // 64 KB
    static constexpr uint32_t SECTOR_SIZE  = 4096;
    static constexpr uint32_t PAGE_SIZE    = 256;
    static constexpr uint8_t  CMD_READ_ID      = 0x9F;
    static constexpr uint8_t  CMD_READ_DATA    = 0x03;
    static constexpr uint8_t  CMD_PAGE_PROGRAM = 0x02;
    static constexpr uint8_t  CMD_SECTOR_ERASE = 0x20;
    static constexpr uint8_t  CMD_READ_STATUS  = 0x05;
    static constexpr uint8_t  CMD_WRITE_ENABLE = 0x06;

    W25Q32Flash() noexcept { data_.fill(0xFF); }

    // Called when CS goes LOW — starts a transaction
    void cs_assert() noexcept { state_ = State::CMD; buf_.clear(); }

    // Called when CS goes HIGH — ends a transaction
    void cs_deassert() noexcept { commit(); state_ = State::IDLE; buf_.clear(); }

    // Exchange one byte: send `tx`, return response byte
    [[nodiscard]] uint8_t exchange(uint8_t tx) noexcept;

    // Direct read (for tests)
    [[nodiscard]] uint8_t  read_byte(uint32_t addr) const noexcept;
    [[nodiscard]] bool     write_byte(uint32_t addr, uint8_t val) noexcept;
    void                   erase_sector(uint32_t addr) noexcept;
    [[nodiscard]] const std::array<uint8_t, CAPACITY>& raw() const noexcept { return data_; }

private:
    enum class State { IDLE, CMD, ADDR, DATA };
    State state_  = State::IDLE;
    uint8_t cmd_  = 0;
    uint32_t addr_= 0;
    uint8_t addr_bytes_ = 0;
    std::vector<uint8_t> buf_;
    bool wel_ = false; // write enable latch

    std::array<uint8_t, CAPACITY> data_{};

    void commit() noexcept;
    [[nodiscard]] uint8_t handle_data(uint8_t tx) noexcept;
};

// ─── SPI peripheral ──────────────────────────────────────────────────────────

class Spi final : public IPeripheral {
public:
    static constexpr uint32_t REG_CR         = 0x00;
    static constexpr uint32_t REG_SR         = 0x04;
    static constexpr uint32_t REG_DR         = 0x08;
    static constexpr uint32_t REG_BURST_LEN  = 0x0C;
    static constexpr uint32_t REG_BURST_TX   = 0x10; // [0x10..0x4C] 16 words
    static constexpr uint32_t REG_BURST_RX   = 0x50; // [0x50..0x8C] 16 words

    static constexpr uint32_t CR_EN      = (1u << 0);
    static constexpr uint32_t CR_CPOL    = (1u << 1);
    static constexpr uint32_t CR_CPHA    = (1u << 2);
    static constexpr uint32_t CR_NSS_LOW = (1u << 3);
    static constexpr uint32_t SR_TX_EMPTY = (1u << 0);
    static constexpr uint32_t SR_RX_READY = (1u << 1);
    static constexpr uint32_t SR_BUSY     = (1u << 2);

    explicit Spi(const char* n = "SPI0") noexcept : name_(n) { cr_ = CR_EN; }
    Spi(const Spi&) = delete;
    Spi& operator=(const Spi&) = delete;

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] uint32_t read_reg (uint32_t offset) const override;
    void                   write_reg(uint32_t offset, uint32_t val) override;

    [[nodiscard]] W25Q32Flash& flash() noexcept { return flash_; }

private:
    const char*  name_;
    uint32_t     cr_         = CR_EN;
    uint32_t     burst_len_  = 1;

    std::array<uint8_t, 64> burst_tx_{};
    std::array<uint8_t, 64> burst_rx_{};

    mutable std::queue<uint8_t> rx_queue_;
    W25Q32Flash flash_;
    bool cs_active_ = false;

    void set_nss(bool low) noexcept;
    [[nodiscard]] uint8_t transfer_byte(uint8_t tx) noexcept;
    void    do_burst() noexcept;
};
