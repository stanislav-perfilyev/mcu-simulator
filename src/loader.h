#pragma once
#include "memory.h"
#include <string>
#include <cstdint>
#include <stdexcept>

struct LoaderError : std::runtime_error {
    explicit LoaderError(const std::string& msg) : std::runtime_error(msg) {}
};

class Loader {
public:
    // Load a raw binary file starting at given address
    [[nodiscard]] static uint32_t load_binary(IMemoryBus& mem,
                                               const std::string& path,
                                               uint32_t load_addr = 0x0000);

    // Load Intel HEX file (.hex); returns entry point from :04 record or 0
    [[nodiscard]] static uint32_t load_ihex(IMemoryBus& mem,
                                             const std::string& path);

private:
    [[nodiscard]] static uint8_t parse_hex_byte(const char* s);
    static void    verify_checksum(const std::string& line);
};
