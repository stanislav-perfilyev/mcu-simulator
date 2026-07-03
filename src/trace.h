#pragma once
#include <iostream>
#include "cpu.h"
#include <string>
#include <cstdint>
#include <ostream>

// ─── Thumb-16 disassembler ────────────────────────────────────────────────────
//
// Returns a human-readable mnemonic for a Thumb-16 instruction.
// Used by the --trace CLI flag and by tests to verify decoding.

class ThumbDisassembler {
public:
    [[nodiscard]] static std::string disassemble(uint32_t pc, uint16_t instr);

private:
    static std::string fmt_reg(uint8_t r);
    static std::string fmt_reglist(uint8_t rlist);
    static std::string cond_name(uint8_t cond);
};

// ─── Trace printer ────────────────────────────────────────────────────────────
//
// Call attach() once to wire up the CPU's trace callback.

class Tracer {
public:
    explicit Tracer(std::ostream& out = std::cout) noexcept;

    void attach(CortexM0& cpu) noexcept;

    // Direct print (used internally by the callback)
    void print(uint32_t pc, uint16_t opcode, const std::string& disasm);

private:
    std::ostream& out_;

    // Static trampoline for the C function pointer
    static void trace_cb(uint32_t pc, uint16_t opcode, const char* disasm);
    static Tracer* active_; // last attached instance
};

// ─── Inline implementation ────────────────────────────────────────────────────

#include <iostream>
#include <sstream>
#include <iomanip>

inline std::string ThumbDisassembler::fmt_reg(uint8_t r) {
    static const char* names[] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc"
    };
    return (r < 16) ? names[r] : "?";
}

inline std::string ThumbDisassembler::fmt_reglist(uint8_t rlist) {
    std::string s = "{";
    bool first = true;
    for (int i = 0; i < 8; ++i) {
        if (rlist & (1 << i)) {
            if (!first) s += ",";
            s += fmt_reg(static_cast<uint8_t>(i));
            first = false;
        }
    }
    return s + "}";
}

inline std::string ThumbDisassembler::cond_name(uint8_t c) {
    static const char* conds[] = {
        "eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","al","nv"
    };
    return (c < 16) ? conds[c] : "??";
}

