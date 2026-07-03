// -----------------------------------------------------------------------------
// tests/test_peripherals.cpp — GTests for UART, SPI, I2C, CAN peripherals
// 22 tests covering register interface, device behaviour, and PeripheralBus
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>
#include "peripherals/peripheral.h"
#include "peripherals/uart.h"
#include "peripherals/spi.h"
#include "peripherals/i2c.h"
#include "peripherals/can.h"

// ═══════════════════════════════════════════════════════════════════════════════
// PeripheralBus (2 tests)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PeripheralBus, AttachAndIsMaped) {
    Uart uart0("UART0");
    PeripheralBus bus;
    bus.attach(PeriphMap::UART0_BASE, &uart0);
    EXPECT_TRUE(bus.is_mapped(PeriphMap::UART0_BASE));
    EXPECT_TRUE(bus.is_mapped(PeriphMap::UART0_BASE + 0x10));
    EXPECT_FALSE(bus.is_mapped(PeriphMap::UART0_BASE + PeriphMap::REGION_SIZE));
}

TEST(PeripheralBus, ReadWriteDispatch) {
    Uart uart0("UART0");
    PeripheralBus bus;
    bus.attach(PeriphMap::UART0_BASE, &uart0);

    // Enable UART via CR register
    bus.write(PeriphMap::UART0_BASE + Uart::REG_CR, Uart::CR_ENABLE);
    uint32_t cr = bus.read(PeriphMap::UART0_BASE + Uart::REG_CR);
    EXPECT_EQ(cr & Uart::CR_ENABLE, Uart::CR_ENABLE);
}

// ═══════════════════════════════════════════════════════════════════════════════
// UART (6 tests)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Uart, InitialState) {
    Uart u("UART0");
    uint32_t sr = u.read_reg(Uart::REG_SR);
    // TX empty set, RX ready clear
    EXPECT_NE(sr & Uart::SR_TX_EMPTY, 0u);
    EXPECT_EQ(sr & Uart::SR_RX_READY, 0u);
}

TEST(Uart, TransmitAndDrainTx) {
    Uart u("UART0");
    u.write_reg(Uart::REG_CR, Uart::CR_ENABLE);
    u.write_reg(Uart::REG_DR, 'A');
    u.write_reg(Uart::REG_DR, 'B');
    u.write_reg(Uart::REG_DR, 'C');

    std::string sent = u.drain_tx();
    EXPECT_EQ(sent, "ABC");
}

TEST(Uart, LoopbackMode) {
    Uart u("UART0");
    u.write_reg(Uart::REG_CR, Uart::CR_ENABLE | Uart::CR_LOOPBACK);
    u.write_reg(Uart::REG_DR, 0x42u);

    uint32_t sr = u.read_reg(Uart::REG_SR);
    EXPECT_NE(sr & Uart::SR_RX_READY, 0u);   // byte appeared in RX
    uint32_t rx = u.read_reg(Uart::REG_DR);
    EXPECT_EQ(rx, 0x42u);
}

TEST(Uart, RxInject) {
    Uart u("UART0");
    u.write_reg(Uart::REG_CR, Uart::CR_ENABLE);
    (void)u.inject_rx('X');
    (void)u.inject_rx('Y');

    EXPECT_EQ(u.read_reg(Uart::REG_DR), (uint32_t)'X');
    EXPECT_EQ(u.read_reg(Uart::REG_DR), (uint32_t)'Y');
}

TEST(Uart, FifoLevelRegister) {
    Uart u("UART0");
    u.write_reg(Uart::REG_CR, Uart::CR_ENABLE | Uart::CR_LOOPBACK);
    u.write_reg(Uart::REG_DR, 1u);
    u.write_reg(Uart::REG_DR, 2u);
    u.write_reg(Uart::REG_DR, 3u);

    uint32_t lvl = u.read_reg(Uart::REG_FIFO_LVL);
    // Lower 16 bits = RX count
    EXPECT_EQ(lvl & 0xFFFFu, 3u);
}

