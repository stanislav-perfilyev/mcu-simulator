#include <gtest/gtest.h>
#include "cpu.h"
#include "memory.h"
#include <vector>
#include <cstdint>

// ─── Test fixture ─────────────────────────────────────────────────────────────

class CpuTest : public ::testing::Test {
protected:
    FlatMemoryBus mem;
    CortexM0      cpu{mem};

    // Load Thumb-16 instructions at address 0 and reset PC
    void load(std::initializer_list<uint16_t> instrs) {
        mem.clear();
        uint32_t addr = 0;
        for (uint16_t instr : instrs) {
            mem.write16(addr, instr); addr += 2;
        }
        // BKPT or infinite loop as terminator (BKPT 0 = 0xBE00 → UndefinedInstructionException
        // Use NOP+HALT trick: BX r14 with LR=0 → halted_=true)
        cpu.reset();
        cpu.set_reg(RegIndex::PC, 0);
        cpu.set_reg(RegIndex::LR, 0); // LR=0, BX LR will halt
        cpu.set_reg(RegIndex::SP, 0x8000);
    }

    uint32_t r(int n) { return cpu.reg(static_cast<RegIndex>(n)); }
    APSR     flags()  { return cpu.apsr(); }

    // Run exactly N steps (throws on error)
    void run(uint64_t steps) { (void)cpu.run(steps); }
};

// ─── MOV immediate ────────────────────────────────────────────────────────────
// MOV r0, #42  → 0x2000 | (42) = 0x202A
TEST_F(CpuTest, MovImm8) {
    load({0x202A}); // MOV r0, #42
    run(1);
    EXPECT_EQ(r(0), 42u);
    EXPECT_FALSE(flags().N);
    EXPECT_FALSE(flags().Z);
}

TEST_F(CpuTest, MovImm8_Zero) {
    load({0x2000}); // MOV r0, #0
    run(1);
    EXPECT_EQ(r(0), 0u);
    EXPECT_TRUE(flags().Z);
    EXPECT_FALSE(flags().N);
}

// ─── ADD immediate (Format 2, imm3) ──────────────────────────────────────────
// ADD r2, r1, #3 = 00011_0_1_011_001_010 = 0x1CCA
TEST_F(CpuTest, AddImm3) {
    load({0x2005,  // MOV r0, #5
          0x1C40}); // ADD r0, r0, #1  (00011_1_0_001_000_000)
    run(2);
    EXPECT_EQ(r(0), 6u);
}

// ─── SUB immediate ───────────────────────────────────────────────────────────
// SUB r0, r0, #3 = Format 3: 001_11_000_00000011 = 0x3803
TEST_F(CpuTest, SubImm8) {
    load({0x200A,  // MOV r0, #10
          0x3803}); // SUB r0, #3
    run(2);
    EXPECT_EQ(r(0), 7u);
}

// ─── CMP ─────────────────────────────────────────────────────────────────────
TEST_F(CpuTest, CmpEqual) {
    load({0x2005,  // MOV r0, #5
          0x2805}); // CMP r0, #5
    run(2);
    EXPECT_TRUE(flags().Z);
    EXPECT_FALSE(flags().N);
}

TEST_F(CpuTest, CmpLess) {
    load({0x2003,  // MOV r0, #3
          0x2805}); // CMP r0, #5  → N=1 (negative result)
    run(2);
    EXPECT_TRUE(flags().N);
    EXPECT_FALSE(flags().Z);
}

// ─── ALU: AND / ORR / EOR ────────────────────────────────────────────────────
// AND r0, r1:  0x4008
// ORR r0, r1:  0x4308
// EOR r0, r1:  0x4048
TEST_F(CpuTest, AluAnd) {
    load({0x200F,  // MOV r0, #0x0F
          0x21F0,  // MOV r1, #0xF0
          0x4008}); // AND r0, r1
    run(3);
    EXPECT_EQ(r(0), 0u); // 0x0F & 0xF0 = 0
    EXPECT_TRUE(flags().Z);
}

TEST_F(CpuTest, AluOrr) {
    load({0x200F,  // MOV r0, #0x0F
          0x21F0,  // MOV r1, #0xF0
          0x4308}); // ORR r0, r1
    run(3);
    EXPECT_EQ(r(0), 0xFFu);
}

