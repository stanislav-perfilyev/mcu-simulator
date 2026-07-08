#include "cpu.h"
#include "memory.h"
#include <sstream>
#include <iomanip>
#include <cstring>

// ─── Architecture constants ───────────────────────────────────────────────────
static constexpr uint8_t  WORD_BITS     = 32; ///< Bits in a 32-bit word (uint32_t)
static constexpr uint8_t  MSB_POS       = 31; ///< Position of the MSB in a 32-bit word
static constexpr uint8_t  BL_SIGN_SHIFT = 21; ///< Sign-extension shift for BL 11-bit offset


// ─── UndefinedInstructionException ───────────────────────────────────────────

std::string UndefinedInstructionException::toHex(uint16_t v) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << v;
    return oss.str();
}

// ─── CortexM0 ────────────────────────────────────────────────────────────────

CortexM0::CortexM0(IMemoryBus& mem) noexcept : mem_(mem) {
    reset();
}

void CortexM0::reset() noexcept {
    regs_.fill(0);
    regs_[static_cast<int>(RegIndex::SP)] = RESET_SP;
    regs_[static_cast<int>(RegIndex::PC)] = RESET_PC & ~1u;
    apsr_ = {};
    cycles_ = 0;
    halted_ = false;
    bl_offset_ = 0;
}

uint32_t CortexM0::reg(RegIndex r) const noexcept {
    return regs_[static_cast<uint8_t>(r)];
}

void CortexM0::set_reg(RegIndex r, uint32_t v) noexcept {
    regs_[static_cast<uint8_t>(r)] = v;
}

uint16_t CortexM0::fetch() {
    uint32_t addr = pc();
    uint16_t instr = mem_.read16(addr);
    advance_pc();
    return instr;
}

bool CortexM0::step() {
    if (halted_) return false;
    uint32_t cur_pc = pc();
    uint16_t instr = fetch();
    if (trace_cb_) {
        // Brief disassembly placeholder; real one is in trace.h
        trace_cb_(cur_pc, instr, "");
    }
    execute(instr);
    ++cycles_;
    return !halted_;
}

uint64_t CortexM0::run(uint64_t max_steps) {
    uint64_t steps = 0;
    while (steps < max_steps && step()) ++steps;
    return steps;
}

// ─── Helper: add with carry ───────────────────────────────────────────────────

std::pair<uint32_t,bool> CortexM0::add32(uint32_t a, uint32_t b, bool cin) {
    uint64_t res64 = static_cast<uint64_t>(a) + b + (cin ? 1u : 0u);
    return {static_cast<uint32_t>(res64), (res64 >> WORD_BITS) != 0};
}

std::pair<uint32_t,bool> CortexM0::sub32(uint32_t a, uint32_t b) {
    return add32(a, ~b, true);
}

// ─── Condition code check ─────────────────────────────────────────────────────

bool CortexM0::condition_passes(uint8_t cond) const noexcept {
    switch (cond & 0xF) {
        case 0x0: return apsr_.Z;                               // EQ
        case 0x1: return !apsr_.Z;                              // NE
        case 0x2: return apsr_.C;                               // CS/HS
        case 0x3: return !apsr_.C;                              // CC/LO
        case 0x4: return apsr_.N;                               // MI
        case 0x5: return !apsr_.N;                              // PL
        case 0x6: return apsr_.V;                               // VS
        case 0x7: return !apsr_.V;                              // VC
        case 0x8: return apsr_.C && !apsr_.Z;                   // HI
        case 0x9: return !apsr_.C || apsr_.Z;                   // LS
        case 0xA: return apsr_.N == apsr_.V;                    // GE
        case 0xB: return apsr_.N != apsr_.V;                    // LT
        case 0xC: return !apsr_.Z && (apsr_.N == apsr_.V);     // GT
        case 0xD: return apsr_.Z || (apsr_.N != apsr_.V);      // LE
        case 0xE: return true;                                   // AL
        default:  return false;
    }
}

// ─── Main decode / dispatch ───────────────────────────────────────────────────