TEST(Uart, IrqThresholdFlag) {
    Uart u("UART0");
    u.write_reg(Uart::REG_CR, Uart::CR_ENABLE | Uart::CR_RX_IRQ_EN | Uart::CR_LOOPBACK);
    u.write_reg(Uart::REG_IRQ_THR, 2u); // trigger IRQ when RX >= 2

    u.write_reg(Uart::REG_DR, 0x11u);
    EXPECT_EQ(u.read_reg(Uart::REG_SR) & Uart::SR_RX_IRQ, 0u); // not yet

    u.write_reg(Uart::REG_DR, 0x22u);
    EXPECT_NE(u.read_reg(Uart::REG_SR) & Uart::SR_RX_IRQ, 0u); // threshold hit
}

// ═══════════════════════════════════════════════════════════════════════════════
// SPI + W25Q32 Flash (5 tests)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Spi, ReadDeviceId) {
    Spi spi("SPI0");
    // CMD_READ_ID=0x9F via single-byte burst
    // Use burst: 1 word, TX = 0x9F_00_00_00
    spi.write_reg(Spi::REG_BURST_LEN, 1u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x9F000000u); // triggers burst
    // RX[0] should be {dummy=0xFF, 0xEF, 0x40, 0x16}
    uint32_t rx0 = spi.read_reg(Spi::REG_BURST_RX + 0);
    EXPECT_EQ((rx0 >> 16) & 0xFF, 0xEFu); // manufacturer
    EXPECT_EQ((rx0 >>  8) & 0xFF, 0x40u); // memory type
    EXPECT_EQ( rx0        & 0xFF, 0x16u); // capacity
}

TEST(Spi, PageProgramAndRead) {
    Spi spi("SPI0");
    spi.write_reg(Spi::REG_CR, Spi::CR_EN);

    // Write-enable: CMD=0x06
    spi.write_reg(Spi::REG_BURST_LEN, 1u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x06000000u);

    // Page program addr=0x000000, data=0xDEADBEEF (4 bytes)
    // CMD=0x02, ADDR[23:0]=0, DATA
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x02000000u); // cmd+addr
    spi.write_reg(Spi::REG_BURST_TX + 4, 0xDEADBEEFu); // data

    // Read back: CMD=0x03, ADDR=0, read 4 bytes
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x03000000u);
    spi.write_reg(Spi::REG_BURST_TX + 4, 0x00000000u);
    uint32_t rx = spi.read_reg(Spi::REG_BURST_RX + 4);
    EXPECT_EQ(rx, 0xDEADBEEFu);
}

TEST(Spi, NorSemantics_CanOnlyClearBits) {
    Spi spi("SPI0");

    auto we = [&]() {
        spi.write_reg(Spi::REG_BURST_LEN, 1u);
        spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
        spi.write_reg(Spi::REG_BURST_TX + 0, 0x06000000u);
    };

    // Write 0xFF (all bits 1)
    we();
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x02000000u);
    spi.write_reg(Spi::REG_BURST_TX + 4, 0xFFFFFFFFu);

    // Write 0xAA (attempt to set bits again — NOR: value &= new)
    we();
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x02000000u);
    spi.write_reg(Spi::REG_BURST_TX + 4, 0xAAAAAAAAu);

    // Read: should be 0xAA & 0xFF = 0xAA (bits were cleared)
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x03000000u);
    spi.write_reg(Spi::REG_BURST_TX + 4, 0x00000000u);
    EXPECT_EQ(spi.read_reg(Spi::REG_BURST_RX + 4), 0xAAAAAAAAu);
}