TEST_F(CpuTest, AluEor) {
    load({0x20FF,  // MOV r0, #0xFF
          0x210F,  // MOV r1, #0x0F
          0x4048}); // EOR r0, r1
    run(3);
    EXPECT_EQ(r(0), 0xF0u);
}

// ─── LSL / LSR ───────────────────────────────────────────────────────────────
// LSL r0, r0, #4 = 000_00_00100_000_000 = 0x0100
TEST_F(CpuTest, LslImm) {
    load({0x2001,  // MOV r0, #1
          0x0100}); // LSL r0, r0, #4
    run(2);
    EXPECT_EQ(r(0), 16u);
}

// LSR r0, r0, #1 = 000_01_00001_000_000 = 0x0840
TEST_F(CpuTest, LsrImm) {
    load({0x2010,  // MOV r0, #16
          0x0840}); // LSR r0, r0, #1
    run(2);
    EXPECT_EQ(r(0), 8u);
}

// ─── MUL ─────────────────────────────────────────────────────────────────────
// MUL r0, r1 = 010000_1101_001_000 = 0x4348
TEST_F(CpuTest, Mul) {
    load({0x2006,  // MOV r0, #6
          0x2107,  // MOV r1, #7
          0x4348}); // MUL r0, r1
    run(3);
    EXPECT_EQ(r(0), 42u);
}

// ─── LDR/STR ─────────────────────────────────────────────────────────────────
TEST_F(CpuTest, StrLdr) {
    // STR r0, [sp, #0] ; LDR r1, [sp, #0]
    // STR r0, sp-rel = 1001_0_000_00000000 = 0x9000
    // LDR r1, sp-rel = 1001_1_001_00000000 = 0x9900
    load({0x200A,  // MOV r0, #10
          0x9000,  // STR r0, [sp, #0]
          0x9900}); // LDR r1, [sp, #0]
    run(3);
    EXPECT_EQ(r(1), 10u);
}

// ─── PUSH / POP ──────────────────────────────────────────────────────────────
TEST_F(CpuTest, PushPop) {
    // PUSH {r0} = 1011_0_10_0_00000001 = 0xB401 with R=0
    // POP  {r1} = 1011_1_10_0_00000010 = 0xBC02
    load({0x2055,  // MOV r0, #85
          0xB401,  // PUSH {r0}
          0xBC02}); // POP {r1}
    run(3);
    EXPECT_EQ(r(1), 0x55u);
}

// ─── Conditional branch ───────────────────────────────────────────────────────
// BEQ +4 (skip next instr if Z=1)
// Pattern: MOV r0,#5; CMP r0,#5; BEQ skip; MOV r0,#0; skip: MOV r1,#1
TEST_F(CpuTest, BranchTaken) {
    load({
        0x2005,  // MOV r0, #5
        0x2805,  // CMP r0, #5   → Z=1
        0xD000,  // BEQ +0 (skip 1 instr: soff8=0 → target=PC+4+0=addr+4)
        0x2000,  // MOV r0, #0   (should be skipped)
        0x2101   // MOV r1, #1
    });
    run(4);
    EXPECT_EQ(r(0), 5u);  // not overwritten
    EXPECT_EQ(r(1), 1u);
}

TEST_F(CpuTest, BranchNotTaken) {
    load({
        0x2003,  // MOV r0, #3
        0x2805,  // CMP r0, #5   → Z=0
        0xD000,  // BEQ (not taken)
        0x2000,  // MOV r0, #0   (executed)
        0x2101   // MOV r1, #1
    });
    run(5);
    EXPECT_EQ(r(0), 0u); // was overwritten
    EXPECT_EQ(r(1), 1u);
}

// ─── Add/Sub — carry and overflow ─────────────────────────────────────────────
TEST_F(CpuTest, AddCarry) {
    // MOV r0, #0xFF; ADD r0, #1 → carry=1, result=0
    load({0x20FF,  // MOV r0, #0xFF
          0x3001}); // ADD r0, #1
    run(2);
    EXPECT_EQ(r(0), 0x100u);
    EXPECT_FALSE(flags().C); // no carry at 8-bit level, but 32-bit: no overflow
}

TEST_F(CpuTest, SubBorrow) {
    load({0x2001,  // MOV r0, #1
          0x3802}); // SUB r0, #2 → underflow
    run(2);
    EXPECT_EQ(r(0), 0xFFFFFFFFu);
    EXPECT_TRUE(flags().N);
}