void CortexM0::execute(uint16_t instr) {
    // Thumb-16 encoding dispatch:
    //   top5 = bits[15:11], top4 = bits[15:12], top6 = bits[15:10]
    uint8_t top5 = static_cast<uint8_t>((instr >> 11) & 0x1FU);
    uint8_t top4 = static_cast<uint8_t>((instr >> 12) & 0x0FU);
    uint8_t top6 = static_cast<uint8_t>((instr >> 10) & 0x3FU);

    if      (top5 <= 0x02)                    exec_shift_imm(instr);   // 000_00..000_10: LSL/LSR/ASR
    else if (top5 == 0x03)                    exec_add_sub(instr);      // 000_11: ADD/SUB reg/imm3
    else if (top5 >= 0x04 && top5 <= 0x07)   exec_imm8(instr);         // 001_xx: MOV/CMP/ADD/SUB imm8
    else if (top6 == 0x10)                    exec_alu(instr);           // 010000: ALU
    else if (top6 == 0x11)                    exec_hi_reg_bx(instr);    // 010001: Hi reg / BX
    else if (top5 == 0x09)                    exec_ldr_pc(instr);        // 01001: LDR PC-rel
    else if (top4 == 0x5 && !((instr>>9)&1)) exec_ldr_str_reg(instr);  // 0101x0: LDR/STR reg
    else if (top4 == 0x6 || top4 == 0x7)     exec_ldr_str_imm(instr);  // 011x: LDR/STR imm5
    else if (top4 == 0x8)                     exec_ldrh_strh_imm(instr);// Enc:0x8: LDRH/STRH
    else if (top4 == 0x9)                     exec_ldr_str_sp(instr);   // Enc:0x9: SP-relative
    else if (top4 == 0xA)                     exec_add_sp_pc(instr);    // Enc:0xA: ADD Rd,SP/PC
    else if (top4 == 0xB) {
        const auto top8 = static_cast<uint8_t>(instr >> 8);
        if      (top8 == 0xB0)  exec_sp_ops(instr);    // ADD/SUB SP #imm7
        else if (top8 == 0xBF)  {}  // NOP/YIELD/WFE/WFI/SEV hint — do nothing (ARMv6-M B1.9.4)
        else if (top8 == 0xBE)  {}  // BKPT — treat as NOP in simulator
        else                     exec_push_pop(instr);  // PUSH/POP
    }
    else if (top4 == 0xC)                     exec_ldm_stm(instr);      // Enc:0xC: LDM/STM
    else if (top4 == 0xD && ((instr>>8)&0xF) != 0xF)
                                               exec_branch_cond(instr);  // Enc:0xD: B<cond>
    else if (top5 == 0x1C)                    exec_branch(instr);        // Enc:0x1C: B
    else if (top5 == 0x1E)                    exec_bl_upper(instr);      // Enc:0x1E: BL hi
    else if (top5 == 0x1F)                    exec_bl_lower(instr);      // Enc:0x1F: BL lo
    else
        throw UndefinedInstructionException(instr);
}

// ─── Format 1: Shift by immediate ────────────────────────────────────────────
// 000xx_imm5_Rs_Rd
void CortexM0::exec_shift_imm(uint16_t instr) {
    uint8_t  op     = (instr >> 11) & 0x3;
    uint8_t  imm5   = (instr >> 6)  & 0x1F;
    uint8_t  rs     = (instr >> 3)  & 0x7;
    uint8_t  rd     = instr         & 0x7;
    uint32_t val    = regs_[rs];
    uint32_t result = 0;

    switch (op) {
        case 0: // LSL
            if (imm5 == 0) { result = val; }
            else           { apsr_.C = (val >> (WORD_BITS - imm5)) & 1; result = val << imm5; }
            break;
        case 1: // LSR
            if (imm5 == 0) { apsr_.C = (val >> MSB_POS) & 1; result = 0; }
            else           { apsr_.C = (val >> (imm5 - 1)) & 1; result = val >> imm5; }
            break;
        case 2: // ASR
            if (imm5 == 0) { apsr_.C = (val >> MSB_POS) & 1; result = static_cast<uint32_t>(static_cast<int32_t>(val) >> MSB_POS); }
            else           { apsr_.C = (val >> (imm5 - 1)) & 1;
                             result = static_cast<uint32_t>(static_cast<int32_t>(val) >> imm5); }
            break;
        default: throw UndefinedInstructionException(instr);
    }
    apsr_.update_nz(result);
    regs_[rd] = result;
}