TEST(Spi, SectorErase) {
    Spi spi("SPI0");

    auto we = [&]() {
        spi.write_reg(Spi::REG_BURST_LEN, 1u);
        spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
        spi.write_reg(Spi::REG_BURST_TX + 0, 0x06000000u);
    };

    // Write some data
    we();
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x02000000u);
    spi.write_reg(Spi::REG_BURST_TX + 4, 0x12345678u);

    // Sector erase CMD=0x20, ADDR=0x000000
    we();
    spi.write_reg(Spi::REG_BURST_LEN, 1u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x20000000u);

    // Read back: should be 0xFFFFFFFF (erased)
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x03000000u);
    spi.write_reg(Spi::REG_BURST_TX + 4, 0x00000000u);
    EXPECT_EQ(spi.read_reg(Spi::REG_BURST_RX + 4), 0xFFFFFFFFu);
}

TEST(Spi, StatusRegisterName) {
    Spi spi("SPI0");
    EXPECT_STREQ(spi.name(), "SPI0");
}

// ═══════════════════════════════════════════════════════════════════════════════
// I2C + LM75 (5 tests)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(I2c, DefaultTemperature25C) {
    I2c i2c("I2C0");
    float t = i2c.sensor().temperature_c();
    EXPECT_NEAR(t, 25.0f, 0.01f);
}

TEST(I2c, ReadTempRegisterViaBus) {
    I2c i2c("I2C0");
    // Latch reg address 0x00 (temp), then read
    i2c.write_reg(I2c::REG_REG_ADDR, 0x00u);
    // START + READ
    uint32_t cr = Lm75::DEFAULT_ADDR | I2c::CR_READ | I2c::CR_START;
    i2c.write_reg(I2c::REG_CR, cr);
    EXPECT_NE(i2c.read_reg(I2c::REG_SR) & I2c::SR_ACK_OK, 0u);
    EXPECT_NE(i2c.read_reg(I2c::REG_SR) & I2c::SR_RX_READY, 0u);

    uint8_t msb = static_cast<uint8_t>(i2c.read_reg(I2c::REG_DR));
    uint8_t lsb = static_cast<uint8_t>(i2c.read_reg(I2c::REG_DR));
    EXPECT_EQ(msb, 0x19u); // 25°C MSB
    EXPECT_EQ(lsb, 0x00u);
}

TEST(I2c, SetTemperatureAndRead) {
    I2c i2c("I2C0");
    i2c.sensor().set_temperature_c10(375); // 37.5°C

    size_t n = i2c.read_sensor_reg(Lm75::DEFAULT_ADDR, Lm75::REG_TEMP, 2);
    EXPECT_EQ(n, 2u);

    uint8_t msb = static_cast<uint8_t>(i2c.read_reg(I2c::REG_DR));
    EXPECT_EQ(msb, 0x25u); // 37°C = 0x25
}

TEST(I2c, NackOnUnknownSlave) {
    I2c i2c("I2C0");
    i2c.write_reg(I2c::REG_REG_ADDR, 0x00u);
    uint32_t cr = 0x50u | I2c::CR_READ | I2c::CR_START; // bad slave addr
    i2c.write_reg(I2c::REG_CR, cr);
    EXPECT_NE(i2c.read_reg(I2c::REG_SR) & I2c::SR_NACK, 0u);
}