// ─── Unconditional branch ─────────────────────────────────────────────────────
TEST_F(CpuTest, UnconditionalBranch) {
    // B with off11=0: target = (PC+2)+2+0 = addr+4, skips next instruction
    load({
        0xE000,  // B → jumps to addr 4 (off11=0, formula: pc()+2+0)
        0x2000,  // MOV r0, #0 (skipped)
        0x2001   // MOV r0, #1
    });
    run(2);
    EXPECT_EQ(r(0), 1u);
}

// ─── ADD SP, #imm ────────────────────────────────────────────────────────────
TEST_F(CpuTest, AddSpImm) {
    load({0xB004}); // ADD sp, #16 (imm7=4, *4=16)
    uint32_t sp_before = cpu.reg(RegIndex::SP); // capture AFTER load() sets SP
    run(1);
    EXPECT_EQ(cpu.reg(RegIndex::SP), sp_before + 16);
}

TEST_F(CpuTest, SubSpImm) {
    load({0xB084}); // SUB sp, #16
    uint32_t sp_before = cpu.reg(RegIndex::SP); // capture AFTER load() sets SP
    run(1);
    EXPECT_EQ(cpu.reg(RegIndex::SP), sp_before - 16);
}

// ─── ROR: rotate by 0 must be identity (was UB before fix) ───────────────────
// ROR r0, r1 = 010000_0111_001_000 = 0x41C8 (op=7, Rm=r1, Rd=r0)
TEST_F(CpuTest, RorByZero) {
    load({0x20AB,  // MOV r0, #0xAB
          0x2100,  // MOV r1, #0       — shift amount = 0
          0x41C8}); // ROR r0, r1
    run(3);
    EXPECT_EQ(r(0), 0xABu);  // identity: value unchanged
    // C flag must remain whatever it was before (not corrupted)
    EXPECT_FALSE(flags().Z);
    EXPECT_FALSE(flags().N);
}

// ─── ROR: normal rotation ─────────────────────────────────────────────────────
TEST_F(CpuTest, RorBy8) {
    load({0x20F0,  // MOV r0, #0xF0
          0x2108,  // MOV r1, #8       — rotate right 8
          0x41C8}); // ROR r0, r1
    run(3);
    // 0x000000F0 ror 8 = 0xF0000000
    EXPECT_EQ(r(0), 0xF0000000u);
    EXPECT_TRUE(flags().N);  // bit31 set
}

// ─── SBC: V-flag correctness ──────────────────────────────────────────────────
// SBC r0, r1 = 010000_0110_001_000 = 0x4188
// Case: 1 - 1 - borrow(1) = -1 → no signed overflow, V=0
// With the OLD buggy formula: ((a^~b) & (a^res)) → gives V=1 (wrong!)
// With the FIXED formula:     ((a^res) & (~b^res)) → gives V=0 (correct)
TEST_F(CpuTest, SbcVFlagNoOverflow) {
    load({
        0x2001,  // MOV r0, #1
        0x2101,  // MOV r1, #1
        0x2802,  // CMP r0, #2  → sets C=0 (1 < 2, borrow)
        0x2001,  // MOV r0, #1  → reload r0; MOV doesn't touch C
        0x4188   // SBC r0, r1  → r0 = 1 + ~1 + 0 = 0xFFFFFFFF
    });
    run(5);
    EXPECT_EQ(r(0), 0xFFFFFFFFu);
    EXPECT_TRUE(flags().N);
    EXPECT_FALSE(flags().C);  // borrow occurred
    EXPECT_FALSE(flags().V);  // 1-1-1=-1 is representable: no overflow
}

// ─── ROR: Rs=32 (multiple of 32, non-zero) → C=bit31, result=identity ─────────
// ARM DDI 0419E: when Rs[7:0] != 0 but Rs[7:0] MOD 32 == 0, C = bit31(Rm)
// Previous bug: b & 0x1F == 0 treated same as b==0 (C unchanged)
TEST_F(CpuTest, RorBy32_Cflag) {
    load({
        0x20F0,  // MOV r0, #0xF0  → bit31=0 (so C should become 0)
        0x2120,  // MOV r1, #32    → shift amount = 32
        0x41C8   // ROR r0, r1
    });
    run(3);
    EXPECT_EQ(r(0), 0xF0u);         // identity: value unchanged
    EXPECT_FALSE(flags().C);        // C = bit31(0xF0) = 0
    EXPECT_FALSE(flags().N);
}