// ─── Format 2: Add/subtract ───────────────────────────────────────────────────
// 00011_I_Op_Rn/imm3_Rs_Rd
void CortexM0::exec_add_sub(uint16_t instr) {
    bool     I   = (instr >> 10) & 1;
    bool     sub = (instr >> 9)  & 1;
    uint8_t  rn  = (instr >> 6)  & 0x7;
    uint8_t  rs  = (instr >> 3)  & 0x7;
    uint8_t  rd  = instr         & 0x7;

    uint32_t a   = regs_[rs];
    uint32_t b   = I ? static_cast<uint32_t>(rn) : regs_[rn];

    auto [result, carry] = sub ? sub32(a, b) : add32(a, b);
    apsr_.C = carry;
    apsr_.V = sub ? (((a ^ b) & (a ^ result)) >> MSB_POS) & 1
                  : (((a ^ result) & (b ^ result)) >> MSB_POS) & 1;
    apsr_.update_nz(result);
    regs_[rd] = result;
}

// ─── Format 3: MOV/CMP/ADD/SUB imm8 ─────────────────────────────────────────
// Op_Rd_Offset8
void CortexM0::exec_imm8(uint16_t instr) {
    uint8_t  op   = (instr >> 11) & 0x3;
    uint8_t  rd   = (instr >> 8)  & 0x7;
    uint32_t imm8 = instr         & 0xFF;

    switch (op) {
        case 0: { // MOV
            apsr_.update_nz(imm8);
            regs_[rd] = imm8;
            break;
        }
        case 1: { // CMP
            auto [res, carry] = sub32(regs_[rd], imm8);
            apsr_.C = carry;
            apsr_.V = (((regs_[rd] ^ imm8) & (regs_[rd] ^ res)) >> MSB_POS) & 1;
            apsr_.update_nz(res);
            break;
        }
        case 2: { // ADD
            auto [res, carry] = add32(regs_[rd], imm8);
            apsr_.C = carry;
            apsr_.V = (((regs_[rd] ^ res) & (imm8 ^ res)) >> MSB_POS) & 1;
            apsr_.update_nz(res);
            regs_[rd] = res;
            break;
        }
        case 3: { // SUB
            auto [res, carry] = sub32(regs_[rd], imm8);
            apsr_.C = carry;
            apsr_.V = (((regs_[rd] ^ imm8) & (regs_[rd] ^ res)) >> MSB_POS) & 1;
            apsr_.update_nz(res);
            regs_[rd] = res;
            break;
        }
    }
}

// ─── Format 4 helpers ────────────────────────────────────────────────────────

// Shift ALU ops (LSL/LSR/ASR/ROR): op=2,3,4,7
void CortexM0::exec_alu_shift(uint8_t op, uint8_t rd, uint32_t a, uint32_t b) noexcept {
    uint8_t sh = b & 0xFF;
    uint32_t result = 0;
    switch (op) {
        case 0x2: // LSL
            if (sh > 0) apsr_.C = (sh < WORD_BITS) ? (a >> (WORD_BITS-sh)) & 1 : (sh == WORD_BITS) ? a & 1 : 0;
            result = (sh >= WORD_BITS) ? 0u : (a << sh); break;
        case 0x3: // LSR
            if (sh > 0) apsr_.C = (sh < WORD_BITS) ? (a >> (sh-1)) & 1 : (sh == WORD_BITS) ? (a >> MSB_POS) & 1 : 0;
            result = (sh >= WORD_BITS) ? 0u : (a >> sh); break;
        case 0x4: // ASR
            if (sh > 0) apsr_.C = (sh < WORD_BITS) ? (a >> (sh-1)) & 1 : (a >> MSB_POS) & 1;
            result = (sh >= WORD_BITS) ? static_cast<uint32_t>(static_cast<int32_t>(a) >> MSB_POS)
                                       : static_cast<uint32_t>(static_cast<int32_t>(a) >> sh); break;
        default: { // 0x7: ROR — ARM DDI 0419E
            uint8_t raw = sh; sh = raw & 0x1F; // rotation mod WORD_BITS
            if (raw == 0) { result = a; }
            else if (sh == 0) { apsr_.C = (a >> MSB_POS) & 1; result = a; }
            else { apsr_.C = (a >> (sh-1)) & 1; result = (a >> sh) | (a << (WORD_BITS - sh)); }
        }
    }
    apsr_.update_nz(result); regs_[rd] = result;
}

