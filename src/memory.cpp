#include "memory.h"
#include <cstring>
#include <sstream>
#include <iomanip>

// ─── BusFaultException ───────────────────────────────────────────────────────

std::string BusFaultException::toHex(uint32_t v) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}

// ─── FlatMemoryBus ───────────────────────────────────────────────────────────

FlatMemoryBus::FlatMemoryBus() noexcept {
    data_.fill(0);
}

void FlatMemoryBus::clear() noexcept {
    data_.fill(0);
}

// Translate full 32-bit address into flat 64KB index.
// Supports: flash (0x0000-0xFFFF), SRAM mirror (0x2000_0000+), Periph (0x4000_0000+)
uint32_t FlatMemoryBus::translate(uint32_t addr) const {
    if (addr < SIZE) return addr;                                    // flat window
    if (addr >= MemMap::SRAM_BASE &&
        addr <  MemMap::SRAM_BASE + MemMap::SRAM_SIZE)
        return MemMap::FLASH_SIZE + (addr - MemMap::SRAM_BASE);      // SRAM after flash
    throw BusFaultException(addr);
}

void FlatMemoryBus::check_bounds(uint32_t addr, size_t access_size) const {
    uint32_t flat = translate(addr); // throws on unmapped
    if (flat + access_size > SIZE)
        throw BusFaultException(addr);
}

uint8_t FlatMemoryBus::read8(uint32_t addr) const {
    return data_[translate(addr)];
}

uint16_t FlatMemoryBus::read16(uint32_t addr) const {
    uint32_t flat = translate(addr);
    if (flat + 2 > SIZE) throw BusFaultException(addr);
    uint16_t val;
    std::memcpy(&val, &data_[flat], 2); // little-endian host assumed
    return val;
}

uint32_t FlatMemoryBus::read32(uint32_t addr) const {
    uint32_t flat = translate(addr);
    if (flat + 4 > SIZE) throw BusFaultException(addr);
    uint32_t val;
    std::memcpy(&val, &data_[flat], 4);
    return val;
}

void FlatMemoryBus::write8(uint32_t addr, uint8_t val) {
    data_[translate(addr)] = val;
}

void FlatMemoryBus::write16(uint32_t addr, uint16_t val) {
    uint32_t flat = translate(addr);
    if (flat + 2 > SIZE) throw BusFaultException(addr);
    std::memcpy(&data_[flat], &val, 2);
}

void FlatMemoryBus::write32(uint32_t addr, uint32_t val) {
    uint32_t flat = translate(addr);
    if (flat + 4 > SIZE) throw BusFaultException(addr);
    std::memcpy(&data_[flat], &val, 4);
}

void FlatMemoryBus::load(uint32_t addr, const uint8_t* src, size_t len) {
    uint32_t flat = translate(addr);
    if (flat + len > SIZE) throw BusFaultException(addr);
    std::memcpy(&data_[flat], src, len);
}

std::span<const uint8_t> FlatMemoryBus::flash_region() const noexcept {
    return {data_.data(), MemMap::FLASH_SIZE};
}

std::span<const uint8_t> FlatMemoryBus::sram_region() const noexcept {
    // SRAM mapped right after flash in our flat model
    return {data_.data() + MemMap::FLASH_SIZE, MemMap::SRAM_SIZE};
}
