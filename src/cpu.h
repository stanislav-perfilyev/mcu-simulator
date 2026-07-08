#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <array>
#include <memory>

// ─── Exceptions ──────────────────────────────────────────────────────────────

struct SimulatorError : std::runtime_error {
    explicit SimulatorError(const std::string& msg) : std::runtime_error(msg) {}
};

struct HardFaultException : SimulatorError {
    explicit HardFaultException(const std::string& msg)
        : SimulatorError("HardFault: " + msg) {}
};

struct UndefinedInstructionException : SimulatorError {
    uint16_t opcode;
    explicit UndefinedInstructionException(uint16_t op)
        : SimulatorError("Undefined instruction: 0x" + toHex(op)), opcode(op) {}
    [[nodiscard]] static std::string toHex(uint16_t v);
};

// ─── APSR flags ──────────────────────────────────────────────────────────────

struct APSR {
    bool N = false; // Negative
    bool Z = false; // Zero
    bool C = false; // Carry
    bool V = false; // Overflow

    void update_nz(uint32_t result) noexcept {
        N = (result >> 31) & 1;
        Z = (result == 0);
    }
};

// ─── Register file ───────────────────────────────────────────────────────────

enum class RegIndex : uint8_t {
    R0=0, R1, R2, R3, R4, R5, R6, R7,
    R8, R9, R10, R11, R12,
    SP=13, LR=14, PC=15
};

// ─── Abstract CPU interface ───────────────────────────────────────────────────

class IMemoryBus; // forward declaration

class ICPU {
public:
    virtual ~ICPU() = default;

    [[nodiscard]] virtual uint32_t  reg(RegIndex r) const noexcept = 0;
    virtual void                    set_reg(RegIndex r, uint32_t v) noexcept = 0;
    [[nodiscard]] virtual APSR      apsr() const noexcept = 0;
    [[nodiscard]] virtual uint64_t  cycle_count() const noexcept = 0;

    // Fetch → decode → execute one instruction. Returns false when simulation ends.
    [[nodiscard]] virtual bool step() = 0;

    // Run up to max_steps. Returns number of steps executed.
    [[nodiscard]] virtual uint64_t run(uint64_t max_steps = UINT64_MAX) = 0;

    virtual void reset() noexcept = 0;
};

// ─── ARM Cortex-M0 implementation ────────────────────────────────────────────

class CortexM0 final : public ICPU {
public:
    static constexpr uint32_t RESET_SP = 0x2000;
    static constexpr uint32_t RESET_PC = 0x0001; // Thumb bit set

    explicit CortexM0(IMemoryBus& mem) noexcept;
    ~CortexM0() override = default;

    // Non-copyable; move-constructible but not move-assignable
    // (IMemoryBus& member cannot be rebound after construction)
    CortexM0(const CortexM0&)            = delete;
    CortexM0& operator=(const CortexM0&) = delete;
    CortexM0(CortexM0&&)                 = default;
    CortexM0& operator=(CortexM0&&)      = delete;

    [[nodiscard]] uint32_t  reg(RegIndex r)  const noexcept override;
    void                    set_reg(RegIndex r, uint32_t v) noexcept override;
    [[nodiscard]] APSR      apsr()           const noexcept override { return apsr_; }
    [[nodiscard]] uint64_t  cycle_count()    const noexcept override { return cycles_; }
    [[nodiscard]] bool      step()           override;
    [[nodiscard]] uint64_t  run(uint64_t max_steps = UINT64_MAX) override;
    void                    reset() noexcept override;

    // Trace support
    using TraceCallback = void(*)(uint32_t pc, uint16_t opcode, const char* disasm);
    void set_trace(TraceCallback cb) noexcept { trace_cb_ = cb; }

private:
    std::array<uint32_t, 16> regs_{};
    APSR                     apsr_{};
    uint64_t                 cycles_ = 0;
    IMemoryBus&              mem_;
    TraceCallback            trace_cb_ = nullptr;
    bool                     halted_ = false;

    // Helpers
    [[nodiscard]] uint32_t pc()  const noexcept { return regs_[15]; }
    [[nodiscard]] uint32_t sp()  const noexcept { return regs_[13]; }
    void set_pc(uint32_t v) noexcept  { regs_[15] = v & ~1u; } // clear Thumb bit
    void set_sp(uint32_t v) noexcept  { regs_[13] = v; }
    void advance_pc() noexcept        { regs_[15] += 2; }

    [[nodiscard]] uint16_t fetch();
    void execute(uint16_t instr);

    // Instruction groups (Thumb-16 formats)
    void exec_shift_imm(uint16_t instr);       // Format 1: LSL/LSR/ASR imm
    void exec_add_sub(uint16_t instr);          // Format 2: ADD/SUB reg/imm3
    void exec_imm8(uint16_t instr);             // Format 3: MOV/CMP/ADD/SUB imm8
    void exec_alu(uint16_t instr);              // Format 4: ALU operations
    void exec_hi_reg_bx(uint16_t instr);        // Format 5: Hi regs / BX
    void exec_ldr_pc(uint16_t instr);           // Format 6: LDR PC-relative
    void exec_ldr_str_reg(uint16_t instr);      // Format 7: LDR/STR reg offset
    void exec_ldr_str_imm(uint16_t instr);      // Format 9: LDR/STR imm5
    void exec_ldrh_strh_imm(uint16_t instr);    // Format 10: LDRH/STRH imm5
    void exec_ldr_str_sp(uint16_t instr);       // Format 11: LDR/STR SP-relative
    void exec_add_sp_pc(uint16_t instr);        // Format 12: ADD Rd, SP/PC, #imm8
    void exec_sp_ops(uint16_t instr);           // Format 13: ADD/SUB SP, #imm7
    void exec_push_pop(uint16_t instr);         // Format 14: PUSH/POP
    void exec_ldm_stm(uint16_t instr);          // Format 15: LDM/STM
    void exec_branch_cond(uint16_t instr);      // Format 16: B<cond>
    void exec_branch(uint16_t instr);           // Format 18: B
    void exec_bl_upper(uint16_t instr);         // Format 19a: BL upper
    void exec_bl_lower(uint16_t instr);         // Format 19b: BL lower

    // Flag helpers
    [[nodiscard]] static std::pair<uint32_t,bool> add32(uint32_t a, uint32_t b, bool cin = false);
    [[nodiscard]] static std::pair<uint32_t,bool> sub32(uint32_t a, uint32_t b);
    [[nodiscard]] bool condition_passes(uint8_t cond) co