// Arithmetic/compare ALU ops (ADC/SBC/NEG/CMP/CMN): op=5,6,9,A,B
void CortexM0::exec_alu_arith(uint8_t op, uint8_t rd, uint32_t a, uint32_t b) noexcept {
    switch (op) {
        case 0x5: { auto [res,c]=add32(a, b,apsr_.C); apsr_.C=c; apsr_.V=(((a^res)&(b^res))>>MSB_POS)&1; apsr_.update_nz(res); regs_[rd]=res; break; } // ADC
        case 0x6: { auto [res,c]=add32(a,~b,apsr_.C); apsr_.C=c; apsr_.V=(((a^res)&(~b^res))>>MSB_POS)&1; apsr_.update_nz(res); regs_[rd]=res; break; } // SBC
        case 0x9: { auto [res,c]=sub32(0, b); apsr_.C=c; apsr_.V=((b&res)>>MSB_POS)&1; apsr_.update_nz(res); regs_[rd]=res; break; } // NEG
        case 0xA: { auto [res,c]=sub32(a, b); apsr_.C=c; apsr_.V=(((a^b)&(a^res))>>MSB_POS)&1; apsr_.update_nz(res); break; } // CMP
        case 0xB: { auto [res,c]=add32(a, b); apsr_.C=c; apsr_.V=(((a^res)&(b^res))>>MSB_POS)&1; apsr_.update_nz(res); break; } // CMN
    }
}

// ─── Format 4: ALU operations (dispatcher) ───────────────────────────────────
// 010000_Op_Rs_Rd
void CortexM0::exec_alu(uint16_t instr) {
    const uint8_t  op = (instr >> 6) & 0xF;
    const uint8_t  rs = (instr >> 3) & 0x7;
    const uint8_t  rd = instr        & 0x7;
    const uint32_t a  = regs_[rd];
    const uint32_t b  = regs_[rs];

    if ((op >= 0x2 && op <= 0x4) || op == 0x7) { exec_alu_shift(op, rd, a, b); return; }
    if (op == 0x5 || op == 0x6 || (op >= 0x9 && op <= 0xB)) { exec_alu_arith(op, rd, a, b); return; }

    uint32_t result = 0;
    switch (op) {
        case 0x0: result = a & b;  break;            // AND
        case 0x1: result = a ^ b;  break;            // EOR
        case 0x8: apsr_.update_nz(a & b); return;   // TST — result not written back
        case 0xC: result = a | b;  break;            // ORR
        case 0xD: result = a * b;  break;            // MUL
        case 0xE: result = a & ~b; break;            // BIC
        case 0xF: result = ~b;     break;            // MVN
        default: return;
    }
    apsr_.update_nz(result); regs_[rd] = result;
}

// ─── Format 5: Hi register operations / BX ───────────────────────────────────
// 010001_Op_H1_H2_Rs_Rd
void CortexM0::exec_hi_reg_bx(uint16_t instr) {
    uint8_t op = (instr >> 8) & 0x3;
    uint8_t rs = ((instr >> 3) & 0xF);
    uint8_t rd = ((instr >> 7) & 0x1) << 3 | (instr & 0x7);
    uint32_t val_s = regs_[rs];
    uint32_t val_d = regs_[rd];

    switch (op) {
        case 0: regs_[rd] = val_d + val_s; break; // ADD (no flags)
        case 1: { // CMP
            auto [res, carry] = sub32(val_d, val_s);
            apsr_.C = carry;
            apsr_.V = (((val_d ^ val_s) & (val_d ^ res)) >> MSB_POS) & 1;
            apsr_.update_nz(res);
            break;
        }
        case 2: regs_[rd] = val_s; break; // MOV (no flags)
        case 3: // BX / BLX
            if (op == 3 && (instr >> 7 & 1)) { // BLX: store return address
                regs_[static_cast<int>(RegIndex::LR)] = pc() | 1;
            }
            set_pc(val_s & ~1u);
            if (!(val_s & 1)) halted_ = true; // returned to ARM mode = HALT in sim
            break;
    }
}

