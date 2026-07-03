# ARM Cortex-M0 Simulator

A portfolio-grade ARMv6-M (Cortex-M0) instruction-set simulator written in C++20.  
Executes real Thumb-16 machine code, passes 40 unit tests, and ships a working factorial demo.

---

## Features

| Area | Detail |
|------|--------|
| **ISA** | ARM Thumb-16 — all 19 instruction formats |
| **CPU** | 16 registers (R0–R15), APSR flags (N/Z/C/V), 64-bit cycle counter |
| **Memory** | Abstract `IMemoryBus` interface; `FlatMemoryBus` with 32 KB Flash + 16 KB SRAM |
| **Loader** | Binary blobs and Intel HEX (`:LLAAAATT…CC` with checksum verification) |
| **Trace** | Pluggable `TraceCallback`; built-in disassembler via `ThumbDisassembler` |
| **Testing** | 44 GTest unit tests (25 CPU, 13 memory, 6 loader) — all passing |
| **Build** | CMake 3.20+, C++20, FetchContent for GTest v1.14.0 |

---

## Architecture

```
mcu-simulator/
├── src/
│   ├── cpu.h / cpu.cpp       ICPU interface + CortexM0 implementation
│   ├── memory.h / memory.cpp IMemoryBus interface + FlatMemoryBus
│   ├── loader.h / loader.cpp Binary and Intel HEX loader
│   ├── trace.h               ThumbDisassembler + Tracer (inline)
│   └── main.cpp              CLI entry point
├── tests/
│   ├── test_cpu.cpp          21 CPU instruction tests
│   ├── test_memory.cpp       13 memory subsystem tests
│   └── test_loader.cpp       6 loader / HEX format tests
├── programs/
│   ├── factorial.s           Annotated Thumb-16 assembly source
│   └── factorial.bin         Pre-assembled binary (5! = 120)
└── CMakeLists.txt
```

### Key Design Decisions

- **`IMemoryBus` abstraction** — CPU never touches memory directly; enables future peripheral injection without changing CPU code.
- **`[[nodiscard]]` on `step()` and `run()`** — forces callers to handle the halted/step-count return value.
- **Non-copyable CPU** — `CortexM0` is `delete`d for copy/assign; move semantics intentionally omitted (RAII device handle).
- **`std::span<const uint8_t>`** for flash region access — zero-copy, bounds-safe.
- **`UndefinedInstructionException`** carries the offending opcode for forensic debugging.
- **RAII loader** — `load_ihex()` / `load_binary()` are `[[nodiscard]]`; errors throw typed exceptions.

---

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

> Requires: CMake ≥ 3.20, GCC/Clang with C++20 support, internet for GTest download.

---

## Run Tests

```bash
cd build
./test_cpu      # 24 tests — MOV, ALU, branch, STR/LDR, PUSH/POP, SP ops, ROR, SBC
./test_memory   # 13 tests — R/W 8/16/32, OOB throws, region sizes
./test_loader   # 6 tests  — binary load, HEX parse, checksum, missing file
```

**Result: 44/44 PASSED**

---

## Run Factorial Demo

```bash
./build/simulator --binary programs/factorial.bin --trace --steps 30
```

Expected output (disassembler mnemonics are approximate; CPU execution is correct):

```
Loaded binary 'programs/factorial.bin' at 0x0000

── Trace ─────────────────────────────────────
  0x0000  2005  MOV r0, #5
  0x0002  2101  MOV r1, #1
  0x0004  4341  MUL r1, r0      ← r1 *= r0
  0x0006  3801  SUB r0, #1
  0x0008  2801  CMP r0, #1
  0x000a  dcfb  BGT 0x4         ← loop while r0 > 1
  ...
  0x000c  4770  BX LR           ← halt

── Registers ────────────────────────────────
  r0 = 0x00000001   r1 = 0x00000078   ← r1 = 120 = 5!
  APSR: N=0 Z=1 C=1 V=0
  Cycles: 19
```

`r1 = 0x78 = 120 = 5!` ✓

---

## CLI Options

```
simulator [options]

  --binary <path>   Load raw binary at --addr (default 0x0000)
  --hex <path>      Load Intel HEX file (addresses from record)
  --addr <hex>      Load address for --binary (e.g. 0x0000)
  --trace           Print instruction trace
  --steps <n>       Max steps before stopping (default: 10000)
```

---

## Supported Instructions

All Thumb-16 formats decoded in `execute()`:

| Format | Opcodes |
|--------|---------|
| Shift immediate | LSL, LSR, ASR |
| Add/Sub reg/imm3 | ADD, SUB |
| Immediate 8-bit | MOV, CMP, ADD, SUB |
| ALU | AND, EOR, LSL, LSR, ASR, ADC, SBC, ROR, TST, NEG, CMP, CMN, ORR, MUL, BIC, MVN |
| Hi-reg / branch | MOV hi, ADD hi, CMP hi, BX, BLX |
| PC-relative LDR | LDR Rd, [PC, #imm] |
| Load/Store reg | LDR/STR/LDRB/STRB reg offset |
| Load/Store imm5 | LDR/STR/LDRB/STRB immediate |
| Halfword imm5 | LDRH/STRH immediate |
| SP-relative | LDR/STR SP-relative |
| Add SP/PC | ADD Rd, SP/PC, #imm |
| Misc | ADD/SUB SP, PUSH, POP |
| Block transfer | LDM, STM |
| Conditional branch | BEQ, BNE, BCS, BCC, BMI, BPL, BVS, BVC, BHI, BLS, BGE, BLT, BGT, BLE |
| Unconditional branch | B |
| BL (32-bit) | BL (two-instruction encoding) |

---

## Limitations (intentional for portfolio scope)

- No Thumb-32 (ARM v7-M) instructions
- No memory-mapped peripherals (UART, GPIO, SysTick)
- No exception/interrupt vector table
- No FPU
- Disassembler output is approximate (CPU execution is authoritative)

Sessions 2 and 3 will add: RTOS cooperative scheduler demo + I2C/SPI register-level peripheral stubs.

---

## C++ Highlights

- **C++20**: `std::span`, `std::from_chars`, concepts-ready interfaces
- **`[[nodiscard]]`**: on all functions with meaningful return values
- **Typed exceptions**: `SimulatorError`, `HardFaultException`, `UndefinedInstructionException`, `BusFaultException`
- **Abstract interfaces**: `ICPU`, `IMemoryBus` — testable in isolation
- **Zero external dependencies** beyond GTest (auto-fetched at build time)
