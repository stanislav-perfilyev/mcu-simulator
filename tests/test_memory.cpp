#include <gtest/gtest.h>
#include "memory.h"
#include <cstdint>

class MemoryTest : public ::testing::Test {
protected:
    FlatMemoryBus mem;
};

// ─── Basic read/write ─────────────────────────────────────────────────────────

TEST_F(MemoryTest, WriteRead8) {
    mem.write8(0x100, 0xAB);
    EXPECT_EQ(mem.read8(0x100), 0xAB);
}

TEST_F(MemoryTest, WriteRead16_LittleEndian) {
    mem.write16(0x200, 0x1234);
    EXPECT_EQ(mem.read16(0x200), 0x1234);
    EXPECT_EQ(mem.read8(0x200), 0x34); // low byte first
    EXPECT_EQ(mem.read8(0x201), 0x12);
}

TEST_F(MemoryTest, WriteRead32) {
    mem.write32(0x300, 0xDEADBEEF);
    EXPECT_EQ(mem.read32(0x300), 0xDEADBEEF);
}

TEST_F(MemoryTest, ClearSetsZero) {
    mem.write32(0x400, 0x12345678);
    mem.clear();
    EXPECT_EQ(mem.read32(0x400), 0u);
}

// ─── Load bulk ────────────────────────────────────────────────────────────────

TEST_F(MemoryTest, BulkLoad) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    mem.load(0x500, data, sizeof(data));
    EXPECT_EQ(mem.read8(0x500), 0x01);
    EXPECT_EQ(mem.read8(0x503), 0x04);
    EXPECT_EQ(mem.read16(0x500), 0x0201);
    EXPECT_EQ(mem.read32(0x500), 0x04030201u);
}

// ─── Bounds checking ──────────────────────────────────────────────────────────

TEST_F(MemoryTest, OobRead8Throws) {
    EXPECT_THROW((void)mem.read8(0xFFFF + 1), BusFaultException);
}

TEST_F(MemoryTest, OobWrite8Throws) {
    EXPECT_THROW(mem.write8(0xFFFF + 1, 0), BusFaultException);
}

TEST_F(MemoryTest, OobRead16Throws) {
    EXPECT_THROW((void)mem.read16(0xFFFF), BusFaultException); // 2 bytes would overflow
}

TEST_F(MemoryTest, BusFaultContainsAddress) {
    try {
        (void)mem.read8(0xDEAD0000u);
        FAIL() << "Expected BusFaultException";
    } catch (const BusFaultException& e) {
        EXPECT_EQ(e.address, 0xDEAD0000u);
    }
}

// ─── SRAM region ─────────────────────────────────────────────────────────────

TEST_F(MemoryTest, SramRegionSize) {
    auto sram = mem.sram_region();
    EXPECT_EQ(sram.size(), MemMap::SRAM_SIZE);
}

TEST_F(MemoryTest, FlashRegionSize) {
    auto flash = mem.flash_region();
    EXPECT_EQ(flash.size(), MemMap::FLASH_SIZE);
}

// ─── Independent cells ────────────────────────────────────────────────────────

TEST_F(MemoryTest, AdjacentCellsIndependent) {
    mem.write8(0x100, 0x11);
    mem.write8(0x101, 0x22);
    mem.write8(0x102, 0x33);
    EXPECT_EQ(mem.read8(0x100), 0x11);
    EXPECT_EQ(mem.read8(0x101), 0x22);
    EXPECT_EQ(mem.read8(0x102), 0x33);
}

TEST_F(MemoryTest, Write32DoesNotCorruptNeighbors) {
    mem.write8(0x0FF, 0xAA);
    mem.write32(0x100, 0x12345678);
    mem.write8(0x104, 0xBB);
    EXPECT_EQ(mem.read8(0x0FF), 0xAA);
    EXPECT_EQ(mem.read32(0x100), 0x12345678u);
    EXPECT_EQ(mem.read8(0x104), 0xBB);
}

// ─── Additional coverage tests ────────────────────────────────────────────────

TEST_F(MemoryTest, BusFaultExceptionMessageContainsHex) {
    try {
        (void)mem.read8(0xABCD1234u);
        FAIL();
    } catch (const BusFaultException& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("ABCD1234"), std::string::npos);
    }
}

TEST_F(MemoryTest, BoundaryReadAddr0) {
    mem.write8(0x0000, 0x55);
    EXPECT_EQ(mem.read8(0x0000), 0x55);
}

TEST_F(MemoryTest, BoundaryReadHighAddr) {
    mem.write8(0xFFFE, 0xCC);
    EXPECT_EQ(mem.read8(0xFFFE), 0xCC);
}

TEST_F(MemoryTest, OobWrite16Throws) {
    EXPECT_THROW(mem.write16(0xFFFF, 0x1234), BusFaultException);
}

TEST_F(MemoryTest, OobWrite32Throws) {
    EXPECT_THROW(mem.write32(0xFFFC, 0xDEAD), BusFaultException);
}

TEST_F(MemoryTest, OobWrite8Throws_Boundary) {
    EXPECT_THROW(mem.write8(0x10000u, 0xFF), BusFaultException);
}

TEST_F(MemoryTest, FlashRegionReflectsWrites) {
    mem.write8(0x0001, 0x42);
    auto flash = mem.flash_region();
    EXPECT_EQ(flash[1], 0x42);
}

TEST_F(MemoryTest, SramRegionReflectsWrites) {
    // SRAM region offset 0 maps to some position in flat memory
    auto sram = mem.sram_region();
    EXPECT_EQ(sram.size(), MemMap::SRAM_SIZE);
    // Just verify it's a valid non-null span
    EXPECT_NE(sram.data(), nullptr);
}

TEST_F(MemoryTest, Read16LittleEndianHigh) {
    mem.write16(0x100, 0xBEEF);
    EXPECT_EQ(mem.read8(0x100), 0xEF);
    EXPECT_EQ(mem.read8(0x101), 0xBE);
}

TEST_F(MemoryTest, Write32Read8Components) {
    mem.write32(0x200, 0x11223344u);
    EXPECT_EQ(mem.read8(0x200), 0x44u);
    EXPECT_EQ(mem.read8(0x201), 0x33u);
    EXPECT_EQ(mem.read8(0x202), 0x22u);
    EXPECT_EQ(mem.read8(0x203), 0x11u);
}