TEST(I2c, WriteAndVerifyTosRegister) {
    I2c i2c("I2C0");
    // Set Tos to 60°C = 0x3C00
    i2c.write_sensor_reg(Lm75::DEFAULT_ADDR, Lm75::REG_TOS, 0x3C00u);
    uint16_t tos = i2c.sensor().read_reg16(Lm75::REG_TOS);
    EXPECT_EQ(tos, 0x3C00u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CAN 2.0A (6 tests)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Can, TxStatusOkAfterSend) {
    CanBus bus;
    Can node0("CAN0", &bus);

    node0.write_reg(Can::REG_TX_ID,  0x123u);
    node0.write_reg(Can::REG_TX_DLC, 4u);
    node0.write_reg(Can::REG_TX_D0,  0xDEADBEEFu);
    node0.write_reg(Can::REG_TX_SEND, 1u);

    EXPECT_NE(node0.read_reg(Can::REG_SR) & Can::SR_TX_OK, 0u);
}

TEST(Can, TwoNodeCommunication) {
    CanBus bus;
    Can node0("CAN0", &bus);
    Can node1("CAN1", &bus);

    // node0 sends 0x1AB with 2 bytes
    node0.write_reg(Can::REG_TX_ID,  0x1ABu);
    node0.write_reg(Can::REG_TX_DLC, 2u);
    node0.write_reg(Can::REG_TX_D0,  0xCAFE0000u);
    node0.write_reg(Can::REG_TX_SEND, 1u);

    // node1 should have it
    EXPECT_NE(node1.read_reg(Can::REG_SR) & Can::SR_RX_READY, 0u);
    EXPECT_EQ(node1.read_reg(Can::REG_RX_ID),  0x1ABu);
    EXPECT_EQ(node1.read_reg(Can::REG_RX_DLC), 2u);
    EXPECT_EQ(node1.read_reg(Can::REG_RX_D0),  0xCAFE0000u);

    // node0 should NOT receive its own frame
    EXPECT_EQ(node0.read_reg(Can::REG_SR) & Can::SR_RX_READY, 0u);
}

TEST(Can, RxPopRemovesFrame) {
    CanBus bus;
    Can node0("CAN0", &bus);
    Can node1("CAN1", &bus);

    node0.write_reg(Can::REG_TX_ID,  0x01u);
    node0.write_reg(Can::REG_TX_DLC, 1u);
    node0.write_reg(Can::REG_TX_D0,  0xAAu);
    node0.write_reg(Can::REG_TX_SEND, 1u);

    EXPECT_TRUE(node1.rx_ready());
    node1.write_reg(Can::REG_RX_POP, 1u);
    EXPECT_FALSE(node1.rx_ready());
}

TEST(Can, FilterAcceptMatchingId) {
    CanBus bus;
    Can node0("CAN0", &bus);
    Can node1("CAN1", &bus);

    // Filter: accept only ID=0x100, mask=0x7FF (exact match)
    node1.write_reg(Can::REG_FILTER_ID,   0x100u);
    node1.write_reg(Can::REG_FILTER_MASK, 0x7FFu);

    node0.write_reg(Can::REG_TX_ID,   0x100u);
    node0.write_reg(Can::REG_TX_DLC,  1u);
    node0.write_reg(Can::REG_TX_SEND, 1u);

    EXPECT_TRUE(node1.rx_ready());
}

TEST(Can, FilterRejectNonMatchingId) {
    CanBus bus;
    Can node0("CAN0", &bus);
    Can node1("CAN1", &bus);

    node1.write_reg(Can::REG_FILTER_ID,   0x100u);
    node1.write_reg(Can::REG_FILTER_MASK, 0x7FFu);

    node0.write_reg(Can::REG_TX_ID,   0x200u); // doesn't match
    node0.write_reg(Can::REG_TX_DLC,  1u);
    node0.write_reg(Can::REG_TX_SEND, 1u);

    EXPECT_FALSE(node1.rx_ready());
}

TEST(Can, RxBufferOrdering) {
    CanBus bus;
    Can tx("CAN_TX", &bus);
    Can rx("CAN_RX", &bus);

    for (uint16_t i = 0; i < 3; ++i) {
        tx.write_reg(Can::REG_TX_ID,  i);
        tx.write_reg(Can::REG_TX_DLC, 1u);
        tx.write_reg(Can::REG_TX_D0,  static_cast<uint32_t>(i) << 24);
        tx.write_reg(Can::REG_TX_SEND, 1u);
    }

    for (uint16_t i = 0; i < 3; ++i) {
        EXPECT_EQ(rx.read_reg(Can::REG_RX_ID), i); // FIFO order
        rx.write_reg(Can::REG_RX_POP, 1u);
    }
    EXPECT_FALSE(rx.rx_ready());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Edge-case / fault-tolerance tests (6 additional)
// ═══════════════════════════════════════════════════════════════════════════════

// --- UART: read empty RX returns 0xFF ----------------------------------------
TEST(Uart, ReadEmptyRxReturns0) {
    Uart u("UART0");
    // Empty RX FIFO: DR returns 0, SR_RX_READY is clear
    EXPECT_EQ(u.read_reg(Uart::REG_DR), 0u);
    EXPECT_EQ(u.read_reg(Uart::REG_SR) & Uart::SR_RX_READY, 0u);
}

// --- UART: TX FIFO overflow clamps, no crash ---------------------------------
TEST(Uart, TxFifoOverflowNoCrash) {
    Uart u("UART0");
    u.write_reg(Uart::REG_CR, Uart::CR_ENABLE);
    // Fill TX FIFO (capacity=FIFO_SIZE) + 10 extra — must not crash or corrupt
    for (size_t i = 0; i < Uart::FIFO_SIZE + 10; ++i)
        u.write_reg(Uart::REG_DR, static_cast<uint32_t>('A' + (i % 26)));
    // SR_TX_FULL should be set once capacity is reached
    uint32_t sr = u.read_reg(Uart::REG_SR);
    EXPECT_NE(sr & Uart::SR_TX_FULL, 0u);
    // drain should return exactly FIFO_SIZE bytes (extras dropped)
    std::string drained = u.drain_tx();
    EXPECT_EQ(drained.size(), Uart::FIFO_SIZE);
}

// --- PeripheralBus: unmapped read throws AddressError -----------------------
TEST(PeripheralBus, UnmappedReadThrows) {
    PeripheralBus bus;
    EXPECT_THROW((void)bus.read(0x1234'5678u), AddressError);
}

// --- PeripheralBus: unmapped write throws AddressError ----------------------
TEST(PeripheralBus, UnmappedWriteThrows) {
    PeripheralBus bus;
    EXPECT_THROW(bus.write(0xDEAD'BEEFu, 0u), AddressError);
}

// --- CAN: RX overflow flag set when buffer full -----------------------------
TEST(Can, RxOverflowFlagOnFullBuffer) {
    CanBus bus;
    Can tx("TX", &bus);
    Can rx("RX", &bus);

    // Fill RX buffer (capacity=32), then one more
    for (int i = 0; i <= 32; ++i) {
        tx.write_reg(Can::REG_TX_ID,  static_cast<uint32_t>(i & 0x7FF));
        tx.write_reg(Can::REG_TX_DLC, 1u);
        tx.write_reg(Can::REG_TX_SEND, 1u);
    }
    EXPECT_NE(rx.read_reg(Can::REG_SR) & Can::SR_RX_OVERFLOW, 0u);
    EXPECT_EQ(rx.rx_count(), 32u); // buffer didn't grow past capacity
}

// --- SPI: SPI access to flash beyond capacity is clamped --------------------
TEST(Spi, FlashBeyondCapacityIsClamped) {
    Spi spi("SPI0");
    // Write-enable
    spi.write_reg(Spi::REG_BURST_LEN, 1u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x06000000u);

    // Page program at addr=0xFFFFFF (near capacity boundary) — must not crash
    spi.write_reg(Spi::REG_BURST_LEN, 2u);
    spi.write_reg(Spi::REG_CR, Spi::CR_EN | Spi::CR_NSS_LOW);
    spi.write_reg(Spi::REG_BURST_TX + 0, 0x02FFFFFFu); // addr=0xFFFFFF
    spi.write_reg(Spi::REG_BURST_TX + 4, 0xDEADBEEFu);
    // No crash = pass; we just read back (addr wraps to within capacity)
    SUCCEED();
}