inline std::string ThumbDisassembler::disassemble(uint32_t pc, uint16_t instr) {
    std::ostringstream os;
    os << std::hex << std::uppercase;

    uint8_t top5 = static_cast<uint8_t>((instr >> 11) & 0x1FU);
    uint8_t top4 = static_cast<uint8_t>((instr >> 12) & 0x0FU);
    uint8_t top6 = static_cast<uint8_t>((instr >> 10) & 0x3FU);

    auto R = [](int n){ return ThumbDisassembler::fmt_reg(static_cast<uint8_t>(n)); };

    // Shift immediate (formats 0-2: LSL/LSR/ASR imm5, top5 = 0x00..0x02)
    if (top5 <= 0x02) {
        static const char* ops[] = {"lsl","lsr","asr"};
        uint8_t op = (instr >> 11) & 0x3;
        os << ops[op < 3 ? op : 0] << " " << R(instr & 7)
           << ", " << R((instr>>3)&7) << ", #" << ((instr>>6)&0x1F);
        return os.str();
    }
    // Add/Sub (format 3: top5 = 0x03)
    if (top5 == 0x03) {
        bool sub = (instr>>9) & 1; bool imm = (instr>>10) & 1;
        os << (sub ? "sub" : "add") << " " << R(instr&7)
           << ", " << R((instr>>3)&7) << ", ";
        if (imm) os << "#" << ((instr>>6)&7);
        else     os << R((instr>>6)&7);
        return os.str();
    }
    // Imm8 (formats 4-7: MOV/CMP/ADD/SUB imm8, top5 = 0x04..0x07)
    if (top5 >= 0x04 && top5 <= 0x07) {
        static const char* ops[] = {"mov","cmp","add","sub"};
        os << ops[(instr>>11)&3] << " " << R((instr>>8)&7) << ", #" << (instr&0xFF);
        return os.str();
    }
    // ALU
    if (top6 == 0x10) {
        static const char* ops[] = {
            "and","eor","lsl","lsr","asr","adc","sbc","ror",
            "tst","neg","cmp","cmn","orr","mul","bic","mvn"
        };
        uint8_t op = (instr>>6)&0xF;
        os << ops[op] << " " << R(instr&7) << ", " << R((instr>>3)&7);
        return os.str();
    }
    // Hi reg / BX
    if (top6 == 0x11) {
        static const char* ops[] = {"add","cmp","mov","bx"};
        uint8_t op = (instr>>8)&3;
        if (op == 3) os << "bx " << R((instr>>3)&0xF);
        else os << ops[op] << " " << R(((instr>>7)&1)<<3|(instr&7)) << ", " << R((instr>>3)&0xF);
        return os.str();
    }
    // PUSH/POP
    if (top4 == 0xB) {
        bool L = (instr>>11)&1;
        bool R_flag = (instr>>8)&1;
        os << (L ? "pop" : "push") << " " << fmt_reglist(static_cast<uint8_t>(instr & 0xFFU));
        if (!L && R_flag) { os << (instr&0xFF ? "," : ""); os << "{lr}"; }
        if ( L && R_flag) { os << (instr&0xFF ? "," : ""); os << "{pc}"; }
        return os.str();
    }
    // Branch cond
    if (top4 == 0xD && ((instr>>8)&0xF) != 0xF) {
        uint8_t cond = (instr>>8)&0xF;
        int32_t off  = static_cast<int8_t>(instr&0xFF) << 1;
        os << "b" << cond_name(cond) << " 0x" << static_cast<uint32_t>(static_cast<int32_t>(pc) + 4 + off);
        return os.str();
    }
    // Branch
    if (top5 == 0x1C) {
        int32_t off = static_cast<int32_t>((instr & 0x7FF) << 21) >> 20;
        os << "b 0x" << static_cast<uint32_t>(static_cast<int32_t>(pc) + 4 + off);
        return os.str();
    }
    // BL
    if (top5 == 0x1E) { os << "bl_h 0x" << (instr & 0x7FF); return os.str(); }
    if (top5 == 0x1F) { os << "bl_l 0x" << (instr & 0x7FF); return os.str(); }
    // LDR/STR imm
    if (top4 >= 0x6 && top4 <= 0x8) {
        bool B = top4 == 0x7; bool L = (instr>>11)&1;
        os << (L ? "ldr" : "str") << (B ? "b" : "") << " "
           << R(instr&7) << ", [" << R((instr>>3)&7) << ", #" << (((instr>>6)&0x1F)<<(B?0:2)) << "]";
        return os.str();
    }
    // SP-relative
    if (top4 == 0x9) {
        bool L = (instr>>11)&1;
        os << (L ? "ldr" : "str") << " " << R((instr>>8)&7) << ", [sp, #" << ((instr&0xFF)<<2) << "]";
        return os.str();
    }
    // LDM/STM
    if (top4 == 0xC) {
        bool L = (instr>>11)&1;
        os << (L ? "ldm" : "stm") << " " << R((instr>>8)&7) << "!, " << fmt_reglist(static_cast<uint8_t>(instr & 0xFFU));
        return os.str();
    }
    // SP ops
    if ((instr>>8) == 0xB0) {
        os << ((instr>>7)&1 ? "sub" : "add") << " sp, #" << ((instr&0x7F)<<2);
        return os.str();
    }

    os << "?? 0x" << std::setw(4) << std::setfill('0') << instr;
    return os.str();
}

inline Tracer* Tracer::active_ = nullptr;

inline Tracer::Tracer(std::ostream& out) noexcept : out_(out) {}

inline void Tracer::attach(CortexM0& cpu) noexcept {
    active_ = this;
    cpu.set_trace(&Tracer::trace_cb);
}

inline void Tracer::print(uint32_t pc, uint16_t opcode, const std::string& disasm) {
    out_ << "  0x" << std::hex << std::setw(4) << std::setfill('0') << pc
         << "  " << std::setw(4) << opcode
         << "  " << disasm << "\n";
}

inline void Tracer::trace_cb(uint32_t pc, uint16_t opcode, const char* /*unused*/) {
    if (active_) {
        auto dis = ThumbDisassembler::disassemble(pc, opcode);
        active_->print(pc, opcode, dis);
    }
}
