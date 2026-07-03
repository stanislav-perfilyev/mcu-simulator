#include "cpu.h"
#include "memory.h"
#include "loader.h"
#include "trace.h"
#include <iostream>
#include <string>
#include <optional>
#include <cstdlib>

struct Config {
    std::string binary_path;
    std::string ihex_path;
    bool        trace   = false;
    uint64_t    steps   = 100'000;
    uint32_t    load_addr = 0x0000;
};

static void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [options]\n"
              << "  --binary <file>     Load raw binary\n"
              << "  --hex    <file>     Load Intel HEX file\n"
              << "  --addr   <hex>      Load address for binary (default: 0x0000)\n"
              << "  --trace             Print every instruction\n"
              << "  --steps  <n>        Max steps (default: 100000)\n";
}

static void dump_registers(const CortexM0& cpu) {
    const char* names[] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc"
    };
    std::cout << "\n── Registers ────────────────────────────────\n";
    for (int i = 0; i < 16; i += 4) {
        for (int j = 0; j < 4; ++j) {
            std::printf("  %-4s = 0x%08X", names[i+j],
                        cpu.reg(static_cast<RegIndex>(i+j)));
        }
        std::puts("");
    }
    APSR f = cpu.apsr();
    std::printf("  APSR: N=%d Z=%d C=%d V=%d\n", f.N, f.Z, f.C, f.V);
    std::printf("  Cycles: %llu\n", static_cast<unsigned long long>(cpu.cycle_count()));
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return EXIT_FAILURE; }

    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--binary" && i+1 < argc) cfg.binary_path = argv[++i];
        else if (arg == "--hex"    && i+1 < argc) cfg.ihex_path   = argv[++i];
        else if (arg == "--addr"   && i+1 < argc)
            cfg.load_addr = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 16));
        else if (arg == "--trace")  cfg.trace  = true;
        else if (arg == "--steps"  && i+1 < argc) cfg.steps = std::stoull(argv[++i]);
        else if (arg == "--help")   { print_usage(argv[0]); return EXIT_SUCCESS; }
        else { std::cerr << "Unknown option: " << arg << "\n"; return EXIT_FAILURE; }
    }

    if (cfg.binary_path.empty() && cfg.ihex_path.empty()) {
        std::cerr << "Error: provide --binary or --hex\n";
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    FlatMemoryBus mem;
    CortexM0      cpu(mem);

    try {
        uint32_t entry = 0;
        if (!cfg.binary_path.empty()) {
            entry = Loader::load_binary(mem, cfg.binary_path, cfg.load_addr);
            std::printf("Loaded binary '%s' at 0x%04X\n",
                        cfg.binary_path.c_str(), cfg.load_addr);
        } else {
            entry = Loader::load_ihex(mem, cfg.ihex_path);
            std::printf("Loaded HEX '%s', entry=0x%08X\n",
                        cfg.ihex_path.c_str(), entry);
        }

        cpu.set_reg(RegIndex::PC, entry & ~1u);

        std::optional<Tracer> tracer;
        if (cfg.trace) {
            std::cout << "\n── Trace ─────────────────────────────────────\n";
            tracer.emplace(std::cout);
            tracer->attach(cpu);
        }

        uint64_t executed = cpu.run(cfg.steps);
        std::printf("\nDone: %llu steps executed.\n",
                    static_cast<unsigned long long>(executed));

        dump_registers(cpu);

    } catch (const BusFaultException& e) {
        std::cerr << "\n[FAULT] " << e.what() << "\n";
        dump_registers(cpu);
        return EXIT_FAILURE;
    } catch (const SimulatorError& e) {
        std::cerr << "\n[SIM ERROR] " << e.what() << "\n";
        dump_registers(cpu);
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