// ─── Format 6: PC-relative load ──────────────────────────────────────────────
// 01001_Rd_Word8
void CortexM0::exec_ldr_pc(uint16_t instr) {
    uint8_t  rd   = (instr >> 8) & 0x7;
    uint32_t off  = (instr & 0xFF) << 2;
    uint32_t base = (pc() + 2) & ~3u; // word-aligned PC
    regs_[rd] = mem_.read32(base + off);
}

// ─── Format 7: Load/store register offset ────────────────────────────────────
// 0101_L_B_0_Ro_Rb_Rd
void CortexM0::exec_ldr_str_reg(uint16_t instr) {
    bool     L   = (instr >> 11) & 1;
    bool     B   = (instr >> 10) & 1;
    uint8_t  ro  = (instr >> 6)  & 0x7;
    uint8_t  rb  = (instr >> 3)  & 0x7;
    uint8_t  rd  = instr         & 0x7;
    uint32_t addr = regs_[rb] + regs_[ro];

    if (L) regs_[rd] = B ? mem_.read8(addr) : mem_.read32(addr);
    else   B ? mem_.write8(addr, regs_[rd] & 0xFF) : mem_.write32(addr, regs_[rd]);
}

// ─── Format 9: Load/store word/byte imm5 ─────────────────────────────────────
// 011_B_L_Off5_Rb_Rd
void CortexM0::exec_ldr_str_imm(uint16_t instr) {
    bool     B    = (instr >> 12) & 1;
    bool     L    = (instr >> 11) & 1;
    uint8_t  off5 = (instr >> 6)  & 0x1F;
    uint8_t  rb   = (instr >> 3)  & 0x7;
    uint8_t  rd   = instr         & 0x7;
    uint32_t off  = B ? off5 : (off5 << 2);
    uint32_t addr = regs_[rb] + off;

    if (L) regs_[rd] = B ? mem_.read8(addr) : mem_.read32(addr);
    else   B ? mem_.write8(addr, regs_[rd] & 0xFF) : mem_.write32(addr, regs_[rd]);
}

// ─── Format 10: LDRH/STRH imm5 ───────────────────────────────────────────────
// 1000_L_Off5_Rb_Rd
void CortexM0::exec_ldrh_strh_imm(uint16_t instr) {
    bool     L    = (instr >> 11) & 1;
    uint8_t  off5 = (instr >> 6)  & 0x1F;
    uint8_t  rb   = (instr >> 3)  & 0x7;
    uint8_t  rd   = instr         & 0x7;
    uint32_t addr = regs_[rb] + (off5 << 1);

    if (L) regs_[rd] = mem_.read16(addr);
    else   mem_.write16(addr, regs_[rd] & 0xFFFF);
}

// ─── Format 11: SP-relative LDR/STR ─────────────────────────────────────────
// 1001_L_Rd_Word8
void CortexM0::exec_ldr_str_sp(uint16_t instr) {
    bool     L   = (instr >> 11) & 1;
    uint8_t  rd  = (instr >> 8)  & 0x7;
    uint32_t off = (instr & 0xFF) << 2;
    uint32_t addr = regs_[13] + off;

    if (L) regs_[rd] = mem_.read32(addr);
    else   mem_.write32(addr, regs_[rd]);
}

// ─── Format 12: ADD Rd, SP/PC, #imm8 ────────────────────────────────────────
// 1010_SP_Rd_Word8
void CortexM0::exec_add_sp_pc(uint16_t instr) {
    bool     sp  = (instr >> 11) & 1;
    uint8_t  rd  = (instr >> 8)  & 0x7;
    uint32_t imm = (instr & 0xFF) << 2;
    uint32_t base = sp ? regs_[13] : ((pc() + 2) & ~3u);
    regs_[rd] = base + imm;
}

// ─── Format 13: ADD/SUB SP ────────────────────────────────────────────────────
// 10110000_S_Word7
void CortexM0::exec_sp_ops(uint16_t instr) {
    bool     sub = (instr >> 7) & 1;
    uint32_t imm = (instr & 0x7F) << 2;
    regs_[13] = sub ? regs_[13] - imm : regs_[13] + imm;
}

// ─── Format 14: PUSH/POP ────────────────────────────────────────────────────
// 1011_L_10_R_Rlist
void CortexM0::exec_push_pop(uint16_t instr) {
    bool    L     = (instr >> 11) & 1;
    bool    R     = (instr >> 8)  & 1;
    uint8_t rlist = static_cast<uint8_t>(instr & 0xFFU);

    if (!L) { // PUSH (descending)
        if (R) { regs_[13] -= 4; mem_.write32(regs_[13], regs_[14]); } // LR
        for (int i = 7; i >= 0; --i) {
            if (rlist & (1 << i)) { regs_[13] -= 4; mem_.write32(regs_[13], regs_[static_cast<size_t>(i)]); }
        }
    } else { // POP (ascending)
        for (int i = 0; i <= 7; ++i) {
            if (rlist & (1 << i)) { regs_[static_cast<size_t>(i)] = mem_.read32(regs_[13]); regs_[13] += 4; }
        }
        if (R) { uint32_t new_pc = mem_.read32(regs_[13]); regs_[13] += 4; set_pc(new_pc & ~1u);
                 if (!(new_pc & 1)) halted_ = true; }
    }
}

// ─── Format 15: LDM/STM ──────────────────────────────────────────────────────
// 1100_L_Rb_Rlist
void CortexM0::exec_ldm_stm(uint16_t instr) {
    bool     L     = (instr >> 11) & 1;
    uint8_t  rb    = (instr >> 8)  & 0x7;
    uint8_t  rlist = static_cast<uint8_t>(instr & 0xFFU);
    uint32_t addr  = regs_[rb];

    for (int i = 0; i <= 7; ++i) {
        if (rlist & (1 << i)) {
            if (L) regs_[static_cast<size_t>(i)] = mem_.read32(addr);
            else   mem_.write32(addr, regs_[static_cast<size_t>(i)]);
            addr += 4;
        }
    }
    if (!(rlist & (1u << rb)) || L) regs_[rb] = addr; // writeback
}

// ─── Format 16: Conditional branch ───────────────────────────────────────────
// 1101_Cond_SOffset8
void CortexM0::exec_branch_cond(uint16_t instr) {
    uint8_t cond    = (instr >> 8) & 0xF;
    int32_t soff8   = static_cast<int8_t>(instr & 0xFF);
    uint32_t target = pc() + 2 + static_cast<uint32_t>(soff8 << 1);
    if (condition_passes(cond)) set_pc(target);
}

// ─── Format 18: Unconditional branch ─────────────────────────────────────────
// 11100_Offset11
void CortexM0::exec_branch(uint16_t instr) {
    int32_t  off11  = static_cast<int32_t>((instr & 0x7FF) << BL_SIGN_SHIFT) >> 20; // sign extend
    uint32_t target = pc() + 2 + static_cast<uint32_t>(off11);
    set_pc(target);
}

// ─── Format 19: BL (two-instruction, 22-bit offset) ──────────────────────────
void CortexM0::exec_bl_upper(uint16_t instr) {
    int32_t off = static_cast<int32_t>((instr & 0x7FF) << BL_SIGN_SHIFT) >> 9; // upper 11 bits, signed
    bl_offset_ = static_cast<uint32_t>(static_cast<int32_t>(pc()) + 2 + off);
}

void CortexM0::exec_bl_lower(uint16_t instr) {
    uint32_t off_lo = (instr & 0x7FF) << 1;
    regs_[14] = pc() | 1u; // LR = next instr + Thumb bit
    set_pc(bl_offset_ + off_lo);
}
